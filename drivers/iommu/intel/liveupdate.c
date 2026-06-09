// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#define pr_fmt(fmt)    "DMAR: liveupdate: " fmt

#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/iommu-liveupdate.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "iommu.h"
#include "../iommu-pages.h"

static bool is_context_table_preserved(struct intel_iommu *iommu,
				       struct iommu_hw_ser *ser,
				       u8 bus, u8 devfn)
{
	u16 bit;

	/* 2 tables per bus in scalable mode */
	bit = (bus << 1);
	/* Upper table gets the odd bit */
	if (devfn & 0x80)
		bit++;

	return test_bit(bit, (unsigned long *)&ser->intel.context_tables_bitmap[0]);
}

static void set_context_table_preserved(struct intel_iommu *iommu,
					struct iommu_hw_ser *ser,
					u8 bus, u8 devfn)
{
	u16 bit;

	/* 2 tables per bus in scalable mode */
	bit = (bus << 1);
	/* Upper table gets the odd bit */
	if (devfn & 0x80)
		bit++;

	set_bit(bit, (unsigned long *)&ser->intel.context_tables_bitmap[0]);
}

static void unpreserve_iommu_context_tables(struct intel_iommu *iommu,
					    struct iommu_hw_ser *ser)
{
	struct context_entry *context;
	int i;

	for (i = 0; i < ROOT_ENTRY_NR; i++) {
		context = iommu_context_addr(iommu, i, 0, 0);
		if (context && is_context_table_preserved(iommu, ser, i, 0))
			iommu_unpreserve_pages(context);

		if (!sm_supported(iommu))
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);
		if (context && is_context_table_preserved(iommu, ser, i, 0x80))
			iommu_unpreserve_pages(context);
	}
}

static int preserve_iommu_context_tables(struct intel_iommu *iommu,
					 struct iommu_hw_ser *ser)
{
	struct context_entry *context;
	bool updating;
	int ret;
	int i;

	updating = !!iommu->iommu.outgoing_preserved_state;
	for (i = 0; i < ROOT_ENTRY_NR; i++) {
		spin_lock(&iommu->lock);
		context = iommu_context_addr(iommu, i, 0, 0);
		spin_unlock(&iommu->lock);
		if (context && !is_context_table_preserved(iommu, ser, i, 0)) {
			ret = iommu_preserve_pages(context);
			if (ret)
				goto error;

			set_context_table_preserved(iommu, ser, i, 0);
		}

		if (!sm_supported(iommu))
			continue;

		spin_lock(&iommu->lock);
		context = iommu_context_addr(iommu, i, 0x80, 0);
		spin_unlock(&iommu->lock);
		if (context && !is_context_table_preserved(iommu, ser, i, 0x80)) {
			ret = iommu_preserve_pages(context);
			if (ret)
				goto error;

			set_context_table_preserved(iommu, ser, i, 0x80);
		}
	}

	return 0;

error:
	if (!updating)
		unpreserve_iommu_context_tables(iommu, ser);

	return ret;
}

static void restore_iommu_context(struct intel_iommu *iommu)
{
	struct context_entry *context;
	int i;

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
}

static int _restore_used_domain_ids(struct iommu_device_ser *ser, void *arg)
{
	int id = ser->domain_iommu_ser.attachment_id;
	struct iommu_hw_ser *iommu_hw_ser;
	struct intel_iommu *iommu = arg;

	if (WARN_ON(!ser->domain_iommu_ser.iommu_phys))
		return -ENOENT;

	iommu_hw_ser = phys_to_virt(ser->domain_iommu_ser.iommu_phys);
	if (iommu_hw_ser->type != IOMMU_INTEL)
		return 0;

	/* Only allocate domain ID from associated IOMMU HW unit */
	if (iommu_hw_ser->intel.phys_addr != iommu->reg_phys)
		return 0;

	/*
	 * This can fail as multiple preserved devices can share the same domain
	 * ID. Since this is done during DMAR init so these failures can be
	 * ignored.
	 */
	ida_alloc_range(&iommu->domain_ida, id, id, GFP_ATOMIC);
	return 0;
}

void intel_iommu_liveupdate_restore_root_table(struct intel_iommu *iommu,
					       struct iommu_hw_ser *iommu_ser)
{
	if (!iommu_ser->intel.restored)
		BUG_ON(!kho_restore_folio(iommu_ser->intel.root_table));

	iommu->root_entry = __va(iommu_ser->intel.root_table);

	if (!iommu_ser->intel.restored)
		restore_iommu_context(iommu);

	iommu_ser->intel.restored = 1;
	iommu_for_each_preserved_device(_restore_used_domain_ids, iommu);
}

int intel_iommu_domain_reattach_iommu(struct dmar_domain *domain,
				      struct intel_iommu *iommu,
				      struct iommu_device_ser *device_ser)
{
	struct iommu_domain_info *info, *curr;
	int ret = -ENOSPC;
	int restored_did;

	if (domain->domain.type == IOMMU_DOMAIN_SVA)
		return 0;

	restored_did = device_ser->domain_iommu_ser.attachment_id;
	if (!ida_exists(&iommu->domain_ida, restored_did))
		return -EINVAL;

	info = kzalloc_obj(*info);
	if (!info)
		return -ENOMEM;

	guard(mutex)(&iommu->did_lock);
	curr = xa_load(&domain->iommu_array, iommu->seq_id);
	if (curr) {
		curr->refcnt++;
		WARN_ON_ONCE(curr->did != restored_did);
		kfree(info);
		return 0;
	}

	info->refcnt	= 1;
	info->did	= restored_did;
	info->iommu	= iommu;
	curr = xa_cmpxchg(&domain->iommu_array, iommu->seq_id,
			  NULL, info, GFP_KERNEL);
	if (curr) {
		ret = xa_err(curr) ? : -EBUSY;
		goto err_unlock;
	}

	return 0;

err_unlock:
	kfree(info);
	return ret;
}

int intel_iommu_preserve_device(struct device *dev,
				struct iommu_device_ser *device_ser)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	if (!dev_is_pci(dev)) {
		dev_err(dev, "Cannot preserve non-PCI device\n");
		return -EOPNOTSUPP;
	}

	if (!info)
		return -EINVAL;

	device_ser->domain_iommu_ser.attachment_id = domain_id_iommu(info->domain,
								     info->iommu);
	return 0;
}

int intel_iommu_preserve(struct iommu_device *iommu_dev,
			 struct iommu_hw_ser *ser)
{
	struct intel_iommu *iommu;
	int ret;

	iommu = container_of(iommu_dev, struct intel_iommu, iommu);

	ret = preserve_iommu_context_tables(iommu, ser);
	if (ret)
		return ret;

	ret = iommu_preserve_pages(iommu->root_entry);
	if (ret) {
		/* Only unpreserve if not updating */
		if (!iommu_dev->outgoing_preserved_state)
			unpreserve_iommu_context_tables(iommu, ser);

		return ret;
	}

	ser->intel.phys_addr = iommu->reg_phys;
	ser->intel.root_table = __pa(iommu->root_entry);
	ser->type = IOMMU_INTEL;
	ser->token = ser->intel.phys_addr;

	return 0;
}

void intel_iommu_unpreserve(struct iommu_device *iommu_dev,
			    struct iommu_hw_ser *ser)
{
	struct intel_iommu *iommu;

	iommu = container_of(iommu_dev, struct intel_iommu, iommu);

	unpreserve_iommu_context_tables(iommu, ser);
	iommu_unpreserve_pages(iommu->root_entry);
}
