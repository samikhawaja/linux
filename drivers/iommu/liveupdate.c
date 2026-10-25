// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

/**
 * DOC: IOMMU Live Update
 *
 * The IOMMU subsystem participates in the Live Update process to preserve its
 * IOMMU domains and IOMMU specific state of preserved devices.
 *
 * File-Lifecycle-Bound (FLB) Data
 * ===============================
 *
 * Preserved state of IOMMU HW needs to be restored during boot when the IOMMU is
 * initialized and registered. The preserved IOMMU domains also need to be
 * restored and associated with the preserved devices early during boot. For
 * this reason, IOMMU subsystem uses LUO File-Lifecycle-Bound Data to store some
 * state globally.
 *
 * During preservation of IOMMUFD into LUO, some of the state is stored in the
 * IOMMU FLB so it can be restored during boot. Once the FDs are retrieved from LUO
 * the restored state can be reassociated with the relevant IOMMUFDs.
 *
 * The FLB also contains the state of IOMMU HW that might be shared between
 * multiple devices, domains and iommufds. This is restored early during boot
 * and no re-association of this state is needed later.
 *
 * As the state is preserved using KHO, the FLB handler callbacks are called by
 * LUO only when the KHO is enabled. So there is no need to check whether KHO is
 * enabled in the callback implementation.
 *
 */

#define pr_fmt(fmt)    "iommu: liveupdate: " fmt

#include <linux/errno.h>
#include <linux/generic_pt/iommu.h>
#include <linux/iommu-liveupdate.h>
#include <linux/iommu.h>
#include <linux/kexec_handover.h>
#include <linux/kho_block.h>
#include <linux/liveupdate.h>
#include <linux/pci.h>

#include "iommu-priv.h"

/**
 * struct iommu_ser_set - Set of serialized objects of one type
 * @bs: KHO block set holding the serialized objects
 * @it: Iterator used to reserve new objects at the end of @bs
 * @count: Number of reserved objects, including the deleted ones
 * @head_pa: Field of struct iommu_flb_ser that records the head of @bs
 */
struct iommu_ser_set {
	struct kho_block_set bs;
	struct kho_block_set_it it;
	u64 count;
	u64 *head_pa;
};

/* Iterate over the serialized objects of a set, skipping the deleted ones */
#define iommu_ser_set_for_each(_set, _it, _obj)				\
	for (kho_block_set_it_init(&(_it), &(_set)->bs);		\
	     ((_obj) = kho_block_set_it_read_entry(&(_it)));)		\
		if ((_obj)->hdr.flags & IOMMU_SER_FLAG_DELETED)		\
			continue;					\
		else

struct iommu_flb_obj {
	/* Protects the serialized state and the object sets */
	struct mutex lock;
	struct iommu_flb_ser *ser;

	struct iommu_ser_set iommu_set;
	struct iommu_ser_set domain_set;
	struct iommu_ser_set device_set;
};

/*
 * Reserve a new serialized object at the end of the set. The deleted objects
 * are not reused, so the objects never move and can be referenced by their
 * physical address.
 */
static void *iommu_ser_set_alloc(struct iommu_ser_set *set)
{
	struct iommu_hdr_ser *hdr;
	int ret;

	ret = kho_block_set_grow(&set->bs, set->count + 1);
	if (ret)
		return ERR_PTR(ret);

	if (!set->count)
		kho_block_set_it_init(&set->it, &set->bs);

	hdr = kho_block_set_it_reserve_entry(&set->it);
	if (WARN_ON_ONCE(!hdr))
		return ERR_PTR(-ENOSPC);

	set->count++;
	*set->head_pa = kho_block_set_head_pa(&set->bs);

	hdr->ref_count = 1;
	hdr->size = set->bs.entry_size;
	return hdr;
}

/*
 * Restore a set of serialized objects from the previous kernel. The objects
 * are read using the size recorded by the previous kernel in their header.
 *
 * Failure to restore preserved IOMMU state is considered fatal. This is
 * because the IOMMU translations for preserved IOMMUs were kept enabled in the
 * previous kernel and the preserved devices have their IOMMU domains still
 * present. Not being able to restore means that the memory mapped into
 * preserved domains might be already corrupted by the preserved devices.
 */
