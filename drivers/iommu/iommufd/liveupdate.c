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

	mutex_lock(&ictx->liveupdate_mutex);

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

	mutex_unlock(&ictx->liveupdate_mutex);

	/*
	 * liveupdate_tokens xarray still holds a reference to the HWPT to make
	 * sure it is not destroyed.
	 */
	return 0;
err:
	mutex_unlock(&ictx->liveupdate_mutex);
	iommufd_put_object(ictx, &hwpt_target->common.obj);
	return rc;
}

static int check_iopt_pages_preserved(struct liveupdate_session *s,
				      struct iommufd_hwpt_paging *hwpt)
{
	u32 req_seals = F_SEAL_SEAL | F_SEAL_GROW | F_SEAL_SHRINK;
	struct iopt_area *area;
	int ret = 0;

	down_read(&hwpt->ioas->iopt.iova_rwsem);
	for (area = iopt_area_iter_first(&hwpt->ioas->iopt, 0, ULONG_MAX); area;
	     area = iopt_area_iter_next(area, 0, ULONG_MAX)) {
		struct iopt_pages *pages = area->pages;

		/* Only allow file based mapping */
		if (pages->type != IOPT_ADDRESS_FILE) {
			ret = -EINVAL;
			break;
		}

		/*
		 * When this memory file was mapped it should be sealed and seal
		 * should be sealed. This means that since mapping was done the
		 * memory file was not grown or shrink and the pages being used
		 * until now remain pinnned and preserved.
		 */
		if ((pages->seals & req_seals) != req_seals) {
			ret = -EINVAL;
			break;
		}

		/* Make sure that the file was preserved. */
		ret = liveupdate_get_token_outgoing(s, pages->file, NULL);
		if (ret)
			break;
	}
	up_read(&hwpt->ioas->iopt.iova_rwsem);

	return ret;
}

static int iommufd_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(args->file);
	struct iommufd_hwpt_paging *hwpt, **hwpt_array = NULL;
	struct iommufd_ioas **ioas_array = NULL;
	struct iommufd_ser *iommufd_ser = NULL;
	struct iommu_domain_ser *domain_ser;
	struct iommufd_hwpt_ser *hwpt_ser;
	unsigned int nr_visited_ioas = 0;
	unsigned int nr_hwpts = 0;
	unsigned long index;
	size_t serial_size;
	bool already_visited;
	unsigned int i, j;
	void *mem = NULL;
	int rc = 0;

	if (IS_ERR(ictx))
		return PTR_ERR(ictx);

	mutex_lock(&ictx->liveupdate_mutex);

	/* Pass 1: Count */
	xa_for_each(&ictx->liveupdate_tokens, index, hwpt) {
		if (!hwpt->common.domain) {
			rc = -EINVAL;
			goto out_unlock;
		}
		nr_hwpts++;
	}

	serial_size = struct_size(iommufd_ser, hwpt_array, nr_hwpts);
	mem = kho_alloc_preserve(serial_size);
	if (!mem) {
		rc = -ENOMEM;
		goto out_unlock;
	}

	iommufd_ser = mem;
	iommufd_ser->nr_hwpts = nr_hwpts;

	hwpt_array = kcalloc(nr_hwpts, sizeof(*hwpt_array), GFP_KERNEL);
	ioas_array = kcalloc(nr_hwpts, sizeof(*ioas_array), GFP_KERNEL);
	if (!hwpt_array || !ioas_array) {
		rc = -ENOMEM;
		goto out_free_mem;
	}

	/* Pass 2: Gather */
	i = 0;
	xa_for_each(&ictx->liveupdate_tokens, index, hwpt) {
		hwpt_array[i++] = hwpt;
	}

	/* Pass 3: Validate and Serialize */
	for (i = 0; i < nr_hwpts; i++) {
		hwpt = hwpt_array[i];
		already_visited = false;

		if (hwpt->ioas) {
			for (j = 0; j < nr_visited_ioas; j++) {
				if (ioas_array[j] == hwpt->ioas) {
					already_visited = true;
					break;
				}
			}

			if (!already_visited) {
				mutex_lock(&hwpt->ioas->mutex);
				rc = check_iopt_pages_preserved(args->session, hwpt);
				mutex_unlock(&hwpt->ioas->mutex);
				if (rc)
					goto out_free_mem;

				ioas_array[nr_visited_ioas++] = hwpt->ioas;
			}
		}

		hwpt_ser = &iommufd_ser->hwpt_array[i];
		hwpt_ser->token = hwpt->liveupdate_token;
		hwpt_ser->reclaimed = false;

		rc = iommu_domain_preserve(hwpt->common.domain, &domain_ser);
		if (rc < 0)
			goto out_free_mem;

		hwpt_ser->domain_data = __pa(domain_ser);
	}

	args->serialized_data = virt_to_phys(iommufd_ser);
	kfree(hwpt_array);
	kfree(ioas_array);
	mutex_unlock(&ictx->liveupdate_mutex);
	iommufd_ctx_put(ictx);
	return 0;

out_free_mem:
	if (mem)
		kho_unpreserve_free(mem);
	kfree(hwpt_array);
	kfree(ioas_array);
out_unlock:
	mutex_unlock(&ictx->liveupdate_mutex);
	iommufd_ctx_put(ictx);
	return rc;
}

static void iommufd_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(args->file);
	struct iommufd_hwpt_paging *hwpt;
	unsigned long index;

	if (WARN_ON(IS_ERR(ictx)))
		return;

	mutex_lock(&ictx->liveupdate_mutex);
	xa_for_each(&ictx->liveupdate_tokens, index, hwpt) {
		if (!hwpt->common.domain)
			continue;

		iommu_domain_unpreserve(hwpt->common.domain);
	}
	mutex_unlock(&ictx->liveupdate_mutex);

	kho_unpreserve_free(phys_to_virt(args->serialized_data));

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
