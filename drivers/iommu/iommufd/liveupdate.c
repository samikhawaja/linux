// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

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

static u32 iopt_inc_num_preserved_domains(struct io_pagetable *iopt)
{
	guard(rwsem_write)(&iopt->domains_rwsem);
	return ++iopt->num_preserved_domains;
}

static void iopt_dec_num_preserved_domains(struct io_pagetable *iopt)
{
	guard(rwsem_write)(&iopt->domains_rwsem);
	if (!WARN_ON(!iopt->num_preserved_domains))
		iopt->num_preserved_domains--;
}

int iommufd_hwpt_liveupdate_mark_preserve(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_liveupdate_mark_preserve *cmd = ucmd->cmd;
	struct iommufd_ctx *ictx = ucmd->ictx;
	struct iommufd_hwpt_paging *hwpt;
	struct iommufd_object *obj;
	unsigned long index;
	bool marked = false;
	int rc = 0;

	hwpt = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt))
		return PTR_ERR(hwpt);

	mutex_lock(&ictx->liveupdate_mutex);

	xa_lock(&ictx->objects);

	/* PRI use cases are not supported. */
	if (hwpt->common.fault) {
		rc = -EOPNOTSUPP;
		goto out_unlock;
	}

	xa_for_each_marked(&ictx->objects, index, obj, IOMMUFD_OBJ_LIVEUPDATE_MARK) {
		struct iommufd_hwpt_paging *hwpt_marked;

		if (WARN_ON_ONCE(obj->type != IOMMUFD_OBJ_HWPT_PAGING))
			continue;

		hwpt_marked = to_hwpt_paging(container_of(obj, struct iommufd_hw_pagetable, obj));

		if (hwpt_marked == hwpt)
			marked = true;

		if (hwpt_marked->liveupdate_token == cmd->hwpt_token) {
			if (hwpt_marked == hwpt)
				goto out_unlock;

			rc = -EADDRINUSE;
			goto out_unlock;
		}
	}

	__xa_set_mark(&ictx->objects, hwpt->common.obj.id, IOMMUFD_OBJ_LIVEUPDATE_MARK);

	if (marked)
		pr_warn_ratelimited("Overwriting HWPT liveupdate token from: %llu to %llu\n",
				    hwpt->liveupdate_token, cmd->hwpt_token);
	hwpt->liveupdate_token = cmd->hwpt_token;

out_unlock:
	xa_unlock(&ictx->objects);
	mutex_unlock(&ictx->liveupdate_mutex);
	iommufd_put_object(ictx, &hwpt->common.obj);
	return rc;
}

static int iopt_area_validate_file_pages(struct iopt_area *area)
{
	unsigned long last_index = iopt_area_last_index(area);
	unsigned long index = iopt_area_index(area);
	unsigned int page_offset = area->page_offset;
	struct iopt_pages *pages = area->pages;
	unsigned long iova = iopt_area_iova(area);

	guard(mutex)(&pages->mutex);
	if (WARN_ON(!area->storage_domain))
		return -EINVAL;

	while (index <= last_index) {
		phys_addr_t phys = iommu_iova_to_phys(area->storage_domain,
						      iova) - page_offset;
		struct page *page = pfn_to_page(PHYS_PFN(phys));
		struct folio *folio = page_folio(page);
		unsigned long nr;

		if (folio->mapping != pages->file->f_mapping)
			return -EINVAL;

		nr = folio_nr_pages(folio) - folio_page_idx(folio, page);
		iova += nr * PAGE_SIZE - page_offset;
		page_offset = 0;
		index += nr;
	}

	return 0;
}

static int iopt_validate_preserved_memory(struct io_pagetable *iopt,
					  struct liveupdate_session *s)
{
	struct iopt_area *area;
	int ret = 0;

	down_read(&iopt->iova_rwsem);
	for (area = iopt_area_iter_first(iopt, 0, ULONG_MAX); area;
	     area = iopt_area_iter_next(area, 0, ULONG_MAX)) {
		struct iopt_pages *pages = area->pages;

		if (!pages)
			continue;

		/* Only allow file based mapping */
		if (pages->type != IOPT_ADDRESS_FILE) {
			ret = -EINVAL;
			break;
		}

		/* Make sure that the file was preserved. */
		ret = liveupdate_get_token_outgoing(s, pages->file, NULL);
		if (ret)
			break;

		/*
		 * Make sure the mapped pages are still associated with the
		 * preserved memfd.
		 */
		ret = iopt_area_validate_file_pages(area);
		if (ret)
			break;
	}
	up_read(&iopt->iova_rwsem);

	return ret;
}

static int iommufd_preserve_hwpt(struct iommufd_hwpt_paging *hwpt,
				 struct iommufd_hwpt_ser *hwpt_ser,
				 struct liveupdate_session *session)
{
	struct iommu_domain_ser *domain_ser;
	int rc;

	/*
	 * Make IOAS immutable so the DMA mappings do not change while
	 * the HWPT is preserved. Since one IOAS can have multiple
	 * HWPTs, if an error occurs this call needs to make the IOAS
	 * mutable again if it was the one that made it immutable.
	 */
	if (iopt_inc_num_preserved_domains(&hwpt->ioas->iopt) == 1) {
		rc = iopt_validate_preserved_memory(&hwpt->ioas->iopt, session);
		if (rc)
			goto err;
	}

	hwpt_ser->token = hwpt->liveupdate_token;

	rc = iommu_preserve_domain(hwpt->common.domain, &domain_ser);
	if (rc < 0)
		goto err;

