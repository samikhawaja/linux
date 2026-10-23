/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

#ifndef _LINUX_KHO_ABI_PCI_H
#define _LINUX_KHO_ABI_PCI_H

#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/stddef.h>
#include <linux/types.h>

/**
 * DOC: PCI File-Lifecycle Bound (FLB) Live Update ABI
 *
 * The PCI core preserves its state across kexec as Live Update
 * File-Lifecycle Bound (FLB) data, identified by the compatible string
 * ``"pci"`` and laid out as described by the structs below.
 *
 * This layout is a contract between kernels. Rather than being versioned, it
 * is extended according to the following rules:
 *
 *  * Existing fields must never be moved, resized, removed, or have their
 *    meaning changed. New fields may only be appended to the end of a struct.
 *
 *  * A bit set in ``pci_ser.required_flags`` marks a feature that the next
 *    kernel must understand to safely take over the preserved devices. The
 *    next kernel panics if it finds a bit it does not know how to handle.
 *
 *  * A bit set in ``pci_ser.optional_flags`` marks a feature that the next
 *    kernel can safely ignore if it does not understand it.
 *
 *  * Fields appended to struct pci_ser must be accompanied by a flag that
 *    indicates they are present, since the size of struct pci_ser is not
 *    recorded.
 *
 *  * The size of each struct pci_dev_ser is recorded in
 *    ``pci_ser.dev_ser_size`` and is never smaller than
 *    ``PCI_DEV_SER_MIN_SIZE``. The next kernel ignores any fields beyond the
 *    ones it knows about, and must handle entries that are smaller than its
 *    own struct pci_dev_ser.
 */

#define PCI_LUO_FLB_COMPATIBLE "pci"

/**
 * struct pci_dev_ser - Serialized state about a single PCI device.
 *
 * @domain: The device's PCI domain number (segment).
 * @bdf: The device's PCI bus, device, and function number.
 * @refcount: Reference count used by the PCI core to keep track of whether it
 *            is done using a device's struct pci_dev_ser. The value of the
 *            reference count is equal to the number of preserved devices at or
 *            below it in the PCI hierarchy when the struct pci_dev_ser is in
 *            use, and 0 otherwise.
 */
struct pci_dev_ser {
	u32 domain;
	u16 bdf;
	u16 refcount;
} __packed;

/*
 * The size of struct pci_dev_ser when this ABI was introduced. Since fields
 * can only be appended, every struct pci_dev_ser is at least this large.
 */
#define PCI_DEV_SER_MIN_SIZE offsetofend(struct pci_dev_ser, refcount)

/**
 * struct pci_ser - PCI Subsystem Live Update State
 *
 * This struct tracks state about all devices that are being preserved across
 * a Live Update for the next kernel. It contains only state owned by the PCI
 * core; the state a driver needs to resume its device is preserved separately
 * by that driver.
 *
 * @required_flags: Features that the next kernel must support to take over
 *                  the preserved devices. No flags are currently defined.
 * @optional_flags: Features that the next kernel may ignore if it does not
 *                  support them. No flags are currently defined.
 * @dev_ser_size: The size in bytes of each struct pci_dev_ser in @devices.
 * @nr_devices: The number of devices that were preserved.
 * @devices: Physical address of the first KHO block containing pci_dev_ser.
 */
struct pci_ser {
	u64 required_flags;
	u64 optional_flags;
	u32 dev_ser_size;
	u32 nr_devices;
	u64 devices;
} __packed;

/**
 * DOC: PCI Saved State Live Update ABI
 *
 * Drivers that keep a PCI device running across a Live Update can hand the
 * state returned by pci_store_saved_state() to the next kernel by embedding a
 * struct pci_saved_state_ser in their own preserved data, produced and parsed
 * by the PCI core via pci_liveupdate_serialize_saved_state() and
 * pci_liveupdate_deserialize_saved_state().
 *
 * Like the FLB data above, this layout is a contract between kernels and is
 * extended by appending fields and defining bits in ``flags`` for features the
 * next kernel must understand, while recording the size of each entry. Unlike
 * the FLB data, it is embedded in a driver's preserved state rather than
 * referenced from struct pci_ser, and validation failures are reported to the
 * calling driver rather than panicking.
 */

/**
 * struct pci_cap_saved_data_ser - A saved capability in a struct pci_saved_state_ser
 * @cap_nr: Capability ID (PCI_CAP_ID_* or PCI_EXT_CAP_ID_*).
 * @extended: 1 if @cap_nr is an extended capability ID, 0 otherwise.
 * @size: Size of @data in bytes, never 0.
 * @data: The saved registers.
 */
struct pci_cap_saved_data_ser {
	u16 cap_nr;
	u16 extended;
	u32 size;
	u8 data[];
} __packed;

/**
 * struct pci_saved_state_ser - Serialized struct pci_saved_state
 * @size: Size in bytes of this struct, including all entries in @caps.
 * @flags: Features a kernel must support to restore the state. None defined.
 * @nr_caps: Number of entries in @caps.
 * @config_space: The saved standard configuration header.
 * @caps: @nr_caps struct pci_cap_saved_data_ser entries.
 */
struct pci_saved_state_ser {
	u32 size;
	u32 flags;
	u32 nr_caps;
	u32 config_space[16];
	u8 caps[];
} __packed;

#endif /* _LINUX_KHO_ABI_PCI_H */
