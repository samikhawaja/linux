// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#define pr_fmt(fmt)    "iommu: liveupdate: " fmt

#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/iommu-liveupdate.h>
#include <linux/iommu.h>
#include <linux/pci.h>
#include <linux/errno.h>

static void *iommu_liveupdate_restore_array(u64 array_phys)
{
	struct iommu_array_hdr_ser *array_hdr;
	void *vaddr = array_phys ? phys_to_virt(array_phys) : NULL;

	while (array_phys) {
		/*
		 * Failure to restore preserved IOMMU state is considered fatal.
		 *
		 * This is because the IOMMU translations for preserved IOMMUs
		 * were kept enabled in the previous kernel and the preserved
		 * devices have their IOMMU domains still present. Not being
		 * able to restore means that the memory mapped into preserved
		 * domains might be already corrupted by the preserved devices.
		 *
		 * There is no way to confirm the integrity of the memory that
		 * was mapped. BUG_ON is the safest option at this point.
		 */
		BUG_ON(!kho_restore_folio(array_phys));
		array_hdr = phys_to_virt(array_phys);
		array_phys = array_hdr->next_array_phys;
	}

	return vaddr;
}

static void iommu_liveupdate_unpreserve_free(u64 array_phys)
{
	struct iommu_array_hdr_ser *array_hdr;

	while (array_phys) {
		array_hdr = phys_to_virt(array_phys);
		array_phys = array_hdr->next_array_phys;
		kho_unpreserve_free(array_hdr);
	}
}

static void iommu_liveupdate_folio_put(u64 array_phys)
{
	struct iommu_array_hdr_ser *array_hdr;

	while (array_phys) {
		array_hdr = phys_to_virt(array_phys);
		array_phys = array_hdr->next_array_phys;
		folio_put(virt_to_folio(array_hdr));
	}
}

static void iommu_liveupdate_flb_free(struct iommu_flb_obj *obj)
{
	if (obj->ser->iommu_domain_array_phys)
		iommu_liveupdate_unpreserve_free(obj->ser->iommu_domain_array_phys);

	if (obj->ser->device_array_phys)
		iommu_liveupdate_unpreserve_free(obj->ser->device_array_phys);

	if (obj->ser->iommu_array_phys)
		iommu_liveupdate_unpreserve_free(obj->ser->iommu_array_phys);

	kho_unpreserve_free(obj->ser);
	kfree(obj);
}

static int iommu_liveupdate_flb_preserve(struct liveupdate_flb_op_args *argp)
{
	struct iommu_flb_obj *obj;
	struct iommu_flb_ser *ser;
	void *mem;

	/* obj exists only in the current kernel to track preserved state */
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	mutex_init(&obj->lock);

	/* mem is allocated via KHO and will survive the kexec */
	mem = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(mem))
		goto err_free_obj;

	ser = mem;
	obj->ser = ser;

	mem = kho_alloc_preserve(PAGE_SIZE);
	if (IS_ERR(mem))
		goto err_free_ser;

	obj->curr_domain_array = mem;
	ser->iommu_domain_array_phys = virt_to_phys(obj->curr_domain_array);

	mem = kho_alloc_preserve(PAGE_SIZE);
	if (IS_ERR(mem))
		goto err_free_domains;

	obj->curr_device_array = mem;
	ser->device_array_phys = virt_to_phys(obj->curr_device_array);

	mem = kho_alloc_preserve(PAGE_SIZE);
	if (IS_ERR(mem))
		goto err_free_devices;

	obj->curr_iommu_array = mem;
	ser->iommu_array_phys = virt_to_phys(obj->curr_iommu_array);

	argp->obj = obj;
	argp->data = virt_to_phys(ser);
	return 0;

err_free_devices:
	kho_unpreserve_free(obj->curr_device_array);
err_free_domains:
	kho_unpreserve_free(obj->curr_domain_array);
err_free_ser:
	kho_unpreserve_free(obj->ser);
err_free_obj:
	kfree(obj);
	return PTR_ERR(mem);
}

static void iommu_liveupdate_flb_unpreserve(struct liveupdate_flb_op_args *argp)
{
	iommu_liveupdate_flb_free(argp->obj);
}

static void iommu_liveupdate_flb_finish(struct liveupdate_flb_op_args *argp)
{
	struct iommu_flb_obj *obj = argp->obj;

	iommu_liveupdate_folio_put(obj->ser->iommu_domain_array_phys);
	iommu_liveupdate_folio_put(obj->ser->device_array_phys);
	iommu_liveupdate_folio_put(obj->ser->iommu_array_phys);

	folio_put(virt_to_folio(obj->ser));
	kfree(obj);
}