static void iommu_ser_set_restore(struct iommu_ser_set *set, u64 head_pa)
{
	size_t entry_size = sizeof(struct iommu_hdr_ser);

	if (head_pa) {
		struct kho_block_header_ser *block = phys_to_virt(head_pa);
		struct iommu_hdr_ser *hdr = (void *)(block + 1);

		entry_size = hdr->size;
	}

	if (entry_size < sizeof(struct iommu_hdr_ser) ||
	    entry_size > KHO_BLOCK_SIZE - sizeof(struct kho_block_header_ser))
		goto err_fatal;

	kho_block_set_init(&set->bs, entry_size);
	if (kho_block_set_restore(&set->bs, head_pa))
		goto err_fatal;

	return;

err_fatal:
	panic("Failed to restore IOMMU Live Update FLB\n");
}

static void iommu_liveupdate_flb_free(struct iommu_flb_obj *obj)
{
	kho_block_set_destroy(&obj->domain_set.bs);
	kho_block_set_destroy(&obj->device_set.bs);
	kho_block_set_destroy(&obj->iommu_set.bs);

	kho_unpreserve_free(obj->ser);
	mutex_destroy(&obj->lock);
	kfree(obj);
}

static int iommu_liveupdate_flb_preserve(struct liveupdate_flb_op_args *argp)
{
	struct iommu_flb_obj *obj;
	struct iommu_flb_ser *ser;

	/* obj exists only in the current kernel to track preserved state */
	obj = kzalloc_obj(*obj, GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	/* ser is allocated via KHO and will survive the kexec */
	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		kfree(obj);
		return PTR_ERR(ser);
	}

	mutex_init(&obj->lock);
	obj->ser = ser;
	ser->version = IOMMU_LUO_FLB_VERSION;

	kho_block_set_init(&obj->domain_set.bs, sizeof(struct iommu_domain_ser));
	obj->domain_set.head_pa = &ser->domain_block_phys;
	kho_block_set_init(&obj->device_set.bs, sizeof(struct iommu_device_ser));
	obj->device_set.head_pa = &ser->device_block_phys;
	kho_block_set_init(&obj->iommu_set.bs, sizeof(struct iommu_hw_ser));
	obj->iommu_set.head_pa = &ser->iommu_block_phys;

	argp->obj = obj;
	argp->data = virt_to_phys(ser);
	return 0;
}

static void iommu_liveupdate_flb_unpreserve(struct liveupdate_flb_op_args *argp)
{
	iommu_liveupdate_flb_free(argp->obj);
}

static void iommu_liveupdate_flb_finish(struct liveupdate_flb_op_args *argp)
{
	struct iommu_flb_obj *obj = argp->obj;

	kho_block_set_destroy(&obj->domain_set.bs);
	kho_block_set_destroy(&obj->device_set.bs);
	kho_block_set_destroy(&obj->iommu_set.bs);

	folio_put(virt_to_folio(obj->ser));
	mutex_destroy(&obj->lock);
	kfree(obj);
}

static int iommu_liveupdate_flb_retrieve(struct liveupdate_flb_op_args *argp)
{
	struct iommu_flb_obj *obj;
	struct iommu_flb_ser *ser;

	obj = kzalloc_obj(*obj, GFP_KERNEL);
	if (!obj) {
		/*
		 * If retrieve fails, the finish path won't be called as
		 * can_finish() will fail, preventing the restore.
		 */
		return -ENOMEM;
	}

	/* Data must be present and valid from the previous kernel */
	if (!kho_restore_folio(argp->data))
		panic("Failed to restore IOMMU Live Update FLB\n");

	ser = phys_to_virt(argp->data);

	/*
	 * FLB version mismatch is considered fatal for security reasons for
	 * now. Check it before restoring the arrays, as their layout depends
	 * on the version.
	 */
	if (ser->version != IOMMU_LUO_FLB_VERSION)
		panic("Incoming IOMMU FLB version (v%llu) is incompatible with this kernel (v%d)\n",
		      ser->version, IOMMU_LUO_FLB_VERSION);

	mutex_init(&obj->lock);
	obj->ser = ser;

	iommu_ser_set_restore(&obj->domain_set, ser->domain_block_phys);
	iommu_ser_set_restore(&obj->device_set, ser->device_block_phys);
	iommu_ser_set_restore(&obj->iommu_set, ser->iommu_block_phys);
	argp->obj = obj;
	return 0;
}

