// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for DMA direct live update preservation.
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <kunit/static_stub.h>
#include <linux/dma-mapping.h>
#include <linux/kho/abi/dma_alloc.h>
#include <linux/slab.h>
#include <linux/list.h>

/* Prototypes for functions exposed by kernel/dma/direct.c for testing */
void *dma_kho_alloc_preserve(size_t size);
int dma_kho_preserve_folio(struct folio *folio);
void dma_kho_unpreserve_folio(struct folio *folio);
void dma_kho_unpreserve_free(void *ptr);
struct folio *dma_kho_restore_folio(phys_addr_t phys);
void dma_kho_restore_free(void *ptr);

static LIST_HEAD(mock_kho_pages);

struct mock_kho_page {
	struct list_head list;
	phys_addr_t phys;
};

/* Our mock replacements */
static int mock_kho_preserve_folio(struct folio *folio)
{
	struct mock_kho_page *p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	
	p->phys = folio_pfn(folio) << PAGE_SHIFT;
	list_add(&p->list, &mock_kho_pages);
	return 0;
}

static void mock_kho_unpreserve_folio(struct folio *folio)
{
	struct mock_kho_page *p, *tmp;
	phys_addr_t phys = folio_pfn(folio) << PAGE_SHIFT;

	list_for_each_entry_safe(p, tmp, &mock_kho_pages, list) {
		if (p->phys == phys) {
			list_del(&p->list);
			kfree(p);
			return;
		}
	}
}

static void *mock_kho_alloc_preserve(size_t size)
{
	void *ptr = (void *)get_zeroed_page(GFP_KERNEL);

	if (ptr)
		mock_kho_preserve_folio(virt_to_folio(ptr));
	return ptr;
}

static void mock_kho_unpreserve_free(void *ptr)
{
	mock_kho_unpreserve_folio(virt_to_folio(ptr));
	free_page((unsigned long)ptr);
}

static void mock_kho_restore_free(void *ptr)
{
	free_page((unsigned long)ptr);
}

static struct folio *mock_kho_restore_folio(phys_addr_t phys)
{
	struct mock_kho_page *p;
	
	list_for_each_entry(p, &mock_kho_pages, list) {
		if (p->phys == phys)
			return page_folio(phys_to_page(phys));
	}
	return NULL;
}

static void test_dma_direct_preserve_restore_common(struct kunit *test, bool coherent)
{
	struct device dev = {0};
	void *addr1, *addr2;
	dma_addr_t handle1, handle2;
	size_t size = PAGE_SIZE;
	u64 state;
	int ret;

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL)
	dev.dma_coherent = coherent;
#else
	if (!coherent) {
		kunit_skip(test, "Architecture does not support non-coherent DMA");
		return;
	}
#endif

	/* Activate mocks */
	kunit_activate_static_stub(test, dma_kho_alloc_preserve, mock_kho_alloc_preserve);
	kunit_activate_static_stub(test, dma_kho_preserve_folio, mock_kho_preserve_folio);
	kunit_activate_static_stub(test, dma_kho_unpreserve_folio, mock_kho_unpreserve_folio);
	kunit_activate_static_stub(test, dma_kho_unpreserve_free, mock_kho_unpreserve_free);
	kunit_activate_static_stub(test, dma_kho_restore_folio, mock_kho_restore_folio);
	kunit_activate_static_stub(test, dma_kho_restore_free, mock_kho_restore_free);

	device_initialize(&dev);
	dev.coherent_dma_mask = DMA_BIT_MASK(64);
	dev.dma_mask = &dev.coherent_dma_mask;

	/* 1. Allocate and write data */
	addr1 = dma_alloc_coherent(&dev, size, &handle1, GFP_KERNEL);
	if (!addr1) {
		kunit_skip(test, "DMA allocation failed (unsupported configuration)");
		return;
	}

	memset(addr1, 0x5A, size);
	
	/* 2. Preserve */
	ret = dma_preserve_coherent_allocation(&dev, addr1, size, handle1, &state);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* 3. Restore */
	addr2 = dma_restore_coherent_allocation(&dev, size, &handle2, GFP_KERNEL, state);
	KUNIT_ASSERT_NOT_NULL(test, addr2);
	
	/* 4. Verify Data and Handle */
	KUNIT_EXPECT_EQ(test, *(u32 *)addr2, 0x5A5A5A5A);
	KUNIT_EXPECT_EQ(test, handle1, handle2);

	/* 5. Clean up */
	dma_free_coherent(&dev, size, addr2, handle2);
}

static void test_dma_direct_coherent(struct kunit *test)
{
	test_dma_direct_preserve_restore_common(test, true);
}

static void test_dma_direct_non_coherent(struct kunit *test)
{
	test_dma_direct_preserve_restore_common(test, false);
}

static struct kunit_case dma_direct_test_cases[] = {
	KUNIT_CASE(test_dma_direct_coherent),
	KUNIT_CASE(test_dma_direct_non_coherent),
	{}
};

static struct kunit_suite dma_direct_test_suite = {
	.name = "dma_direct_liveupdate",
	.test_cases = dma_direct_test_cases,
};
kunit_test_suite(dma_direct_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit test for DMA direct live update preservation");
MODULE_LICENSE("GPL");
