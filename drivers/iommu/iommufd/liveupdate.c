// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/iommufd.h>
#include <linux/liveupdate.h>
#include <linux/iommu-liveupdate.h>
#include <linux/mm.h>
#include <linux/pci.h>

#include "iommufd_private.h"
#include "io_pagetable.h"

int iommufd_hwpt_liveupdate_mark_preserve(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_liveupdate_mark_preserve *cmd = ucmd->cmd;
	struct iommufd_hwpt_paging *hwpt_target;
	struct iommufd_ctx *ictx = ucmd->ictx;
	void *curr;
	int rc = 0;

	hwpt_target = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt_target))
		return PTR_ERR(hwpt_target);

	/*
	 * Use xa_cmpxchg to safely store only if the token is not already in
	 * use.
	 */
	curr = xa_cmpxchg(&ictx->liveupdate_tokens, cmd->hwpt_token, NULL,
			  hwpt_target, GFP_KERNEL);
	if (xa_is_err(curr)) {
		rc = xa_err(curr);
		goto err;
	} else if (curr) {
		rc = -EADDRINUSE;
		goto err;
	}

	hwpt_target->liveupdate_preserve = true;
	hwpt_target->liveupdate_token = cmd->hwpt_token;

	/*
	 * liveupdate_tokens xarray still holds a reference to the HWPT to make
	 * sure it is not destroyed.
	 */
	return 0;
err:
	iommufd_put_object(ictx, &hwpt_target->common.obj);
	return rc;
}

static void iommufd_set_ioas_mutable(struct iommufd_ctx *ictx)
{
	struct iommufd_object *obj;
	struct iommufd_ioas *ioas;
	unsigned long index;

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index, obj) {
		if (obj->type != IOMMUFD_OBJ_IOAS)
			continue;

		ioas = container_of(obj, struct iommufd_ioas, obj);

		/*
		 * Not taking any IOAS lock here. All writers take LUO
		 * session mutex, and this writer racing with readers is not
		 * really a problem.
		 */
		WRITE_ONCE(ioas->iopt.lu_map_immutable, false);
	}
	xa_unlock(&ictx->objects);
}

static int check_iopt_pages_preserved(struct liveupdate_session *s,
				      struct iommufd_hwpt_paging *hwpt)
{
	u32 req_seals = F_SEAL_SEAL | F_SEAL_GROW | F_SEAL_SHRINK;
	struct iopt_area *area;
	int ret;