static struct liveupdate_flb_ops iommu_flb_ops = {
	.preserve = iommu_liveupdate_flb_preserve,
	.unpreserve = iommu_liveupdate_flb_unpreserve,
	.retrieve = iommu_liveupdate_flb_retrieve,
	.finish = iommu_liveupdate_flb_finish,
};

static struct liveupdate_flb iommu_flb = {
	.compatible = IOMMU_LUO_FLB_COMPATIBLE,
	.ops = &iommu_flb_ops,
};

/**
 * iommu_liveupdate_register_flb() - Register IOMMU Live Update FLB
 * @handler: Live Update file handler to associate FLB with
 *
 * Register a Live Update FLB for IOMMU state preservation with the file
 * handler.
 *
 * Return: 0 on success, or negative error code.
 */
int iommu_liveupdate_register_flb(struct liveupdate_file_handler *handler)
{
	return liveupdate_register_flb(handler, &iommu_flb);
}
EXPORT_SYMBOL(iommu_liveupdate_register_flb);

/**
 * iommu_liveupdate_unregister_flb() - Unregister IOMMU Live Update FLB
 * @handler: Live Update file handler to associate FLB with
 */
void iommu_liveupdate_unregister_flb(struct liveupdate_file_handler *handler)
{
	liveupdate_unregister_flb(handler, &iommu_flb);
}
EXPORT_SYMBOL(iommu_liveupdate_unregister_flb);

/*
 * iommu_liveupdate_flb_get_incoming() - Helper function to get FLB state
 * @flb_objp: Pointer to get the restored FLB object
 *
 * Return: 0 if FLB state found and restored, error if no data found
 */
static int iommu_liveupdate_flb_get_incoming(struct iommu_flb_obj **flb_objp)
{
	int ret;

	ret = liveupdate_flb_get_incoming(&iommu_flb, (void **)flb_objp);
	if (ret == -ENODATA || ret == -ENOENT || ret == -EOPNOTSUPP)
		return ret;

	if (ret)
		panic("Failed to restore IOMMU Live Update FLB\n");

	return 0;
}

/**
 * iommu_for_each_preserved_device() - Iterate on preserved devices
 * @fn: Iterator function to call for each preserved device
 * @arg: Argument to pass to iterator function
 *
 * Return: 0 on success, or an error.
 */
int iommu_for_each_preserved_device(iommu_preserved_device_iter_fn fn,
				    void *arg)
{
	struct iommu_device_ser *device_ser;
	struct iommu_flb_obj *flb_obj;
	struct kho_block_set_it it;
	int ret;

	ret = iommu_liveupdate_flb_get_incoming(&flb_obj);
	if (ret)
		return ret;

	iommu_ser_set_for_each(&flb_obj->device_set, it, device_ser) {
		ret = fn(device_ser, arg);
		if (ret)
			break;
	}

	liveupdate_flb_put_incoming(&iommu_flb);
	return ret;
}
EXPORT_SYMBOL(iommu_for_each_preserved_device);

/**
 * iommu_get_preserved_data() - Get preserved data for an IOMMU HW
 * @token: Token used to preserve this IOMMU HW
 * @type: IOMMU type in preserved state
 *
 * Gets the preserved state of an IOMMU HW using token and the IOMMU type.
 *
 * Return: struct iommu_hw_ser on success, NULL if no preserved state found.
 */
struct iommu_hw_ser *iommu_get_preserved_data(u64 token, enum iommu_type_ser type)
{
	struct iommu_hw_ser *iommu_ser;
	struct iommu_flb_obj *flb_obj;
	struct kho_block_set_it it;
	int ret;

	ret = iommu_liveupdate_flb_get_incoming(&flb_obj);
	if (ret)
		return NULL;

	iommu_ser_set_for_each(&flb_obj->iommu_set, it, iommu_ser) {
		if (iommu_ser->token == token && iommu_ser->type == type)
			break;
	}

	liveupdate_flb_put_incoming(&iommu_flb);
	return iommu_ser;
}
EXPORT_SYMBOL(iommu_get_preserved_data);

