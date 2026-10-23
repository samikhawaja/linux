// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 * David Matlack <dmatlack@google.com>
 */

/**
 * DOC: VFIO PCI Preservation via LUO
 *
 * VFIO PCI devices in noiommu mode can be preserved over a kexec using the Live
 * Update Orchestrator (LUO) file preservation while keeping the device running
 * and performing DMA across the kexec transition. This allows userspace (such
 * as a VMM or userspace driver) to transfer an in-use device to the next kernel
 * without resetting it.
 *
 * .. note::
 *    This support is enabled by ``CONFIG_VFIO_PCI_LIVEUPDATE`` and is limited
 *    to devices in noiommu mode (see Restrictions below).
 *
 * Usage Example
 * =============
 *
 * VFIO PCI devices can be preserved across a kexec by preserving the bound
 * character device file associated with the device in a LUO session::
 *
 *   device_fd = open("/dev/vfio/devices/noiommu-vfioX", O_RDWR);
 *   ioctl(device_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
 *   ...
 *   ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, { ..., device_fd, ...});
 *
 * .. note::
 *    LUO will hold an extra reference to the device file for as long as it is
 *    preserved, so there is no way for the file to be destroyed or the device
 *    to be unbound from the vfio-pci driver while it is preserved.
 *
 * Retrieving the file after kexec is not yet supported.
 *
 * Restrictions
 * ============
 *
 * The kernel imposes the following restrictions when preserving VFIO devices:
 *
 *  * The device must be bound to the ``vfio-pci`` driver (not a variant driver)
 *    and registered in **noiommu** mode (``vfio.enable_unsafe_noiommu_mode=1``
 *    with no IOMMU group attached to the device). Attempting to preserve an
 *    IOMMU-backed device fails with ``-EOPNOTSUPP``.
 *
 *  * The device file descriptor must be obtained by opening the VFIO character
 *    device (``/dev/vfio/devices/noiommu-vfioX``) and not via
 *    ``VFIO_GROUP_GET_DEVICE_FD``, and ``VFIO_DEVICE_BIND_IOMMUFD`` must have
 *    completed on that file descriptor before it is preserved.
 *
 *  * ``CONFIG_VFIO_PCI_ZDEV_KVM`` must not be enabled. This may be relaxed in
 *    the future.
 *
 *  * The device must not be an Intel display device or SR-IOV Virtual Function
 *    (VF). This may be relaxed in the future.
 *
 *  * The device must meet the following conditions prior to kexec, without
 *    which the ``reboot(LINUX_REBOOT_CMD_KEXEC)`` syscall (to initiate the
 *    kexec) will fail with ``-EINVAL`` and abort the Live Update:
 *
 *    - All VFIO interrupts on the device must be disabled.
 *    - The device must be in the ``PCI_D0`` power state and not in VFIO
 *      low-power mode (``VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY``).
 *    - SR-IOV must not be enabled on the device.
 *
 * In addition, the device must meet all of the restrictions imposed by the
 * core PCI layer documented at :doc:`/PCI/liveupdate`.
 *
 * Preservation Behavior
 * =====================
 *
 * During ``LIVEUPDATE_SESSION_PRESERVE_FD``, VFIO serializes the device's PCI
 * domain and BDF, its ``reset_works`` capability flag, and the initial
 * ``struct pci_saved_state`` captured when the device was first opened.
 *
 * During the ``reboot(LINUX_REBOOT_CMD_KEXEC)`` freeze phase, VFIO validates
 * that interrupts are disabled and the device is in ``PCI_D0``, snapshots the
 * virtualized PCI configuration space (``vconfig``) and capability map
 * (``pci_config_map``), masks physical INTx (`PCI_COMMAND_INTX_DISABLE`), and
 * marks the device frozen so subsequent userspace accesses return ``-EIO``.
 *
 * VFIO ioctls and config-space writes that race with the freeze phase are
 * serialized with it: those that could change the device or ``vconfig``
 * either complete before the snapshot is taken or fail with ``-EIO``
 * (``-EBUSY`` for ``VFIO_DEVICE_PCI_HOT_RESET``). Userspace should still
 * quiesce them before invoking ``reboot(LINUX_REBOOT_CMD_KEXEC)``. Existing
 * BAR mmaps, ioeventfds and dma-bufs keep working while the device is frozen
 * because the device keeps running.
 *
 * The device is **not** reset and ``PCI_COMMAND_MASTER`` is left untouched so
 * the device continues running and performing DMA across kexec. If the Live
 * Update is aborted after freeze (for example, if a later file fails to freeze),
 * the ``unfreeze()`` callback restores the physical INTx mask and clears the
 * frozen state, leaving the existing file descriptor immediately usable again.
 *
 * DMA Memory Ordering Contract
 * ============================
 *
 * Because a preserved noiommu device continues performing DMA using physical
 * addresses across kexec, any memory buffer targeted by device DMA must remain
 * at the same physical address in both kernels and must not be freed to the
 * page allocator while the device can still access it:
 *
 *  * When using a LUO-preserved ``memfd``, userspace **must** preserve the
 *    ``memfd`` in the **same** LUO session as the VFIO device and **before**
 *    preserving the VFIO device. LUO finishes files within a session in reverse
 *    preservation order (LIFO), guaranteeing that if the device is not taken
 *    over in the new kernel, VFIO's ``finish()`` callback stops bus mastering
 *    and resets the device before the ``memfd``'s ``finish()`` callback releases
 *    its folios. Cross-session ordering is the responsibility of userspace.
 *
 *  * After retrieving a preserved ``memfd`` in the new kernel, its folios are
 *    ordinary unpinned shmem page cache until pinned again; userspace must keep
 *    the ``memfd`` alive and re-pin/map it before finishing the session, and
 *    must not rely on a ``memfd`` whose ``LIVEUPDATE_SESSION_RETRIEVE_FD`` call
 *    failed (failed ``memfd`` retrieval frees its folios immediately).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cleanup.h>
#include <linux/errno.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/vfio_pci.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/pci_liveupdate.h>
#include <linux/string.h>
#include <linux/vfio.h>

#include "vfio_pci_priv.h"

static bool vfio_pci_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					     struct file *file)
{
	struct vfio_device *device = vfio_device_from_file(file);
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;

	if (!device)
		return false;

	if (device->ops != &vfio_pci_ops)
		return false;

	guard(mutex)(&device->dev_set->lock);

	/* Live Update support is limited to bound cdev files. */
	if (!vfio_file_is_bound_cdev(file))
		return false;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	/*
	 * Don't support specialized vfio-pci devices for now since they haven't
	 * been tested.
	 */
	if (IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM) || vfio_pci_is_intel_display(pdev))
		return false;

	/* Only non-VFs are supported for now. */
	if (pdev->is_virtfn)
		return false;

	return true;
}