static int iommu_liveupdate_flb_retrieve(struct liveupdate_flb_op_args *argp)
{
	struct iommu_flb_obj *obj;
	struct iommu_flb_ser *ser;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	/* Data must be present and valid from the previous kernel */
	BUG_ON(!kho_restore_folio(argp->data));

	mutex_init(&obj->lock);
	ser = phys_to_virt(argp->data);
	obj->ser = ser;

	obj->curr_domain_array = iommu_liveupdate_restore_array(ser->iommu_domain_array_phys);
	obj->curr_device_array = iommu_liveupdate_restore_array(ser->device_array_phys);
	obj->curr_iommu_array = iommu_liveupdate_restore_array(ser->iommu_array_phys);
	argp->obj = obj;
	return 0;
}

static struct liveupdate_flb_ops iommu_flb_ops = {
	.preserve = iommu_liveupdate_flb_preserve,
	.unpreserve = iommu_liveupdate_flb_unpreserve,
	.finish = iommu_liveupdate_flb_finish,
	.retrieve = iommu_liveupdate_flb_retrieve,
};

static struct liveupdate_flb iommu_flb = {
	.compatible = IOMMU_LUO_FLB_COMPATIBLE,
	.ops = &iommu_flb_ops,
};

int iommu_liveupdate_register_flb(struct liveupdate_file_handler *handler)
{
	return liveupdate_register_flb(handler, &iommu_flb);
}
EXPORT_SYMBOL(iommu_liveupdate_register_flb);

void iommu_liveupdate_unregister_flb(struct liveupdate_file_handler *handler)
{
	liveupdate_unregister_flb(handler, &iommu_flb);
}
EXPORT_SYMBOL(iommu_liveupdate_unregister_flb);

