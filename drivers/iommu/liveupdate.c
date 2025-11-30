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
#include <linux/iommu-liveupdate.h>
#include <linux/iommu.h>
#include <linux/kexec_handover.h>
#include <linux/kho_block.h>
#include <linux/liveupdate.h>

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

struct iommu_flb_obj {
	/* Protects the serialized state and the object sets */
	struct mutex lock;
	struct iommu_flb_ser *ser;

	struct iommu_ser_set iommu_set;
	struct iommu_ser_set domain_set;
	struct iommu_ser_set device_set;
};

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