/**
 * iommu_preserve_domain() - Preserve an IOMMU domain across live update
 * @domain: Domain to preserve
 * @ser: Pointer to receive the virtual serialized domain state handle
 *
 * Return: 0 on success, or negative error code.
 */
int iommu_preserve_domain(struct iommu_domain *domain, struct iommu_domain_ser **ser)
{
	struct pt_iommu *pt = iommupt_from_domain(domain);
	struct iommu_domain_ser *domain_ser;
	struct iommu_flb_obj *flb_obj;
	int ret;

	if (!pt || !pt->ops->preserve || !pt->ops->unpreserve)
		return -EOPNOTSUPP;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return ret;

	mutex_lock(&flb_obj->lock);
	if (domain->domain_ser) {
		ret = -EBUSY;
		goto out_unlock;
	}

	domain_ser = iommu_ser_set_alloc(&flb_obj->domain_set);
	if (IS_ERR(domain_ser)) {
		ret = PTR_ERR(domain_ser);
		goto out_unlock;
	}

	ret = pt->ops->preserve(pt, domain_ser);
	if (ret) {
		domain_ser->hdr.flags |= IOMMU_SER_FLAG_DELETED;
		goto out_unlock;
	}

	domain->domain_ser = domain_ser;
	*ser = domain_ser;
	ret = 0;
out_unlock:
	mutex_unlock(&flb_obj->lock);
	liveupdate_flb_put_outgoing(&iommu_flb);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_preserve_domain);

/**
 * iommu_unpreserve_domain() - Unpreserve a preserved IOMMU domain
 * @domain: Domain to unpreserve
 */
void iommu_unpreserve_domain(struct iommu_domain *domain)
{
	struct pt_iommu *pt = iommupt_from_domain(domain);
	struct iommu_domain_ser *domain_ser;
	struct iommu_flb_obj *flb_obj;
	int ret;

	if (WARN_ON(!pt || !pt->ops->unpreserve))
		return;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (WARN_ON(ret))
		return;

	mutex_lock(&flb_obj->lock);
	if (!domain->domain_ser)
		goto out_unlock;

	/*
	 * There is no check for attached devices here. The correctness relies
	 * on the Live Update Orchestrator's session lifecycle. All resources
	 * (iommufd, vfio devices) are preserved within a single session. If the
	 * session is torn down, the .unpreserve callbacks for all files will be
	 * invoked, ensuring a consistent cleanup without needing explicit
	 * refcounting for the serialized objects here.
	 */
	domain_ser = domain->domain_ser;
	pt->ops->unpreserve(pt, domain_ser);
	domain_ser->hdr.flags |= IOMMU_SER_FLAG_DELETED;
	domain->domain_ser = NULL;
out_unlock:
	mutex_unlock(&flb_obj->lock);
	liveupdate_flb_put_outgoing(&iommu_flb);
}
EXPORT_SYMBOL_GPL(iommu_unpreserve_domain);

static struct iommu_hw_ser *alloc_iommu_hw_ser(struct iommu_flb_obj *flb)
{
	return iommu_ser_set_alloc(&flb->iommu_set);
}

static int iommu_preserve_locked(struct iommu_device *iommu,
				 struct iommu_flb_obj *flb_obj)
{
	struct iommu_hw_ser *iommu_hw_ser;
	int ret;

	if (!iommu->ops->preserve || !iommu->ops->unpreserve)
		return -EOPNOTSUPP;

	lockdep_assert_held(&flb_obj->lock);
	if (iommu->iommu_ser) {
		iommu->iommu_ser->hdr.ref_count++;
		return 0;
	}

	iommu_hw_ser = alloc_iommu_hw_ser(flb_obj);
	if (IS_ERR(iommu_hw_ser))
		return PTR_ERR(iommu_hw_ser);

	ret = iommu->ops->preserve(iommu, iommu_hw_ser);
	if (ret) {
		iommu_hw_ser->hdr.flags |= IOMMU_SER_FLAG_DELETED;
		return ret;
	}

	iommu->iommu_ser = iommu_hw_ser;
	return ret;
}

static void iommu_unpreserve_locked(struct iommu_device *iommu,
				    struct iommu_flb_obj *flb_obj)
{
	struct iommu_hw_ser *iommu_hw_ser = iommu->iommu_ser;

