/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2026, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#ifndef _LINUX_KHO_ABI_IOMMUFD_H
#define _LINUX_KHO_ABI_IOMMUFD_H

#include <linux/mutex_types.h>
#include <linux/compiler.h>
#include <linux/types.h>

/**
 * DOC: IOMMUFD Live Update ABI
 *
 * This header defines the ABI for preserving the state of an IOMMUFD file
 * across a kexec reboot using LUO.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in the IOMMUFD_LUO_COMPATIBLE string.
 */

#define IOMMUFD_LUO_COMPATIBLE "iommufd-v1"

/**
 * struct iommufd_hwpt_ser - IOMMUFD HWPT serialized state
 * @size: Size of this struct
 * @padding: Padding
 * @domain_ser_phys: Physical address of the serialized state of associated domain
 * @token: User provided token
 */
struct iommufd_hwpt_ser {
	u32 size;
	u32 padding;
	u64 domain_ser_phys;
	u64 token;
} __packed;

/**
 * struct iommufd_ser - IOMMUFD serialized state
 * @size: Size of this struct
 * @features: Bitmask of iommufd specific features used by the preserved state
 * @nr_hwpts: Number of preserved HWPTs
 * @hwpt_array: Array of serialized state of preserved HWPTs
 */
struct iommufd_ser {
	u32 size;
	u32 features;
	u64 nr_hwpts;
	struct iommufd_hwpt_ser hwpt_array[];
} __packed;

#endif /* _LINUX_KHO_ABI_IOMMUFD_H */
