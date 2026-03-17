/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KHO_ABI_DMA_ALLOC_H
#define _LINUX_KHO_ABI_DMA_ALLOC_H

#include <linux/types.h>

/**
 * DOC: DMA Alloc ABI
 *
 * This header defines the structures used to serialize the state of DMA
 * allocations, done by device driver, across a Live Update.
 *
 * Depending on system and device configurations, DMA allocations can be:
 *
 * - Direct allocations that are contiguous and,
 *   - vmapped
 *   - non vmapped.
 *   - Not mapped in kernel.
 * - IOMMU mapped allocations that are,
 *   - contiguous
 *   - non-contiguous and remapped.
 */

/**
 * struct dma_alloc_ser - Serialized state of a single DMA allocation.
 * @iova: IOVA used by this DMA allocation.
 * @nr_pages: Number of pages in this allocation.
 * @is_folio: Whether the pages backing the memory allocation are folios.
 * @is_contiguous: Whether the memory backing the allocation is contiguous.
 * @page_phys: Physical addresses of the preserved pages.
 */
struct dma_alloc_ser {
	u64 iova;
	u64 nr_pages;
	bool is_folio;
	bool is_contiguous;
	u64 page_phys[];
} __packed;

#endif /* _LINUX_KHO_ABI_DMA_ALLOC_H */
