/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2026, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#ifndef _LINUX_KHO_ABI_IOMMU_H
#define _LINUX_KHO_ABI_IOMMU_H

#include <linux/mutex_types.h>
#include <linux/compiler.h>
#include <linux/kho/abi/block.h>
#include <linux/types.h>

/**
 * DOC: IOMMU File-Lifecycle Bound (FLB) Live Update ABI
 *
 * This header defines the ABI for preserving IOMMU state across kexec using
 * Live Update File-Lifecycle Bound (FLB) data.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in IOMMU_LUO_FLB_VERSION.
 *
 * Memory Layout of Serialization Structures:
 * ==========================================
 *
 * Each serialized type (IOMMU, Domain, Device) is stored in a set of KHO
 * serialization blocks (see include/linux/kho/abi/block.h). Each block starts
 * with a struct kho_block_header_ser, followed by the serialized objects of
 * the respective type. Blocks are linked through the next field of the header.
 *
 * Top Level (struct iommu_flb_ser):
 * +---------------------------+
 * | - iommu_block_phys        |
 * | - domain_block_phys       |
 * | - device_block_phys       |
 * +---------------------------+
 *
 * For example see below the representation of the domain blocks.
 *
 * +---------------------------+   +---------------------------+
 * | kho_block_header_ser      |-->| kho_block_header_ser      |--> NULL
 * | - next                    |   | - next                    |
 * | - count                   |   | - count                   |
 * |                           |   |                           |
 * | [ iommu_domain_ser ]      |   | [ iommu_domain_ser ]      |
 * | [ iommu_domain_ser ]      |   | [ iommu_domain_ser ]      |
 * | ...                       |   | ...                       |
 * +---------------------------+   +---------------------------+
 *
 * Each object in a block starts with a common header (iommu_hdr_ser). The
 * size in the header is the size of each object in the block, so the next
 * kernel can read the objects even if their size has changed.
 * For example, the layout of struct iommu_domain_ser is:
 *
 *   +-----------------------------+
 *   | iommu_domain_ser            |
 *   | +-------------------------+ |
 *   | | hdr (iommu_hdr_ser)     | |
 *   | | - ref_count             | |
 *   | | - deleted / incoming    | |
 *   | +-------------------------+ |
 *   | - top_table_phys          | |
 *   | - top_level               | |
 *   | - restored_domain         | |
 *   +-----------------------------+
 *
 * This pattern applies identically to iommu_device_ser and iommu_hw_ser.
 */

#define IOMMU_LUO_FLB_COMPATIBLE "iommu-liveupdate"
#define IOMMU_LUO_FLB_VERSION 1

/**
 * enum iommu_type_ser - Type of the IOMMU being preserved
 * @IOMMU_SER_TYPE_INVALID: Invalid type of IOMMU
 * @IOMMU_SER_TYPE_INTEL: Intel VT-d IOMMU
 *
 * IOMMU type is stored in the IOMMU HW state to differentiate between various
 * IOMMU HWs.
 */
enum iommu_type_ser {
	IOMMU_SER_TYPE_INVALID,
	IOMMU_SER_TYPE_INTEL,
};

#define IOMMU_SER_FLAG_DELETED	(1 << 0)
#define IOMMU_SER_FLAG_INCOMING	(1 << 1)

/**
 * struct iommu_hdr_ser - Common header for all serialized IOMMU objects
 * @ref_count: Reference count for the object
 * @flags: Bitmask of IOMMU_SER_FLAG_* flags indicating object state
 * @size: Size of the serialized object
 * @features: Bitmask of object specific features used by the preserved state
 */
struct iommu_hdr_ser {
	u32 ref_count;
	u32 flags;
	u32 size;
	u32 features;
} __packed;

/**
 * struct iommu_domain_ser - Serialized state of an IOMMU domain
 * @hdr: Common object header
 * @top_table_phys: Physical address of the top-level page table
 * @top_level: Level of the top-level page table
 * @max_vasz_lg2: Maximum number of bits the VA can contain
 * @sign_extend: FEAT_SIGN_EXTEND is enabled for this domain
 * @restored_domain: Pointer to the restored domain (valid only after restore)
 */
