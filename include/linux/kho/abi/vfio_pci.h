/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2026, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 * David Matlack <dmatlack@google.com>
 */

#ifndef _LINUX_LIVEUPDATE_ABI_VFIO_PCI_H
#define _LINUX_LIVEUPDATE_ABI_VFIO_PCI_H

#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/types.h>

/**
 * DOC: VFIO PCI Live Update ABI
 *
 * VFIO uses the ABI defined below for preserving device files across a kexec
 * reboot using LUO.
 *
 * Device metadata, virtualized PCI configuration space, VFIO's capability map,
 * and the initial PCI saved state captured when the device was first opened are
 * serialized into memory which is then handed to the next kernel via KHO.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in the VFIO_PCI_LUO_FH_COMPATIBLE string.
 */

#define VFIO_PCI_LUO_FH_COMPATIBLE "vfio-pci-v1"

/**
 * define VFIO_PCI_SER_RESET_WORKS - Device supports function reset.
 *
 * Set in &vfio_pci_core_device_ser.flags when the preserved device had
 * ``vdev->reset_works == true`` prior to kexec.
 */
#define VFIO_PCI_SER_RESET_WORKS	BIT(0)

#define VFIO_PCI_SER_KNOWN_FLAGS	VFIO_PCI_SER_RESET_WORKS

/**
 * struct vfio_pci_core_device_ser - Serialized state of a single VFIO PCI
 * device.
 *
 * @domain: The device's PCI domain number (segment).
 * @bdf: The device's PCI bus, device, and function number.
 * @cfg_size: Size in bytes of the device's PCI configuration space
 *            (``PCI_CFG_SPACE_SIZE`` or ``PCI_CFG_SPACE_EXP_SIZE``).
 * @size: Total size in bytes of this serialized buffer, equal to
 *        ``sizeof(struct vfio_pci_core_device_ser) + 2 * @cfg_size + @saved_state_size``.
 * @flags: Bitmask of ``VFIO_PCI_SER_*`` flags; unknown bits are rejected.
 * @saved_state_size: Size in bytes of the serialized &struct pci_saved_state_ser
 *                    blob stored in @data after ``config_map``, or 0 if none.
 * @reserved: Reserved for future use; must be 0.
 * @data: Variable-length payload containing, in order:
 *        1. ``u8 vconfig[@cfg_size]`` — VFIO virtualized config space snapshot
 *           captured during ``freeze()``.
 *        2. ``u8 config_map[@cfg_size]`` — VFIO capability map snapshot
 *           captured during ``freeze()``.
 *        3. ``u8 saved_state[@saved_state_size]`` — serialized
 *           &struct pci_saved_state_ser captured during ``preserve()``.
 */
struct vfio_pci_core_device_ser {
	u32 domain;
	u16 bdf;
	u16 cfg_size;
	u32 size;
	u32 flags;
	u32 saved_state_size;
	u32 reserved;
	u8 data[];
} __packed;

#endif /* _LINUX_LIVEUPDATE_ABI_VFIO_PCI_H */