static int vfio_pci_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	size_t saved_size = 0, total_size;
	struct pci_dev *pdev;
	u16 cfg_size;
	int err;

	if (!vfio_device_is_noiommu(device))
		return -EOPNOTSUPP;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;
	cfg_size = pdev->cfg_size;

	if (cfg_size != PCI_CFG_SPACE_SIZE && cfg_size != PCI_CFG_SPACE_EXP_SIZE)
		return -EINVAL;

	guard(mutex)(&device->dev_set->lock);

	if (!vfio_file_is_bound_cdev(args->file))
		return -EINVAL;

	if (vdev->pci_saved_state) {
		saved_size = pci_liveupdate_saved_state_ser_size(vdev->pci_saved_state);
		if (!saved_size)
			return -EINVAL;
	}

	total_size = size_add(sizeof(*ser),
			      size_add(size_mul(2, cfg_size), saved_size));
	if (total_size > U32_MAX)
		return -EOVERFLOW;

	ser = kho_alloc_preserve(total_size);
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	if (saved_size) {
		void *dst = ser->data + 2 * (size_t)cfg_size;

		err = pci_liveupdate_serialize_saved_state(vdev->pci_saved_state,
							   dst, saved_size);
		if (err) {
			kho_unpreserve_free(ser);
			return err;
		}
	}

	err = pci_liveupdate_preserve(pdev);
	if (err) {
		kho_unpreserve_free(ser);
		return err;
	}

	ser->domain = pci_domain_nr(pdev->bus);
	ser->bdf = pci_dev_id(pdev);
	ser->cfg_size = cfg_size;
	ser->size = total_size;
	ser->flags = vdev->reset_works ? VFIO_PCI_SER_RESET_WORKS : 0;
	ser->saved_state_size = saved_size;
	ser->reserved = 0;

	args->serialized_data = virt_to_phys(ser);
	return 0;
}

