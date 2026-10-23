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
 * After kexec, the preserved VFIO device file can be retrieved from the session
 * just like any other preserved file::
 *
 *   ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, &arg);
 *   device_fd = arg.fd;
 *   ...
 *   ioctl(session_fd, LIVEUPDATE_SESSION_FINISH, ...);
 *
 * .. note::
 *    After kexec, if a device was preserved by the previous kernel, attempting
 *    to open a new file for the device via its character device
 *    (``/dev/vfio/devices/X``) or via ``VFIO_GROUP_GET_DEVICE_FD`` will fail
 *    with ``-EBUSY``.
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
 * Retrieval and Finish Behavior After Kexec
 * =========================================
 *
 * After kexec, userspace must ensure the preserved PCI device is bound to
 * ``vfio-pci`` in noiommu mode before invoking
 * ``LIVEUPDATE_SESSION_RETRIEVE_FD`` (for example, via ``vfio-pci.ids=...`` on
 * the kernel command line). A failed ``LIVEUPDATE_SESSION_RETRIEVE_FD`` call is
 * cached permanently by LUO for that file.
 *
 * ``LIVEUPDATE_SESSION_RETRIEVE_FD`` validates the handed-over serialized state
 * and returns a new unbound VFIO cdev file descriptor. The device is claimed on
 * the first ``VFIO_DEVICE_BIND_IOMMUFD`` on that file descriptor.
 *
 * ``LIVEUPDATE_SESSION_FINISH`` (or closing a retrieved session file
 * descriptor) always completes:
 *
 *  * If the device was retrieved and claimed via ``VFIO_DEVICE_BIND_IOMMUFD``,
 *    ``finish()`` simply completes its Live Update participation.
 *  * If the device was not claimed (never retrieved, ``retrieve()`` failed, or
 *    retrieved but never bound via ``VFIO_DEVICE_BIND_IOMMUFD``), ``finish()``
 *    logs one ``pci_warn()`` naming the device, clears ``PCI_COMMAND_MASTER``,
 *    resets the function and restores the initial PCI saved state just as
 *    closing the device would, and finishes its Live Update participation so
 *    the device can be opened normally afterwards. If ``finish()`` cannot
 *    reset a device that has a function reset, for example because it cannot
 *    take the device lock while the device is being unbound from
 *    ``vfio-pci``, it still clears ``PCI_COMMAND_MASTER``, and the function
 *    reset is retried when the device is unbound, unless the device is opened
 *    (which resets it) before that.
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
 *    before the ``memfd``'s ``finish()`` callback releases its folios. It
 *    normally resets the device at that point too (see above for when it
 *    cannot). Cross-session ordering is the responsibility of userspace.
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
#include <linux/file.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/vfio_pci.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/pci_liveupdate.h>
#include <linux/slab.h>
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

static int vfio_pci_liveupdate_validate_ser(const struct vfio_pci_core_device_ser *ser)
{
	size_t expected;

	if (ser->reserved)
		return -EINVAL;

	if (ser->flags & ~VFIO_PCI_SER_KNOWN_FLAGS)
		return -EOPNOTSUPP;

	if (ser->cfg_size != PCI_CFG_SPACE_SIZE &&
	    ser->cfg_size != PCI_CFG_SPACE_EXP_SIZE)
		return -EINVAL;

	expected = size_add(sizeof(*ser),
			    size_add(size_mul(2, ser->cfg_size),
				     ser->saved_state_size));
	if (expected != ser->size)
		return -EINVAL;

	return 0;
}

static struct pci_saved_state *
vfio_pci_liveupdate_parse_saved_state(struct pci_dev *pdev,
				      const struct vfio_pci_core_device_ser *ser)
{
	const void *blob;

	if (!ser->saved_state_size)
		return NULL;

	blob = ser->data + 2 * (size_t)ser->cfg_size;
	return pci_liveupdate_deserialize_saved_state(pdev, blob,
						      ser->saved_state_size);
}

static int match_device(struct device *dev, const void *arg)
{
	struct vfio_device *device = container_of(dev, struct vfio_device, device);
	const struct vfio_pci_core_device_ser *ser = arg;
	struct pci_dev *pdev;

	if (device->ops != &vfio_pci_ops)
		return false;

	pdev = dev_is_pci(device->dev) ? to_pci_dev(device->dev) : NULL;
	if (!pdev)
		return false;

	return ser->bdf == pci_dev_id(pdev) &&
	       ser->domain == pci_domain_nr(pdev->bus);
}

