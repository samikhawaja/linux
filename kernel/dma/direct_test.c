// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for DMA alloction live update preservation.
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <kunit/static_stub.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/kho/abi/dma_alloc.h>
#include <linux/kexec_handover.h>
#include <linux/slab.h>

static const size_t dma_test_sizes[] = {
	128,
	PAGE_SIZE,
	2 * PAGE_SIZE,
	3 * PAGE_SIZE,
	8 * PAGE_SIZE,
};

static void dma_size_desc(const size_t *size, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "size=%zu", *size);
}

KUNIT_ARRAY_PARAM(dma_size, dma_test_sizes, dma_size_desc);

static struct page *mock_kho_restore_pages(phys_addr_t phys, unsigned long nr_pages)
{
	struct page *page = phys_to_page(phys);

	if (!kho_test_pages_preserved(phys, nr_pages))
		return NULL;

	kho_unpreserve_pages(page, nr_pages);
	return page;
}

static struct folio *mock_kho_restore_folio(phys_addr_t phys)
{
	struct folio *folio = page_folio(phys_to_page(phys));

	if (!kho_test_pages_preserved(phys, (1 << folio_order(folio))))
		return NULL;

	kho_unpreserve_folio(folio);
	return folio;
}

static void test_dma_direct_preserve_restore_common(struct kunit *test,
						    bool coherent,
						    size_t size)
{
	struct device dev = {0};
	void *addr1, *addr2;
	dma_addr_t handle1, handle2;
	unsigned long nr_pages = 1 << get_order(size);
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

	kunit_activate_static_stub(test, kho_restore_pages, mock_kho_restore_pages);
	kunit_activate_static_stub(test, kho_restore_folio, mock_kho_restore_folio);

	device_initialize(&dev);
	dev.coherent_dma_mask = DMA_BIT_MASK(64);
	dev.dma_mask = &dev.coherent_dma_mask;

	/* use atomic so it doesn't use CMA */
	addr1 = dma_alloc_coherent(&dev, size, &handle1, GFP_ATOMIC);
	if (!addr1) {
		kunit_skip(test, "DMA allocation failed (unsupported configuration)");
		return;
	}

	ret = dma_preserve_coherent_allocation(&dev, addr1, size, handle1, &state);
	KUNIT_EXPECT_EQ(test, ret, 0);

	KUNIT_EXPECT_TRUE_MSG(test, kho_test_pages_preserved(virt_to_phys(addr1), nr_pages), "Allocated block not tracked in KHO");

	addr2 = dma_restore_coherent_allocation(&dev, size, &handle2, GFP_KERNEL, state);
	KUNIT_ASSERT_NOT_NULL(test, addr2);

	KUNIT_EXPECT_EQ(test, handle1, handle2);

	dma_free_coherent(&dev, size, addr2, handle2);

	KUNIT_EXPECT_FALSE_MSG(test, kho_test_pages_preserved(virt_to_phys(addr1), nr_pages), "Allocated block still tracked after free");
}

static void test_dma_direct_coherent(struct kunit *test)
{
	const size_t *size = test->param_value;
	test_dma_direct_preserve_restore_common(test, true, *size);
}

static void test_dma_direct_non_coherent(struct kunit *test)
{
	const size_t *size = test->param_value;
	test_dma_direct_preserve_restore_common(test, false, *size);
}

static void test_dma_direct_cma(struct kunit *test)
{
#ifdef CONFIG_DMA_CMA
	struct device dev = {0};
	const size_t *size = test->param_value;
	void *addr1;
	dma_addr_t handle1;
	u64 state;
	int ret;

	device_initialize(&dev);
	dev.coherent_dma_mask = DMA_BIT_MASK(64);
	dev.dma_mask = &dev.coherent_dma_mask;

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL)
	dev.dma_coherent = true;
#endif

	/* Allocate from CMA */
	addr1 = dma_alloc_coherent(&dev, *size, &handle1, GFP_KERNEL);
	if (!addr1) {
		kunit_skip(test, "DMA allocation failed (unsupported configuration)");
		return;
	}

	ret = dma_preserve_coherent_allocation(&dev, addr1, *size, handle1, &state);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);

	dma_free_coherent(&dev, *size, addr1, handle1);
#else
	kunit_skip(test, "CONFIG_DMA_CMA is disabled");
#endif
}

static struct kunit_case dma_direct_test_cases[] = {
	KUNIT_CASE_PARAM(test_dma_direct_coherent, dma_size_gen_params),
	KUNIT_CASE_PARAM(test_dma_direct_non_coherent, dma_size_gen_params),
	KUNIT_CASE(test_dma_direct_cma),
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
