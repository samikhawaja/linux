// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for DMA direct live update preservation.
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <kunit/static_stub.h>
#include <linux/dma-mapping.h>
#include <linux/kho/abi/dma_alloc.h>
#include <linux/kexec_handover.h>
#include <linux/slab.h>

static struct page *mock_kho_restore_pages(phys_addr_t phys, unsigned long nr_pages)
{
	struct page *page = phys_to_page(phys);

	if (!kho_test_pages_preserved(phys, nr_pages))
		return NULL;

	kho_unpreserve_pages(page, nr_pages);
	return page;
}

static void test_dma_direct_preserve_restore_common(struct kunit *test, bool coherent)
{
	struct device dev = {0};
	void *addr1, *addr2;
	dma_addr_t handle1, handle2;
	size_t size = PAGE_SIZE * 4; /* Test 4 pages */
	u64 state;
	int ret, i;

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

	device_initialize(&dev);
	dev.coherent_dma_mask = DMA_BIT_MASK(64);
	dev.dma_mask = &dev.coherent_dma_mask;

	addr1 = dma_alloc_coherent(&dev, size, &handle1, GFP_KERNEL);
	if (!addr1) {
		kunit_skip(test, "DMA allocation failed (unsupported configuration)");
		return;
	}

	ret = dma_preserve_coherent_allocation(&dev, addr1, size, handle1, &state);
	KUNIT_EXPECT_EQ(test, ret, 0);

	for (i = 0; i < 4; i++) {
		phys_addr_t p = virt_to_phys(addr1 + (i * PAGE_SIZE));
		KUNIT_EXPECT_TRUE_MSG(test, kho_test_pages_preserved(p, 1), "Page %d (phys %llx) not tracked in KHO", i, (u64)p);
	}

	addr2 = dma_restore_coherent_allocation(&dev, size, &handle2, GFP_KERNEL, state);
	KUNIT_ASSERT_NOT_NULL(test, addr2);

	KUNIT_EXPECT_EQ(test, handle1, handle2);

	dma_free_coherent(&dev, size, addr2, handle2);

	for (i = 0; i < 4; i++) {
		phys_addr_t p = virt_to_phys(addr1 + (i * PAGE_SIZE));
		KUNIT_EXPECT_FALSE_MSG(test, kho_test_pages_preserved(p, 1), "Page %d (phys %llx) still tracked after free", i, (u64)p);
	}
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