struct iommu_domain_ser {
	struct iommu_hdr_ser hdr;
	u64 top_table_phys;
	u64 top_level;
	u32 max_vasz_lg2;
	u8 sign_extend;
	u8 padding[3];
	struct iommu_domain *restored_domain;
} __packed;

/**
 * struct iommu_dev_map_ser - Serialized mapping between device and domain.
 * @attachment_id: ID of the attachment between device and domain.
 * @domain_phys: Physical address of the domain
 */
struct iommu_dev_map_ser {
	u64 attachment_id;
	u64 domain_phys;
} __packed;

/**
 * struct iommu_device_intel_ser - Intel specific state of serialized device
 * @features: Bitmask of Intel device specific features
 * @restored: Whether the device state is restored
 * @pasid_table: Physical address of pasid table
 * @max_pasid: Maximum supported pasid
 */
struct iommu_device_intel_ser {
	u32 features;
	u8 restored;
	u8 padding[3];
	u64 pasid_table;
	u64 max_pasid;
} __packed;

/**
 * struct iommu_device_ser - Serialized state of a device
 * @hdr: Common object header
 * @devid: Device ID
 * @pci_domain_nr: PCI domain number
 * @dma_owner_token: Token to identify the DMA owner of this device
 * @iommu_phys: Physical address of the serialized IOMMU the device is behind
 * @attachment: Attachment of the device to its domain
 * @reserved: Reserved for future common fields.
 * @intel: Intel specific serialization data
 *
 * The union of IOMMU specific serialization data must be the last member, so
 * adding a new IOMMU type only grows the struct at the end.
 */
struct iommu_device_ser {
	struct iommu_hdr_ser hdr;
	u32 devid;
	u32 pci_domain_nr;
	u64 dma_owner_token;
	u64 iommu_phys;
	struct iommu_dev_map_ser attachment;
	u64 reserved[2];
	union {
		struct iommu_device_intel_ser intel;
	};
} __packed;

/* There are maximum 256 buses, so maximum 512 context tables */
#define VTD_PRESERVED_BITMAP_LONGS  DIV_ROUND_UP(512, BITS_PER_LONG_LONG)

/**
 * struct iommu_intel_ser - Serialized state of an Intel IOMMU instance
 * @features: Bitmask of Intel IOMMU specific features
 * @sm_enabled: Whether the IOMMU was used in scalable mode
 * @phys_addr: Physical address of the IOMMU register base
 * @root_table: Physical address of the root entry table
 * @context_tables_bitmap: Bitmap representing the context tables that are
 * preserved.
 */
struct iommu_intel_ser {
	u32 features;
	u8 sm_enabled;
	u8 padding[3];
	u64 phys_addr;
	u64 root_table;
	u64 context_tables_bitmap[VTD_PRESERVED_BITMAP_LONGS];
} __packed;

/**
 * struct iommu_hw_ser - Serialized state of an IOMMU instance
 * @hdr: Common object header
 * @token: Unique token to identify the IOMMU instance
 * @type: IOMMU type serialized state belongs to
 * @reserved: Reserved for future common fields.
 * @intel: Intel specific serialization data
 *
 * The union of IOMMU specific serialization data must be the last member, so
 * adding a new IOMMU type only grows the struct at the end.
 */
struct iommu_hw_ser {
	struct iommu_hdr_ser hdr;
	u64 token;
	u64 type;
	u64 reserved[2];
	union {
		struct iommu_intel_ser intel;
	};
} __packed;

/**
 * struct iommu_flb_ser - Top-level serialization structure
 * @iommu_block_phys: Physical address of the first KHO block of IOMMU HWs, or
 *                    0 if none
 * @domain_block_phys: Physical address of the first KHO block of domains, or
 *                     0 if none
 * @device_block_phys: Physical address of the first KHO block of devices, or 0
 *                     if none
 */
struct iommu_flb_ser {
	u64 version;
	u64 iommu_block_phys;
	u64 domain_block_phys;
	u64 device_block_phys;
} __packed;

#endif /* _LINUX_KHO_ABI_IOMMU_H */