int iommu_for_each_preserved_device(iommu_preserved_device_iter_fn fn,
				    void *arg)
{
	struct iommu_flb_obj *flb_obj;
	struct iommu_device_array_ser *array;
	int ret, i, idx;

	ret = liveupdate_flb_get_incoming(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return -ENOENT;

	array = phys_to_virt(flb_obj->ser->device_array_phys);
	for (i = 0, idx = 0; i < flb_obj->ser->nr_devices; ++i, ++idx) {
		if (idx >= MAX_IOMMU_DEVICE_SERS_PER_PAGE) {
			array = phys_to_virt(array->hdr.next_array_phys);
			idx = 0;
		}

		if (array->objects[idx].hdr.deleted)
			continue;

		ret = fn(&array->objects[idx], arg);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(iommu_for_each_preserved_device);

static inline bool device_ser_match(struct iommu_device_ser *match,
				    struct pci_dev *pdev)
{
	return match->devid == pci_dev_id(pdev) && match->pci_domain_nr == pci_domain_nr(pdev->bus);
}

struct iommu_device_ser *iommu_get_device_preserved_data(struct device *dev)
{
	struct iommu_flb_obj *flb_obj;
	struct iommu_device_array_ser *array;
	int ret, i, idx;

	if (!dev_is_pci(dev))
		return NULL;

	ret = liveupdate_flb_get_incoming(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return NULL;

	array = phys_to_virt(flb_obj->ser->device_array_phys);
	for (i = 0, idx = 0; i < flb_obj->ser->nr_devices; ++i, ++idx) {
		if (idx >= MAX_IOMMU_DEVICE_SERS_PER_PAGE) {
			array = phys_to_virt(array->hdr.next_array_phys);
			idx = 0;
		}

		if (array->objects[idx].hdr.deleted)
			continue;

		if (device_ser_match(&array->objects[idx], to_pci_dev(dev))) {
			array->objects[idx].hdr.incoming = true;
			return &array->objects[idx];
		}
	}

	return NULL;
}
EXPORT_SYMBOL(iommu_get_device_preserved_data);

struct iommu_hw_ser *iommu_get_preserved_data(u64 token, enum iommu_lu_type type)
{
	struct iommu_flb_obj *flb_obj;
	struct iommu_hw_array_ser *array;
	int ret, i, idx;

	ret = liveupdate_flb_get_incoming(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return NULL;

	array = phys_to_virt(flb_obj->ser->iommu_array_phys);
	for (i = 0, idx = 0; i < flb_obj->ser->nr_iommus; ++i, ++idx) {
		if (idx >= MAX_IOMMU_HW_SERS_PER_PAGE) {
			array = phys_to_virt(array->hdr.next_array_phys);
			idx = 0;
		}

		if (array->objects[idx].hdr.deleted)
			continue;

		if (array->objects[idx].token == token &&
		    array->objects[idx].type == type)
			return &array->objects[idx];
	}

	return NULL;
}
EXPORT_SYMBOL(iommu_get_preserved_data);

static struct iommu_domain_ser *lu_alloc_domain_ser(struct iommu_flb_obj *flb)
{
	struct iommu_domain_array_ser *array = flb->curr_domain_array;
	struct iommu_domain_ser *domain;

	if (array->hdr.nr_objects >= MAX_IOMMU_DOMAIN_SERS_PER_PAGE) {
		struct iommu_domain_array_ser *next_array_phys;

		next_array_phys = kho_alloc_preserve(PAGE_SIZE);
		if (IS_ERR(next_array_phys))
			return NULL;

		array->hdr.next_array_phys = virt_to_phys(next_array_phys);
		flb->curr_domain_array = next_array_phys;
		array = next_array_phys;
	}

	domain = &array->objects[array->hdr.nr_objects++];
	domain->hdr.idx = flb->ser->nr_domains++;
	domain->hdr.ref_count = 1;

	return domain;
}

int iommu_domain_preserve(struct iommu_domain *domain, struct iommu_domain_ser **ser)
{
	struct iommu_domain_ser *domain_ser;
	struct iommu_flb_obj *flb_obj;
	int ret;

	if (!domain->ops->preserve)
		return -EOPNOTSUPP;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return ret;

	guard(mutex)(&flb_obj->lock);
	domain_ser = lu_alloc_domain_ser(flb_obj);
	if (!domain_ser)
		return -ENOMEM;

	ret = domain->ops->preserve(domain, domain_ser);
	if (ret) {
		domain_ser->hdr.deleted = true;
		return ret;
	}

	domain->preserved_state = domain_ser;
	*ser = domain_ser;
	return 0;
}
EXPORT_SYMBOL_GPL(iommu_domain_preserve);

void iommu_domain_unpreserve(struct iommu_domain *domain)
{
	struct iommu_domain_ser *domain_ser;
	struct iommu_flb_obj *flb_obj;
	int ret;

	if (!domain->ops->unpreserve)
		return;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return;

	guard(mutex)(&flb_obj->lock);

	/*
	 * There is no check for attached devices here. The correctness relies
	 * on the Live Update Orchestrator's session lifecycle. All resources
	 * (iommufd, vfio devices) are preserved within a single session. If the
	 * session is torn down, the .unpreserve callbacks for all files will be
	 * invoked, ensuring a consistent cleanup without needing explicit
	 * refcounting for the serialized objects here.
	 */
	domain_ser = domain->preserved_state;
	domain->ops->unpreserve(domain, domain_ser);
	domain_ser->hdr.deleted = true;
	domain->preserved_state = NULL;
}
EXPORT_SYMBOL_GPL(iommu_domain_unpreserve);

static struct iommu_hw_ser *iommu_lu_alloc_iommu(struct iommu_flb_obj *flb)
{
	struct iommu_hw_array_ser *array = flb->curr_iommu_array;
	struct iommu_hw_ser *instance;

	if (array->hdr.nr_objects >= MAX_IOMMU_HW_SERS_PER_PAGE) {
		struct iommu_hw_array_ser *next_array_phys;

		next_array_phys = kho_alloc_preserve(PAGE_SIZE);
		if (IS_ERR(next_array_phys))
			return NULL;

		array->hdr.next_array_phys = virt_to_phys(next_array_phys);
		flb->curr_iommu_array = next_array_phys;
		array = next_array_phys;
	}

	instance = &array->objects[array->hdr.nr_objects++];
	instance->hdr.idx = flb->ser->nr_iommus++;
	instance->hdr.ref_count = 1;

	return instance;
}

static int iommu_preserve_locked(struct iommu_device *iommu)
{
	struct iommu_flb_obj *flb_obj;
	struct iommu_hw_ser *iommu_hw_ser;
	int ret;

	if (!iommu->ops->preserve)
		return -EOPNOTSUPP;

	if (iommu->outgoing_preserved_state) {
		iommu->outgoing_preserved_state->hdr.ref_count++;
		return 0;
	}

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return ret;

	iommu_hw_ser = iommu_lu_alloc_iommu(flb_obj);
	if (!iommu_hw_ser)
		return -ENOMEM;

	ret = iommu->ops->preserve(iommu, iommu_hw_ser);
	if (ret)
		iommu_hw_ser->hdr.deleted = true;

	iommu->outgoing_preserved_state = iommu_hw_ser;
	return ret;
}

static void iommu_unpreserve_locked(struct iommu_device *iommu)
{
	struct iommu_hw_ser *iommu_hw_ser = iommu->outgoing_preserved_state;

	iommu_hw_ser->hdr.ref_count--;
	if (iommu_hw_ser->hdr.ref_count)
		return;

	iommu->outgoing_preserved_state = NULL;
	iommu->ops->unpreserve(iommu, iommu_hw_ser);
	iommu_hw_ser->hdr.deleted = true;
}

static struct iommu_device_ser *iommu_lu_alloc_device(struct iommu_flb_obj *flb)
{
	struct iommu_device_array_ser *array = flb->curr_device_array;
	struct iommu_device_ser *device;

	if (array->hdr.nr_objects >= MAX_IOMMU_DEVICE_SERS_PER_PAGE) {
		struct iommu_device_array_ser *next_array_phys;

		next_array_phys = kho_alloc_preserve(PAGE_SIZE);
		if (IS_ERR(next_array_phys))
			return NULL;

		array->hdr.next_array_phys = virt_to_phys(next_array_phys);
		flb->curr_device_array = next_array_phys;
		array = next_array_phys;
	}

	device = &array->objects[array->hdr.nr_objects++];
	device->hdr.idx = flb->ser->nr_devices++;
	device->hdr.ref_count = 1;

	return device;
}

int iommu_preserve_device(struct iommu_domain *domain,
			  struct device *dev, u64 token)
{
	struct iommu_flb_obj *flb_obj;
	struct iommu_device_ser *device_ser;
	struct dev_iommu *iommu;
	struct pci_dev *pdev;
	int ret;

	if (!dev_is_pci(dev))
		return -EOPNOTSUPP;

	if (!domain->preserved_state)
		return -EINVAL;

	if (!iommu_group_dma_owner_claimed(dev->iommu_group))
		return -EINVAL;

	pdev = to_pci_dev(dev);
	iommu = dev->iommu;
	if (!iommu->iommu_dev->ops->preserve_device ||
	    !iommu->iommu_dev->ops->preserve)
		return -EOPNOTSUPP;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return ret;

	guard(mutex)(&flb_obj->lock);
	device_ser = iommu_lu_alloc_device(flb_obj);
	if (!device_ser)
		return -ENOMEM;

	ret = iommu_preserve_locked(iommu->iommu_dev);
	if (ret) {
		device_ser->hdr.deleted = true;
		return ret;
	}

	device_ser->domain_iommu_ser.domain_phys = __pa(domain->preserved_state);
	device_ser->domain_iommu_ser.iommu_phys = __pa(iommu->iommu_dev->outgoing_preserved_state);
	device_ser->devid = pci_dev_id(pdev);
	device_ser->pci_domain_nr = pci_domain_nr(pdev->bus);
	device_ser->token = token;

	ret = iommu->iommu_dev->ops->preserve_device(dev, device_ser);
	if (ret) {
		device_ser->hdr.deleted = true;
		iommu_unpreserve_locked(iommu->iommu_dev);
		return ret;
	}

	dev->iommu->device_ser = device_ser;
	return 0;
}

void iommu_unpreserve_device(struct iommu_domain *domain, struct device *dev)
{
	struct iommu_flb_obj *flb_obj;
	struct iommu_device_ser *iommu_device_ser;
	struct dev_iommu *iommu;
	struct pci_dev *pdev;
	int ret;

	if (!dev_is_pci(dev))
		return;

	if (!iommu_group_dma_owner_claimed(dev->iommu_group))
		return;

	pdev = to_pci_dev(dev);
	iommu = dev->iommu;
	if (!iommu->iommu_dev->ops->unpreserve_device ||
	    !iommu->iommu_dev->ops->unpreserve)
		return;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (WARN_ON(ret))
		return;

	guard(mutex)(&flb_obj->lock);
	iommu_device_ser = dev_iommu_preserved_state(dev);
	if (WARN_ON(!iommu_device_ser))
		return;

	iommu->iommu_dev->ops->unpreserve_device(dev, iommu_device_ser);
	dev->iommu->device_ser = NULL;

	iommu_unpreserve_locked(iommu->iommu_dev);
}