	hwpt_ser->domain_ser_phys = virt_to_phys(domain_ser);
	return 0;

err:
	iopt_dec_num_preserved_domains(&hwpt->ioas->iopt);
	return rc;
}

static void _iommufd_unpreserve(struct iommufd_ctx *ictx,
				struct iommufd_ser *ser)
{
	struct iommufd_hwpt_paging *hwpt;
	struct iommufd_object *obj;
	unsigned long index;

	xa_lock(&ictx->objects);
	xa_for_each_marked(&ictx->objects, index, obj, IOMMUFD_OBJ_LIVEUPDATE_MARK) {
		if (obj->type != IOMMUFD_OBJ_HWPT_PAGING)
			continue;

		hwpt = to_hwpt_paging(container_of(obj, struct iommufd_hw_pagetable, obj));
		if (!hwpt->liveupdate_preserved)
			continue;

		xa_unlock(&ictx->objects);

		iommu_unpreserve_domain(hwpt->common.domain);
		iopt_dec_num_preserved_domains(&hwpt->ioas->iopt);

		hwpt->liveupdate_preserved = false;
		iommufd_put_object(ictx, obj);

		xa_lock(&ictx->objects);
	}
	xa_unlock(&ictx->objects);

	kho_unpreserve_free(ser);
}

static int iommufd_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct iommufd_ctx *ictx;
	struct iommufd_hwpt_paging *hwpt;
	struct iommufd_ser *iommufd_ser;
	struct iommufd_object *obj;
	unsigned int nr_hwpts;
	unsigned long index;
	unsigned int i;
	void *mem;
	int rc;

	ictx = iommufd_ctx_from_file(args->file);
	if (IS_ERR(ictx))
		return PTR_ERR(ictx);

	mutex_lock(&ictx->liveupdate_mutex);

	/* Count the number of HWPTs to preserve */
	nr_hwpts = 0;
	xa_lock(&ictx->objects);
	xa_for_each_marked(&ictx->objects, index, obj, IOMMUFD_OBJ_LIVEUPDATE_MARK) {
		if (obj->type != IOMMUFD_OBJ_HWPT_PAGING)
			continue;

		hwpt = to_hwpt_paging(container_of(obj, struct iommufd_hw_pagetable, obj));
		if (!hwpt->common.domain) {
			rc = -EINVAL;
			xa_unlock(&ictx->objects);
			goto out_unlock;
		}
		nr_hwpts++;
	}
	xa_unlock(&ictx->objects);

	mem = kho_alloc_preserve(struct_size(iommufd_ser,
					     hwpt_array, nr_hwpts));
	if (IS_ERR(mem)) {
		rc = PTR_ERR(mem);
		goto out_unlock;
	}

	iommufd_ser = mem;

	/* Preserve HWPTs */
	i = 0;
	xa_lock(&ictx->objects);
	xa_for_each_marked(&ictx->objects, index, obj, IOMMUFD_OBJ_LIVEUPDATE_MARK) {
		if (obj->type != IOMMUFD_OBJ_HWPT_PAGING)
			continue;

		if (!iommufd_lock_obj(obj)) {
			rc = -ENOENT;
			xa_unlock(&ictx->objects);
			goto out_unpreserve;
		}

		/*
		 * HWPT is locked so it will not be destroyed. The xarray lock
		 * can be released here before preserving the HWPT.
		 */
		xa_unlock(&ictx->objects);
		hwpt = to_hwpt_paging(container_of(obj, struct iommufd_hw_pagetable, obj));
		rc = iommufd_preserve_hwpt(hwpt, &iommufd_ser->hwpt_array[i++], args->session);
		if (rc) {
			iommufd_put_object(ictx, obj);
			goto out_unpreserve;
		}

		/*
		 * Mark the HWPT as successfully preserved. This is distinct
		 * from IOMMUFD_OBJ_LIVEUPDATE_MARK, which only indicates the
		 * userspace intent to preserve.
		 */
		hwpt->liveupdate_preserved = true;
		xa_lock(&ictx->objects);
	}
	xa_unlock(&ictx->objects);

	/* Store the actual number of HWPTs that are preserved */
	iommufd_ser->nr_hwpts = i;

	args->serialized_data = virt_to_phys(iommufd_ser);
	mutex_unlock(&ictx->liveupdate_mutex);
	iommufd_ctx_put(ictx);
	return 0;

out_unpreserve:
	_iommufd_unpreserve(ictx, iommufd_ser);
out_unlock:
	mutex_unlock(&ictx->liveupdate_mutex);
	iommufd_ctx_put(ictx);
	return rc;
}

static void iommufd_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
	struct iommufd_ctx *ictx;

	ictx = iommufd_ctx_from_file(args->file);
	if (WARN_ON(IS_ERR(ictx)))
		return;

	mutex_lock(&ictx->liveupdate_mutex);
	_iommufd_unpreserve(ictx, phys_to_virt(args->serialized_data));
	mutex_unlock(&ictx->liveupdate_mutex);

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
	.owner = THIS_MODULE,
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
		goto out;

	ret = iommu_liveupdate_register_flb(&iommufd_ser_handler);
	if (ret)
		liveupdate_unregister_file_handler(&iommufd_ser_handler);

out:
	if (ret == -EOPNOTSUPP)
		return 0;

	return ret;
}

void iommufd_liveupdate_unregister(void)
{
	iommu_liveupdate_unregister_flb(&iommufd_ser_handler);
	liveupdate_unregister_file_handler(&iommufd_ser_handler);
}
