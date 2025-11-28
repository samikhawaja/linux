// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#define pr_fmt(fmt)    "iommu: liveupdate: " fmt

#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "iommu.h"
#include "../iommu-pages.h"

static void unpreserve_iommu_context(struct intel_iommu *iommu, int end)
{
	struct context_entry *context;
	int i;

	if (end < 0)
		end = ROOT_ENTRY_NR;

	for (i = 0; i < end; i++) {
		context = iommu_context_addr(iommu, i, 0, 0);
		if (context)
			iommu_unpreserve_page(context);

		if (!sm_supported(iommu))
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);
		if (context)
			iommu_unpreserve_page(context);
	}
}

static int preserve_iommu_context(struct intel_iommu *iommu)
{
	struct context_entry *context;
	int ret;
	int i;

	for (i = 0; i < ROOT_ENTRY_NR; i++) {
		context = iommu_context_addr(iommu, i, 0, 0);
		if (context) {
			ret = iommu_preserve_page(context);
			if (ret)
				goto error;
		}

		if (!sm_supported(iommu))
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);
		if (context) {
			ret = iommu_preserve_page(context);
			if (ret)
				goto error_sm;
		}
	}

	return 0;

error_sm:
	context = iommu_context_addr(iommu, i, 0, 0);
	iommu_unpreserve_page(context);
error:
	unpreserve_iommu_context(iommu, i);
	return ret;
}

static int restore_iommu_context(struct intel_iommu *iommu)
{
	struct context_entry *context;
	int i, ret = 0;

	for (i = 0; i < ROOT_ENTRY_NR; i++) {
		context = iommu_context_addr(iommu, i, 0, 0);
		if (context)
			BUG_ON(!kho_restore_folio(virt_to_phys(context)));

		if (!sm_supported(iommu))
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);
		if (context)
			BUG_ON(!kho_restore_folio(virt_to_phys(context)));
	}

	return ret;
}

static void restore_used_domain_ids(struct intel_iommu *iommu,
				    struct iommu_unit_ser *iommu_ser)
{
	unsigned int count;
	int id;

	for (count = 0; count < iommu_ser->count_domains; count++) {
		id = iommu_ser->domains[count].did;
		BUG_ON(ida_alloc_range(&iommu->domain_ida, id, id, GFP_KERNEL) < 0);
	}
}

int intel_iommu_get_preserved_domain_id(struct dmar_domain *domain,
					struct intel_iommu *iommu)
{
	struct iommu_domain_ser *domain_ser;
	struct iommu_unit_ser *iommu_ser;
	unsigned int count;
	int id;

	if (!iommu->iommu.preserved_state || !domain->domain.preserved_state)
		return -EINVAL;

	domain_ser = domain->domain.preserved_state;
	iommu_ser = phys_to_virt(iommu->iommu.preserved_state->data);
	for (count = 0; count < iommu_ser->count_domains; count++) {
		if (iommu_ser->domains[count].domain_idx != domain_ser->idx)
			continue;

		id = iommu_ser->domains[count].did;
		iommu_ser->domains[count].domain_idx = -1;
		return id;
	}

	BUG_ON(-EINVAL);
}

int intel_iommu_liveupdate_restore_root_table(struct intel_iommu *iommu,
					      struct iommu_device_ser *iommu_ser)
{
	struct iommu_unit_ser *iser = phys_to_virt(iommu_ser->data);
	int ret;

	if (!iommu_ser)
		return -ENOENT;

	iommu->root_entry = __va(iser->root_table);

	ret = restore_iommu_context(iommu);
	if (ret) {
		WARN_ONCE(ret, "Cannot restore iommu [%llx] root context\n", iommu->reg_phys);
		folio_put(virt_to_folio(iommu->root_entry));
		iommu->root_entry = NULL;
	}

	restore_used_domain_ids(iommu, iser);
	pr_info("Restored IOMMU[0x%llx] Root Table at: 0x%llx\n",
		iommu->reg_phys, iser->root_table);

	return ret;
}

static struct folio *folio_alloc_preserved(size_t sz)
{
	struct folio *folio;
	int ret;

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, get_order(sz));
	if (!folio)
		return ERR_PTR(-ENOMEM);

	ret = kho_preserve_folio(folio);
	if (ret) {
		folio_put(folio);
		folio = ERR_PTR(ret);
	}

	return folio;
}

static void preserved_folio_put(struct folio *folio)
{
	kho_unpreserve_folio(folio);
	folio_put(folio);
}

static int count_domain_ids(struct intel_iommu *iommu)
{
	unsigned int count = 0;
	int id;

	for (id = 0; id < cap_ndoms(iommu->cap); id++)
		if (ida_exists(&iommu->domain_ida, id))
			count++;

	return count;
}

static int preserve_iommu_domain_attachment(struct device_domain_info *info)
{
	struct iommu_unit_ser *iommu_ser;
	int idx;

	/* Do this in a lock */
	iommu_ser = phys_to_virt(info->iommu->iommu.preserved_state->data);
	if (iommu_ser->count_domains == iommu_ser->count_max_domains)
		return -ENOSPC;

	idx = iommu_ser->count_domains++;
	iommu_ser->domains[idx].did = domain_id_iommu(info->domain, info->iommu);
	iommu_ser->domains[idx].domain_idx = info->domain->domain.preserved_state->idx;

	return 0;
}

int intel_iommu_preserve_device(struct device *dev, struct device_ser *device_ser)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	if (!dev_is_pci(dev))
		return -ENOTSUPP;

	if (!info)
		return -EINVAL;

	return preserve_iommu_domain_attachment(info);
}

void intel_iommu_unpreserve_device(struct device *dev, struct device_ser *device_ser)
{
}

int intel_iommu_preserve(struct iommu_device *iommu_dev, struct iommu_device_ser *iommu_device_ser)
{
	struct iommu_unit_ser *ser;
	struct intel_iommu *iommu;
	struct folio *folio;
	int ret, count_dids;

	iommu = container_of(iommu_dev, struct intel_iommu, iommu);

	count_dids = count_domain_ids(iommu);
	folio = folio_alloc_preserved(sizeof(*ser) +
				      count_dids * sizeof(struct iommu_domain_attachments));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	ser = folio_address(folio);

	spin_lock(&iommu->lock);
	ret = preserve_iommu_context(iommu);
	if (ret)
		goto err;

	ret = iommu_preserve_page(iommu->root_entry);
	if (ret) {
		unpreserve_iommu_context(iommu, -1);
		goto err;
	}

	ser->phys_addr = iommu->reg_phys;
	ser->root_table = __pa(iommu->root_entry);
	ser->count_max_domains = count_dids;
	strncpy(iommu_device_ser->compatible, "intel", sizeof(iommu_device_ser->compatible));
	iommu_device_ser->token = iommu->reg_phys;
	iommu_device_ser->data = virt_to_phys(ser);
	spin_unlock(&iommu->lock);

	return 0;
err:
	preserved_folio_put(folio);
	spin_unlock(&iommu->lock);
	return ret;
}

void intel_iommu_unpreserve(struct iommu_device *iommu, struct iommu_device_ser *iommu_device_ser)
{
}