	lockdep_assert_held(&flb_obj->lock);
	if (WARN_ON(!iommu_hw_ser))
		return;

	if (WARN_ON_ONCE(!iommu->ops->preserve ||
			 !iommu->ops->unpreserve))
		return;

	iommu_hw_ser->hdr.ref_count--;
	if (iommu_hw_ser->hdr.ref_count)
		return;

	iommu->iommu_ser = NULL;
	iommu->ops->unpreserve(iommu, iommu_hw_ser);
	iommu_hw_ser->hdr.flags |= IOMMU_SER_FLAG_DELETED;
}

static struct iommu_device_ser *alloc_iommu_device_ser(struct iommu_flb_obj *flb)
{
	return iommu_ser_set_alloc(&flb->device_set);
}

/**
 * iommu_preserve_device() - Preserve device state across live update
 * @domain: Associated IOMMU domain
 * @dev: Device to preserve
 * @dma_owner_token: Token to identify DMA owner of this device
 *
 * Return: 0 on success, or negative error code.
 */
int iommu_preserve_device(struct iommu_domain *domain,
			  struct device *dev, u64 dma_owner_token)
{
	struct iommu_device_ser *device_ser;
	struct iommu_flb_obj *flb_obj;
	struct dev_iommu *iommu;
	struct pci_dev *pdev;
	int ret;

	if (!dev_is_pci(dev))
		return -EOPNOTSUPP;

	if (!iommu_group_dma_owner_claimed(dev->iommu_group))
		return -EINVAL;

	if (!iommu_group_is_singleton(dev->iommu_group))
		return -EOPNOTSUPP;

	pdev = to_pci_dev(dev);
	iommu = dev->iommu;
	if (!iommu->iommu_dev->ops->preserve_device ||
	    !iommu->iommu_dev->ops->unpreserve_device ||
	    !iommu->iommu_dev->ops->preserve ||
	    !iommu->iommu_dev->ops->unpreserve)
		return -EOPNOTSUPP;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return ret;