/*
 * vfio-pci makes the vfio device visible before its probe completes, and the
 * probe runs under the device lock. Look the device up with that lock held, so
 * that a device found here has completed its probe, and the reset done by
 * finish() does not contend with the probe for that lock. Return the device
 * with a registration reference held.
 */
static struct vfio_device *
vfio_pci_liveupdate_find_probed(const struct vfio_pci_core_device_ser *ser)
{
	struct vfio_device *device;
	struct pci_dev *pdev;

	pdev = pci_get_domain_bus_and_slot(ser->domain, PCI_BUS_NUM(ser->bdf),
					   ser->bdf & 0xff);
	if (!pdev)
		return NULL;

	device_lock(&pdev->dev);
	device = vfio_find_device(ser, match_device);
	device_unlock(&pdev->dev);
	pci_dev_put(pdev);
	return device;
}

static int
vfio_pci_liveupdate_attach_incoming(struct vfio_pci_core_device *vdev,
				    struct vfio_pci_liveupdate_incoming *in)
{
	guard(mutex)(&vdev->vdev.dev_set->lock);

	if (vdev->liveupdate_incoming || !vdev->liveupdate_kept_running ||
	    !pci_liveupdate_is_incoming(vdev->pdev))
		return -EBUSY;

	vdev->liveupdate_incoming = in;
	return 0;
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	struct pci_saved_state *saved_state = NULL;
	const struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_liveupdate_incoming *in;
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;
	struct pci_dev *pdev;
	struct file *file;
	size_t in_size;
	int ret;

	if (!args->serialized_data)
		return -EINVAL;

	ser = phys_to_virt(args->serialized_data);
	ret = vfio_pci_liveupdate_validate_ser(ser);
	if (ret)
		return ret;

	device = vfio_pci_liveupdate_find_probed(ser);
	if (!device)
		return -ENODEV;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	if (!vfio_device_is_noiommu(device)) {
		ret = -EOPNOTSUPP;
		goto out_put_dev;
	}

	if (!pci_liveupdate_is_incoming(pdev) ||
	    ser->cfg_size != pdev->cfg_size) {
		ret = -EINVAL;
		goto out_put_dev;
	}

	if (ser->saved_state_size) {
		saved_state = vfio_pci_liveupdate_parse_saved_state(pdev, ser);
		if (IS_ERR(saved_state)) {
			ret = PTR_ERR(saved_state);
			goto out_put_dev;
		}
	}

	in_size = struct_size(in, data, size_mul(2, ser->cfg_size));
	in = kzalloc(in_size, GFP_KERNEL);
	if (!in) {
		ret = -ENOMEM;
		goto out_free_saved;
	}

	in->saved_state = saved_state;
	in->reset_works = !!(ser->flags & VFIO_PCI_SER_RESET_WORKS);
	in->cfg_size = ser->cfg_size;
	memcpy(in->data, ser->data, 2 * (size_t)ser->cfg_size);

	file = vfio_device_liveupdate_cdev_open(device);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto out_free_in;
	}

	ret = vfio_pci_liveupdate_attach_incoming(vdev, in);
	if (ret)
		goto out_fput;

	args->file = file;
	vfio_device_put_registration(device);
	return 0;

out_fput:
	fput(file);
out_free_in:
	kfree(in);
out_free_saved:
	kfree(saved_state);
out_put_dev:
	vfio_device_put_registration(device);
	return ret;
}