	for (area = iopt_area_iter_first(&hwpt->ioas->iopt, 0, ULONG_MAX); area;
	     area = iopt_area_iter_next(area, 0, ULONG_MAX)) {
		struct iopt_pages *pages = area->pages;

		/* Only allow file based mapping */
		if (pages->type != IOPT_ADDRESS_FILE)
			return -EINVAL;

		/*
		 * When this memory file was mapped it should be sealed and seal
		 * should be sealed. This means that since mapping was done the
		 * memory file was not grown or shrink and the pages being used
		 * until now remain pinnned and preserved.
		 */
		if ((pages->seals & req_seals) != req_seals)
			return -EINVAL;

		/* Make sure that the file was preserved. */
		ret = liveupdate_get_token_outgoing(s, pages->file, NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static int iommufd_save_hwpt_array(struct iommufd_ctx *ictx,
			      struct iommufd_ser *iommufd_ser,
			      struct liveupdate_session *session)
{
	struct iommufd_hwpt_paging *hwpt, **hwpt_array = NULL;
	struct iommu_domain_ser *domain_ser;
	struct iommufd_hwpt_ser *hwpt_lu;
	struct iommufd_object *obj;
	unsigned int nr_hwpts = 0;
	unsigned long index;
	unsigned int i;
	int rc = 0;

	if (iommufd_ser) {
		hwpt_array = kcalloc(iommufd_ser->nr_hwpts, sizeof(*hwpt_array),
				GFP_KERNEL);
		if (!hwpt_array)
			return -ENOMEM;
	}

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index, obj) {
		if (obj->type != IOMMUFD_OBJ_HWPT_PAGING)
			continue;

		hwpt = container_of(obj, struct iommufd_hwpt_paging, common.obj);
		if (!hwpt->liveupdate_preserve)
			continue;

		if (hwpt->ioas) {
			/*
			 * Obtain exclusive access to the IOAS and IOPT while we
			 * set immutability
			 */
			mutex_lock(&hwpt->ioas->mutex);
			down_write(&hwpt->ioas->iopt.domains_rwsem);
			down_write(&hwpt->ioas->iopt.iova_rwsem);

			hwpt->ioas->iopt.lu_map_immutable = true;

			up_write(&hwpt->ioas->iopt.iova_rwsem);
			up_write(&hwpt->ioas->iopt.domains_rwsem);
			mutex_unlock(&hwpt->ioas->mutex);
		}

		if (!hwpt->common.domain) {
			rc = -EINVAL;
			xa_unlock(&ictx->objects);
			goto out;
		}

		if (!iommufd_ser) {
			rc = check_iopt_pages_preserved(session, hwpt);
			if (rc) {
				xa_unlock(&ictx->objects);
				goto out;
			}
		} else {
			hwpt_array[nr_hwpts] = hwpt;
			hwpt_lu = &iommufd_ser->hwpt_array[nr_hwpts];

			hwpt_lu->token = hwpt->liveupdate_token;
			hwpt_lu->reclaimed = false;
		}

		nr_hwpts++;
	}
	xa_unlock(&ictx->objects);

	if (WARN_ON(iommufd_ser && iommufd_ser->nr_hwpts != nr_hwpts)) {
		rc = -EFAULT;
		goto out;
	}

	if (iommufd_ser) {
		/*
		 * iommu_domain_preserve may sleep and must be called
		 * outside of xa_lock
		 */
		for (i = 0; i < nr_hwpts; i++) {
			hwpt = hwpt_array[i];
			hwpt_lu = &iommufd_ser->hwpt_array[i];

			rc = iommu_domain_preserve(hwpt->common.domain, &domain_ser);
			if (rc < 0)
				goto out;

			hwpt_lu->domain_data = __pa(domain_ser);
		}
	}

	rc = nr_hwpts;

out:
	kfree(hwpt_array);
	return rc;
}

static int iommufd_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(args->file);
	struct iommufd_ser *iommufd_ser;
	size_t serial_size;
	void *mem;
	int rc;

	if (IS_ERR(ictx))
		return PTR_ERR(ictx);

	rc = iommufd_save_hwpt_array(ictx, NULL, args->session);
	if (rc < 0)
		goto err_ioas_mutable;

	serial_size = struct_size(iommufd_ser, hwpt_array, rc);

	mem = kho_alloc_preserve(serial_size);
	if (!mem) {
		rc = -ENOMEM;
		goto err_ioas_mutable;
	}

	iommufd_ser = mem;
	iommufd_ser->nr_hwpts = rc;
	rc = iommufd_save_hwpt_array(ictx, iommufd_ser, args->session);
	if (rc < 0)
		goto err_free;

	args->serialized_data = virt_to_phys(iommufd_ser);
	iommufd_ctx_put(ictx);
	return 0;

err_free:
	kho_unpreserve_free(mem);
err_ioas_mutable:
	iommufd_set_ioas_mutable(ictx);
	iommufd_ctx_put(ictx);
	return rc;
}

static void iommufd_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(args->file);
	struct iommufd_hwpt_paging *hwpt;
	struct iommufd_object *obj;
	unsigned long index;

	if (WARN_ON(IS_ERR(ictx)))
		return;

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index, obj) {
		if (obj->type != IOMMUFD_OBJ_HWPT_PAGING)
			continue;

		hwpt = container_of(obj, struct iommufd_hwpt_paging, common.obj);
		if (!hwpt->liveupdate_preserve)
			continue;
		if (!hwpt->common.domain)
			continue;

		iommu_domain_unpreserve(hwpt->common.domain);
	}
	xa_unlock(&ictx->objects);

	kho_unpreserve_free(phys_to_virt(args->serialized_data));

	iommufd_set_ioas_mutable(ictx);
	iommufd_ctx_put(ictx);
}

static int iommufd_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static bool iommufd_liveupdate_can_finish(struct liveupdate_file_op_args *args)
{
	return false;
}

static void iommufd_liveupdate_finish(struct liveupdate_file_op_args *args)
{
}

static bool iommufd_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					    struct file *file)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(file);

	if (IS_ERR(ictx))
		return false;

	iommufd_ctx_put(ictx);
	return true;
}

static struct liveupdate_file_ops iommufd_ser_file_ops = {
	.can_preserve = iommufd_liveupdate_can_preserve,
	.preserve = iommufd_liveupdate_preserve,
	.unpreserve = iommufd_liveupdate_unpreserve,
	.retrieve = iommufd_liveupdate_retrieve,
	.can_finish = iommufd_liveupdate_can_finish,
	.finish = iommufd_liveupdate_finish,
};

static struct liveupdate_file_handler iommufd_ser_handler = {
	.compatible = IOMMUFD_LUO_COMPATIBLE,
	.ops = &iommufd_ser_file_ops,
};

int iommufd_liveupdate_register(void)
{
	int ret;

	ret = liveupdate_register_file_handler(&iommufd_ser_handler);
	if (ret)
		return ret;

	ret = iommu_liveupdate_register_flb(&iommufd_ser_handler);
	if (ret)
		liveupdate_unregister_file_handler(&iommufd_ser_handler);

	return ret;
}

void iommufd_liveupdate_unregister(void)
{
	iommu_liveupdate_unregister_flb(&iommufd_ser_handler);
	liveupdate_unregister_file_handler(&iommufd_ser_handler);
}
