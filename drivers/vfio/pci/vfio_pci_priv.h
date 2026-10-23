/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VFIO_PCI_PRIV_H
#define VFIO_PCI_PRIV_H

#include <linux/vfio_pci_core.h>

/* Special capability IDs predefined access */
#define PCI_CAP_ID_INVALID		0xFF	/* default raw access */
#define PCI_CAP_ID_INVALID_VIRT		0xFE	/* default virt access */

/* Cap maximum number of ioeventfds per device (arbitrary) */
#define VFIO_PCI_IOEVENTFD_MAX		1000

extern const struct vfio_device_ops vfio_pci_ops;

struct vfio_pci_ioeventfd {
	struct list_head	next;
	struct vfio_pci_core_device	*vdev;
	struct virqfd		*virqfd;
	void __iomem		*addr;
	uint64_t		data;
	loff_t			pos;
	int			bar;
	int			count;
	bool			test_mem;
};

bool __vfio_pci_intx_mask(struct vfio_pci_core_device *vdev);
bool vfio_pci_intx_mask(struct vfio_pci_core_device *vdev);
void __vfio_pci_intx_unmask(struct vfio_pci_core_device *vdev);
void vfio_pci_intx_unmask(struct vfio_pci_core_device *vdev);

int vfio_pci_eventfd_replace_locked(struct vfio_pci_core_device *vdev,
				    struct vfio_pci_eventfd __rcu **peventfd,
				    struct eventfd_ctx *ctx);

int vfio_pci_set_irqs_ioctl(struct vfio_pci_core_device *vdev, uint32_t flags,
			    unsigned index, unsigned start, unsigned count,
			    void *data);

ssize_t vfio_pci_config_rw(struct vfio_pci_core_device *vdev, char __user *buf,
			   size_t count, loff_t *ppos, bool iswrite);

ssize_t vfio_pci_config_rw_single(struct vfio_pci_core_device *vdev,
				  char __user *buf, size_t count, loff_t *ppos,
				  bool iswrite);

ssize_t vfio_pci_bar_rw(struct vfio_pci_core_device *vdev, char __user *buf,
			size_t count, loff_t *ppos, bool iswrite);

#ifdef CONFIG_VFIO_PCI_VGA
ssize_t vfio_pci_vga_rw(struct vfio_pci_core_device *vdev, char __user *buf,
			size_t count, loff_t *ppos, bool iswrite);
#else
static inline ssize_t vfio_pci_vga_rw(struct vfio_pci_core_device *vdev,
				      char __user *buf, size_t count,
				      loff_t *ppos, bool iswrite)
{
	return -EINVAL;
}
#endif

int vfio_pci_ioeventfd(struct vfio_pci_core_device *vdev, loff_t offset,
		       uint64_t data, int count, int fd);

int vfio_pci_init_perm_bits(void);
void vfio_pci_uninit_perm_bits(void);

int vfio_config_init(struct vfio_pci_core_device *vdev);
void vfio_config_free(struct vfio_pci_core_device *vdev);

int vfio_pci_set_power_state(struct vfio_pci_core_device *vdev,
			     pci_power_t state);

void vfio_pci_zap_and_down_write_memory_lock(struct vfio_pci_core_device *vdev);
u16 vfio_pci_memory_lock_and_enable(struct vfio_pci_core_device *vdev);
void vfio_pci_memory_unlock_and_restore(struct vfio_pci_core_device *vdev,
					u16 cmd);

#ifdef CONFIG_VFIO_PCI_IGD
bool vfio_pci_is_intel_display(struct pci_dev *pdev);
int vfio_pci_igd_init(struct vfio_pci_core_device *vdev);
#else
static inline bool vfio_pci_is_intel_display(struct pci_dev *pdev)
{
	return false;
}

static inline int vfio_pci_igd_init(struct vfio_pci_core_device *vdev)
{
	return -ENODEV;
}
#endif

#ifdef CONFIG_VFIO_PCI_ZDEV_KVM
int vfio_pci_info_zdev_add_caps(struct vfio_pci_core_device *vdev,
				struct vfio_info_cap *caps);
int vfio_pci_zdev_open_device(struct vfio_pci_core_device *vdev);
void vfio_pci_zdev_close_device(struct vfio_pci_core_device *vdev);
int vfio_pci_zdev_feature_err(struct vfio_device *device, u32 flags,
			      void __user *arg, size_t argsz);
#else
static inline int vfio_pci_info_zdev_add_caps(struct vfio_pci_core_device *vdev,
					      struct vfio_info_cap *caps)
{
	return -ENODEV;
}

static inline int vfio_pci_zdev_open_device(struct vfio_pci_core_device *vdev)
{
	return 0;
}

static inline void vfio_pci_zdev_close_device(struct vfio_pci_core_device *vdev)
{}

static inline int vfio_pci_zdev_feature_err(struct vfio_device *device,
					    u32 flags, void __user *arg,
					    size_t argsz)
{
	return -ENOTTY;
}
#endif

static inline bool vfio_pci_is_vga(struct pci_dev *pdev)
{
	return (pdev->class >> 8) == PCI_CLASS_DISPLAY_VGA;
}

#ifdef CONFIG_VFIO_PCI_DMABUF
int vfio_pci_core_feature_dma_buf(struct vfio_pci_core_device *vdev, u32 flags,
				  struct vfio_device_feature_dma_buf __user *arg,
				  size_t argsz);