	mutex_lock(&flb_obj->lock);
	if (!domain->domain_ser) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (dev_iommu_preserved_state(dev)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	device_ser = alloc_iommu_device_ser(flb_obj);
	if (IS_ERR(device_ser)) {
		ret = PTR_ERR(device_ser);
		goto out_unlock;
	}

	ret = iommu_preserve_locked(iommu->iommu_dev, flb_obj);
	if (ret) {
		device_ser->hdr.flags |= IOMMU_SER_FLAG_DELETED;
		goto out_unlock;
	}

	device_ser->attachment.domain_phys = virt_to_phys(domain->domain_ser);
	device_ser->iommu_phys = virt_to_phys(iommu->iommu_dev->iommu_ser);
	device_ser->devid = pci_dev_id(pdev);
	device_ser->pci_domain_nr = pci_domain_nr(pdev->bus);
	device_ser->dma_owner_token = dma_owner_token;

	ret = iommu->iommu_dev->ops->preserve_device(dev, device_ser);
	if (!ret) {
		WRITE_ONCE(dev->iommu->device_ser, device_ser);

		/* Validate that no sibling device was added concurrently */
		if (!iommu_group_is_singleton(dev->iommu_group)) {
			iommu->iommu_dev->ops->unpreserve_device(dev, device_ser);
			WRITE_ONCE(dev->iommu->device_ser, NULL);
			ret = -EOPNOTSUPP;
		}
	}

	if (ret) {
		device_ser->hdr.flags |= IOMMU_SER_FLAG_DELETED;
		iommu_unpreserve_locked(iommu->iommu_dev, flb_obj);
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&flb_obj->lock);
	liveupdate_flb_put_outgoing(&iommu_flb);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_preserve_device);

/**
 * iommu_unpreserve_device() - Unpreserve device state
 * @domain: Associated IOMMU domain
 * @dev: Device to unpreserve
 */
void iommu_unpreserve_device(struct iommu_domain *domain, struct device *dev)
{
	struct iommu_device_ser *iommu_device_ser;
	struct iommu_flb_obj *flb_obj;
	struct dev_iommu *iommu;
	int ret;

	if (!dev_is_pci(dev))
		return;

	if (!iommu_group_dma_owner_claimed(dev->iommu_group))
		return;

	iommu = dev->iommu;
	if (WARN_ON(!iommu->iommu_dev->ops->unpreserve_device ||
		    !iommu->iommu_dev->ops->unpreserve))
		return;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (WARN_ON(ret))
		return;

	mutex_lock(&flb_obj->lock);
	iommu_device_ser = dev_iommu_preserved_state(dev);
	if (WARN_ON(!iommu_device_ser))
		goto out_unlock;

	iommu_device_ser->hdr.flags |= IOMMU_SER_FLAG_DELETED;
	iommu->iommu_dev->ops->unpreserve_device(dev, iommu_device_ser);
	WRITE_ONCE(dev->iommu->device_ser, NULL);

	iommu_unpreserve_locked(iommu->iommu_dev, flb_obj);
out_unlock:
	mutex_unlock(&flb_obj->lock);
	liveupdate_flb_put_outgoing(&iommu_flb);
}
EXPORT_SYMBOL_GPL(iommu_unpreserve_device);

static inline bool match_device_ser(struct iommu_device_ser *match,
				    struct pci_dev *pdev)
{
	return match->devid == pci_dev_id(pdev) && match->pci_domain_nr == pci_domain_nr(pdev->bus);
}

/**
 * iommu_init_device_preserved_data() - Initialize preserved state for device
 * @dev: Target device
 *
 * Looks up incoming Live Update state for @dev and attaches it to the device if
 * found.
 */
void iommu_init_device_preserved_data(struct device *dev)
{
	struct iommu_device_ser *device_ser;
	struct iommu_flb_obj *flb_obj;
	struct kho_block_set_it it;
	int ret;

	if (!dev_is_pci(dev))
		return;

	ret = iommu_liveupdate_flb_get_incoming(&flb_obj);
	if (ret)
		return;

	mutex_lock(&flb_obj->lock);
	iommu_ser_set_for_each(&flb_obj->device_set, it, device_ser) {
		if (match_device_ser(device_ser, to_pci_dev(dev))) {
			device_ser->hdr.flags |= IOMMU_SER_FLAG_INCOMING;
			WRITE_ONCE(dev->iommu->device_ser, device_ser);
			break;
		}
	}

	mutex_unlock(&flb_obj->lock);
	liveupdate_flb_put_incoming(&iommu_flb);
}
EXPORT_SYMBOL(iommu_init_device_preserved_data);

/**
 * iommu_restore_domain() - Restore a preserved domain for a device
 * @dev: Target device
 * @ser: Serialized device state
 *
 * Restores or reuses a restored preserved domain for @dev from serialized state
 * @ser.
 *
 * Return: Restored iommu_domain pointer, or ERR_PTR.
 */
struct iommu_domain *iommu_restore_domain(struct device *dev,
					  struct iommu_device_ser *ser)
{
	struct iommu_domain_ser *domain_ser;
	struct iommu_flb_obj *flb_obj;
	struct iommu_domain *domain;
	struct pt_iommu *pt;
	int ret;

	ret = iommu_liveupdate_flb_get_incoming(&flb_obj);
	if (ret)
		return ERR_PTR(ret);

	mutex_lock(&flb_obj->lock);

	/* Preserved device should have a preserved domain */
	if (!ser->attachment.domain_phys) {
		domain = ERR_PTR(-EINVAL);
		goto out;
	}

	domain_ser = phys_to_virt(ser->attachment.domain_phys);
	if (domain_ser->restored_domain) {
		domain = domain_ser->restored_domain;
		goto out;
	}

	domain_ser->hdr.flags |= IOMMU_SER_FLAG_INCOMING;
	domain = iommu_paging_domain_alloc(dev);
	if (IS_ERR(domain))
		goto out;

	pt = iommupt_from_domain(domain);
	if (!pt) {
		iommu_domain_free(domain);
		domain = ERR_PTR(-EOPNOTSUPP);
		goto out;
	}

	ret = pt->ops->restore(pt, domain_ser);
	if (ret) {
		iommu_domain_free(domain);
		domain = ERR_PTR(ret);
		goto out;
	}

	domain->domain_ser = domain_ser;
	domain_ser->restored_domain = domain;

out:
	mutex_unlock(&flb_obj->lock);
	liveupdate_flb_put_incoming(&iommu_flb);
	return domain;
}
