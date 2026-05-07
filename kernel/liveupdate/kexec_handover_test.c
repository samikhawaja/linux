// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for Kexec Handover (KHO)
 */

#include <kunit/test.h>
#include <kunit/static_stub.h>
#include <linux/kexec_handover.h>
#include <linux/gfp.h>
#include <linux/mm.h>

static struct page *kho_test_restore_pages_mock(phys_addr_t phys, unsigned long nr_pages)
{
	struct page *page = phys_to_page(phys);

	if (!kho_test_pages_preserved(phys, nr_pages))
		return NULL;

	kho_unpreserve_pages(page, nr_pages);
	return page;
}

static struct folio *kho_test_restore_folio_mock(phys_addr_t phys)
{
	struct folio *folio = page_folio(phys_to_page(phys));

	if (!kho_test_pages_preserved(phys, (1 << folio_order(folio))))
		return NULL;

	kho_unpreserve_folio(folio);
	return folio;
}

static int kho_test_init(struct kunit *test)
{
	kunit_activate_static_stub(test, kho_restore_pages,
				   kho_test_restore_pages_mock);
	kunit_activate_static_stub(test, kho_restore_folio,
				   kho_test_restore_folio_mock);
	return 0;
}

static void kho_test_alloc_preserve_lifecycle(struct kunit *test)
{
	void *mem;
	unsigned long pfn;

	mem = kho_alloc_preserve(PAGE_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mem);
	pfn = PHYS_PFN(__pa(mem));

	/* 2. Verify preservation in tree */
	KUNIT_EXPECT_TRUE(test, kho_test_pages_preserved(__pa(mem), 1));

	/* 3. Restore (triggers mock which also unpreserves) */
	kho_restore_free(mem);

	/* 4. Verify tree is now clean */
	KUNIT_EXPECT_FALSE(test, kho_test_pages_preserved(__pa(mem), 1));
}

static void kho_test_pages_lifecycle(struct kunit *test)
{
	struct page *page;
	struct page *restored;
	int err;

	/* 1. Allocate pages */
	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 1); /* 2 pages */
	KUNIT_ASSERT_NOT_NULL(test, page);

	/* 2. Preserve */
	err = kho_preserve_pages(page, 2);
	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_TRUE(test,
			  kho_test_pages_preserved(page_to_phys(page), 2));

	/* 3. Restore */
	restored = kho_restore_pages(page_to_phys(page), 2);
	KUNIT_EXPECT_NOT_NULL(test, restored);
	KUNIT_EXPECT_PTR_EQ(test, restored, page);

	/* 4. Verify clean tree */
	KUNIT_EXPECT_FALSE(test, kho_test_pages_preserved(page_to_phys(page), 2));

	/* 5. Cleanup */
	__free_pages(page, 1);
}

static void kho_test_folio_lifecycle(struct kunit *test)
{
	struct folio *folio;
	struct folio *restored;
	int err;

	/* 1. Allocate folio */
	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 1);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	/* 2. Preserve */
	err = kho_preserve_folio(folio);
	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_TRUE(test, kho_test_pages_preserved(PFN_PHYS(folio_pfn(folio)), 1 << folio_order(folio)));

	/* 3. Restore */
	restored = kho_restore_folio(PFN_PHYS(folio_pfn(folio)));
	KUNIT_EXPECT_NOT_NULL(test, restored);
	KUNIT_EXPECT_PTR_EQ(test, restored, folio);

	/* 4. Verify clean tree */
	KUNIT_EXPECT_FALSE(test, kho_test_pages_preserved(PFN_PHYS(folio_pfn(folio)), 1 << folio_order(folio)));

	/* 5. Cleanup */
	folio_put(folio);
}

static struct kunit_case kho_test_cases[] = {
	KUNIT_CASE(kho_test_alloc_preserve_lifecycle),
	KUNIT_CASE(kho_test_pages_lifecycle),
	KUNIT_CASE(kho_test_folio_lifecycle),
	{}
};

static struct kunit_suite kho_test_suite = {
	.name = "kho_test",
	.init = kho_test_init,
	.test_cases = kho_test_cases,
};

kunit_test_suite(kho_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for Kexec Handover (KHO)");
MODULE_LICENSE("GPL");