void vfio_pci_dma_buf_cleanup(struct vfio_pci_core_device *vdev);
void vfio_pci_dma_buf_move(struct vfio_pci_core_device *vdev, bool revoked);
#else
static inline int
vfio_pci_core_feature_dma_buf(struct vfio_pci_core_device *vdev, u32 flags,
			      struct vfio_device_feature_dma_buf __user *arg,
			      size_t argsz)
{
	return -ENOTTY;
}
static inline void vfio_pci_dma_buf_cleanup(struct vfio_pci_core_device *vdev)
{
}
static inline void vfio_pci_dma_buf_move(struct vfio_pci_core_device *vdev,
					 bool revoked)
{
}
#endif

struct vfio_pci_liveupdate_incoming {
	struct pci_saved_state *saved_state;
	bool reset_works;
	u16 cfg_size;
	u8 data[];
};

void vfio_pci_core_liveupdate_reset_locked(struct vfio_pci_core_device *vdev);

#ifdef CONFIG_VFIO_PCI_LIVEUPDATE
int __init vfio_pci_liveupdate_init(void);
void vfio_pci_liveupdate_cleanup(void);

static inline bool vfio_pci_dev_is_frozen(struct vfio_pci_core_device *vdev)
{
	/* Written in VFIO PCI Liveupdate during freeze()/unfreeze() */
	return smp_load_acquire(&vdev->liveupdate_frozen);
}

static inline bool vfio_pci_dev_is_kept_running(struct vfio_pci_core_device *vdev)
{
	return vdev->liveupdate_kept_running;
}

static inline void vfio_pci_liveupdate_init_dev(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = to_pci_dev(vdev->vdev.dev);

	if (pci_liveupdate_is_incoming(pdev)) {
		vdev->disable_idle_d3 = true;
		vdev->liveupdate_kept_running = true;
	}
}

/*
 * Called by probe once the device is registered, with the device lock held.
 * retrieve() and finish() look the device up with that lock held, see
 * vfio_pci_liveupdate_find_probed(), so they see the result of this check.
 */
static inline void
vfio_pci_liveupdate_check_noiommu(struct vfio_pci_core_device *vdev)
{
	guard(mutex)(&vdev->vdev.dev_set->lock);

	if (vdev->liveupdate_kept_running &&
	    !vfio_device_is_noiommu(&vdev->vdev)) {
		pci_warn(vdev->pdev,
			 "Stopping device kept running across Live Update: not in noiommu mode\n");
		pci_clear_master(vdev->pdev);
		vdev->liveupdate_stopped_early = true;
	}
}

static inline void
vfio_pci_liveupdate_free_incoming(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_liveupdate_incoming *in = vdev->liveupdate_incoming;

	if (!in)
		return;

	kfree(in->saved_state);
	kfree(in);
	vdev->liveupdate_incoming = NULL;
}

static inline void
vfio_pci_liveupdate_claim_incoming(struct vfio_pci_core_device *vdev)
{
	/*
	 * A reset that finish() left for unbind is not needed once the device
	 * is opened after finish(): the open resets the device if it can, and
	 * so does the close.
	 */
	vdev->liveupdate_reset_pending = false;

	if (!vdev->liveupdate_incoming)
		return;

	vfio_pci_liveupdate_free_incoming(vdev);
	vdev->liveupdate_kept_running = false;
}

static inline void
vfio_pci_liveupdate_unregister_dev(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;

	scoped_guard(mutex, &vdev->vdev.dev_set->lock) {
		/*
		 * Reset a device unbound before finish(), or whose reset
		 * finish() could not do, see vfio_pci_liveupdate_finish_vdev().
		 */
		if (vdev->liveupdate_kept_running ||
		    vdev->liveupdate_reset_pending) {
			if (vdev->liveupdate_kept_running &&
			    !vdev->liveupdate_stopped_early)
				pci_warn(pdev,
					 "Resetting device not taken over after Live Update (vfio-pci unbound)\n");
			vfio_pci_core_liveupdate_reset_locked(vdev);
			vdev->liveupdate_kept_running = false;
			vdev->liveupdate_reset_pending = false;
		}
		vfio_pci_liveupdate_free_incoming(vdev);
	}

	if (pci_liveupdate_is_incoming(pdev))
		pci_liveupdate_finish(pdev);
}
#else
static inline int vfio_pci_liveupdate_init(void)
{
	return 0;
}

static inline void vfio_pci_liveupdate_cleanup(void)
{
}

static inline bool vfio_pci_dev_is_frozen(struct vfio_pci_core_device *vdev)
{
	return false;
}

static inline bool vfio_pci_dev_is_kept_running(struct vfio_pci_core_device *vdev)
{
	return false;
}

static inline void vfio_pci_liveupdate_init_dev(struct vfio_pci_core_device *vdev)
{
}

static inline void
vfio_pci_liveupdate_check_noiommu(struct vfio_pci_core_device *vdev)
{
}

static inline void
vfio_pci_liveupdate_free_incoming(struct vfio_pci_core_device *vdev)
{
}

static inline void
vfio_pci_liveupdate_claim_incoming(struct vfio_pci_core_device *vdev)
{
}

static inline void
vfio_pci_liveupdate_unregister_dev(struct vfio_pci_core_device *vdev)
{
}
#endif /* CONFIG_VFIO_PCI_LIVEUPDATE */

#endif