static void vfio_pci_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);

	pci_liveupdate_unpreserve(to_pci_dev(device->dev));
	kho_unpreserve_free(phys_to_virt(args->serialized_data));
}

static int vfio_pci_liveupdate_freeze(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device_ser *ser = phys_to_virt(args->serialized_data);
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;
	u16 cmd;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	guard(mutex)(&device->dev_set->lock);
	guard(mutex)(&vdev->igate);
	guard(rwsem_write)(&vdev->memory_lock);

	if (!vdev->vconfig || !vdev->pci_config_map ||
	    ser->cfg_size != pdev->cfg_size)
		return -EINVAL;

	/*
	 * Userspace must disable interrupts on the device prior to freeze so
	 * that the device does not send any interrupts until new interrupt
	 * handlers have been established by the next kernel.
	 */
	if (vdev->irq_type != VFIO_PCI_NUM_IRQS) {
		pci_err(pdev, "Freeze failed! Interrupts are still enabled.\n");
		return -EINVAL;
	}

	if (pdev->current_state != PCI_D0 || vdev->pm_runtime_engaged) {
		pci_err(pdev, "Freeze failed! Device not in D0 state.\n");
		return -EINVAL;
	}

	/*
	 * VFs are not preserved, and the next kernel disables SR-IOV on the
	 * device while it keeps running. vfio_pci_core_sriov_configure() sets
	 * sriov_active under memory_lock, and does not enable SR-IOV once the
	 * device is frozen.
	 */
	if (vdev->sriov_active) {
		pci_err(pdev, "Freeze failed! SR-IOV is enabled.\n");
		return -EINVAL;
	}

	memcpy(ser->data, vdev->vconfig, ser->cfg_size);
	memcpy(ser->data + ser->cfg_size, vdev->pci_config_map, ser->cfg_size);

	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	vdev->liveupdate_intx_disabled = !!(cmd & PCI_COMMAND_INTX_DISABLE);
	if (!vdev->liveupdate_intx_disabled)
		pci_intx(pdev, 0);

	/*
	 * Disable ioctl, read, write and mmap access to the device while it is
	 * frozen. If the Live Update is aborted, unfreeze() re-enables access.
	 */
	smp_store_release(&vdev->liveupdate_frozen, true);
	return 0;
}

static void vfio_pci_liveupdate_unfreeze(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct vfio_pci_core_device *vdev =
		container_of(device, struct vfio_pci_core_device, vdev);

	guard(mutex)(&device->dev_set->lock);
	guard(mutex)(&vdev->igate);
	guard(rwsem_write)(&vdev->memory_lock);

	if (!vdev->liveupdate_intx_disabled)
		pci_intx(vdev->pdev, 1);

	/* Pairs with smp_load_acquire() in vfio_pci_dev_is_frozen(). */
	smp_store_release(&vdev->liveupdate_frozen, false);
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static void vfio_pci_liveupdate_finish(struct liveupdate_file_op_args *args)
{
}

static const struct liveupdate_file_ops vfio_pci_liveupdate_file_ops = {
	.can_preserve = vfio_pci_liveupdate_can_preserve,
	.preserve = vfio_pci_liveupdate_preserve,
	.unpreserve = vfio_pci_liveupdate_unpreserve,
	.freeze = vfio_pci_liveupdate_freeze,
	.unfreeze = vfio_pci_liveupdate_unfreeze,
	.retrieve = vfio_pci_liveupdate_retrieve,
	.finish = vfio_pci_liveupdate_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler vfio_pci_liveupdate_fh = {
	.ops = &vfio_pci_liveupdate_file_ops,
	.compatible = VFIO_PCI_LUO_FH_COMPATIBLE,
};

int __init vfio_pci_liveupdate_init(void)
{
	int ret;

	ret = liveupdate_register_file_handler(&vfio_pci_liveupdate_fh);
	if (ret)
		return ret == -EOPNOTSUPP ? 0 : ret;

	ret = pci_liveupdate_register_flb(&vfio_pci_liveupdate_fh);
	if (ret)
		liveupdate_unregister_file_handler(&vfio_pci_liveupdate_fh);

	return ret;
}

void vfio_pci_liveupdate_cleanup(void)
{
	pci_liveupdate_unregister_flb(&vfio_pci_liveupdate_fh);
	liveupdate_unregister_file_handler(&vfio_pci_liveupdate_fh);
}
