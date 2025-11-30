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

static void unpreserve_iommu_context_table(struct intel_iommu *iommu, int end)
{
	struct context_entry *context;
	int i;

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

static int preserve_iommu_context_table(struct intel_iommu *iommu)
{
	struct context_entry *context;
	int ret;
	int i;

	for (i = 0; i < ROOT_ENTRY_NR; i++) {
		/*
		 * Alloc the context tables now to make sure the iommu unit is
		 * properly preserved. These might stay unused and wastes around
		 * 32MB max in scalable mode.
		 */
		spin_lock(&iommu->lock);
		context = iommu_context_addr(iommu, i, 0, 1);
		spin_unlock(&iommu->lock);
		if (!context) {
			ret = -ENOMEM;
			goto error;
		}
		ret = iommu_preserve_page(context);
		if (ret)
			goto error;

		if (!sm_supported(iommu))
			continue;

		spin_lock(&iommu->lock);
		context = iommu_context_addr(iommu, i, 0x80, 1);
		spin_unlock(&iommu->lock);
		if (!context) {
			ret = -ENOMEM;
			goto error_sm;
		}
		ret = iommu_preserve_page(context);
		if (ret)
			goto error_sm;
	}

	return 0;

error_sm:
	context = iommu_context_addr(iommu, i, 0, 0);
	iommu_unpreserve_page(context);
error:
	unpreserve_iommu_context_table(iommu, i);
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

	ret = preserve_iommu_context_table(iommu);
	if (ret)
		return ret;

	ret = iommu_preserve_page(iommu->root_entry);
	if (ret) {
		unpreserve_iommu_context_table(iommu, ROOT_ENTRY_NR);
		return ret;
	}

	ser->intel.phys_addr = iommu->reg_phys;
	ser->intel.root_table = __pa(iommu->root_entry);
	ser->type = IOMMU_INTEL;
	ser->token = ser->intel.phys_addr;

	return 0;
}

void intel_iommu_unpreserve(struct iommu_device *iommu_dev,
			    struct iommu_hw_ser *iommu_ser)
{
	struct intel_iommu *iommu;

	iommu = container_of(iommu_dev, struct intel_iommu, iommu);

	unpreserve_iommu_context_table(iommu, ROOT_ENTRY_NR);
	iommu_unpreserve_page(iommu->root_entry);
}