static void vfio_pci_liveupdate_finish_vdev(struct vfio_pci_core_device *vdev,
					    const struct vfio_pci_core_device_ser *ser,
					    const char *reason)
{
	struct pci_dev *pdev = vdev->pdev;
	struct pci_saved_state *saved_state = NULL;
	bool reset_works = true;
	bool was_reset;

	scoped_guard(mutex, &vdev->vdev.dev_set->lock) {
		if (vdev->liveupdate_kept_running) {
			if (vdev->liveupdate_incoming) {
				saved_state = vdev->liveupdate_incoming->saved_state;
				vdev->liveupdate_incoming->saved_state = NULL;
				reset_works = vdev->liveupdate_incoming->reset_works;
			} else if (!vfio_pci_liveupdate_validate_ser(ser) &&
				   ser->cfg_size == pdev->cfg_size) {
				reset_works = !!(ser->flags & VFIO_PCI_SER_RESET_WORKS);
				saved_state = vfio_pci_liveupdate_parse_saved_state(pdev, ser);
				if (IS_ERR(saved_state))
					saved_state = NULL;
			}

			/*
			 * The device is stopped below, so stop shielding it from
			 * the dev_set resets, including the fallback done by
			 * vfio_pci_core_liveupdate_reset().
			 */
			vdev->liveupdate_kept_running = false;

			/* A device stopped early was logged when it was stopped. */
			if (vdev->liveupdate_stopped_early)
				reason = NULL;

			/*
			 * An unbind of the device waits for the reference held
			 * by finish() with the device lock held, so the reset
			 * only tries to take that lock. If the device has a
			 * function reset but was not reset, e.g. because that
			 * lock was taken, bus mastering is still cleared, and
			 * the function reset is retried when the device is
			 * unbound, unless the device is opened before that. See
			 * vfio_pci_liveupdate_unregister_dev() and
			 * vfio_pci_liveupdate_claim_incoming().
			 */
			was_reset = vfio_pci_core_liveupdate_reset(vdev,
								   saved_state,
								   reset_works,
								   reason);
			vdev->liveupdate_reset_pending = reset_works &&
							 !was_reset;
		}
		vfio_pci_liveupdate_free_incoming(vdev);
	}

	if (pci_liveupdate_is_incoming(pdev))
		pci_liveupdate_finish(pdev);
}

/*
 * Look up the vfio-pci device of a preserved device that was not retrieved,
 * with the device lock held, like vfio_pci_liveupdate_find_probed(). If it is
 * bound to vfio-pci, return the vfio-pci device with a registration reference
 * held, so that the caller finishes it. Otherwise, finish the device here.
 */
static struct vfio_device *
vfio_pci_liveupdate_finish_lookup(const struct vfio_pci_core_device_ser *ser)
{
	struct vfio_device *device = NULL;
	struct pci_dev *pdev, *bridge;

	pdev = pci_get_domain_bus_and_slot(ser->domain, PCI_BUS_NUM(ser->bdf),
					   ser->bdf & 0xff);
	if (!pdev)
		return NULL;

	bridge = pci_upstream_bridge(pdev);
	if (bridge)
		pci_dev_lock(bridge);
	pci_dev_lock(pdev);

	if (pci_liveupdate_is_incoming(pdev)) {
		if (!pdev->dev.driver) {
			pci_warn(pdev,
				 "Resetting device not taken over after Live Update (not bound to vfio-pci)\n");
			pci_clear_master(pdev);
			pci_reset_function_locked(pdev);
			pci_liveupdate_finish(pdev);
		} else {
			/*
			 * A driver is bound and its probe has completed, as it
			 * runs under the device lock. Holding the lock also
			 * prevents an unbind until the registration reference
			 * below is taken.
			 */
			device = vfio_find_device(ser, match_device);
			if (!device) {
				pci_warn(pdev,
					 "Device not taken over after Live Update is bound to %s, not resetting it\n",
					 dev_driver_string(&pdev->dev));
				pci_liveupdate_finish(pdev);
			}
		}
	}

	pci_dev_unlock(pdev);
	if (bridge)
		pci_dev_unlock(bridge);
	pci_dev_put(pdev);
	return device;
}

static void vfio_pci_liveupdate_finish(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device_ser *ser = phys_to_virt(args->serialized_data);
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;
	const char *reason;

	if (args->retrieve_status > 0) {
		device = vfio_device_from_file(args->file);
		vdev = container_of(device, struct vfio_pci_core_device, vdev);
		vfio_pci_liveupdate_finish_vdev(vdev, ser,
						"retrieved but not opened");
	} else {
		reason = args->retrieve_status ? "retrieve failed" : "not retrieved";
		device = vfio_pci_liveupdate_finish_lookup(ser);
		if (device) {
			vdev = container_of(device, struct vfio_pci_core_device,
					    vdev);
			vfio_pci_liveupdate_finish_vdev(vdev, ser, reason);
			vfio_device_put_registration(device);
		}
	}

	kho_restore_free(ser);
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
