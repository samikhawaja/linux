/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_IOMMU_H
#define SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_IOMMU_H

#include <linux/list.h>
#include <linux/types.h>

#include <libvfio/assert.h>

typedef u64 iova_t;

struct iommu_mode {
	const char *name;
	const char *container_path;
	unsigned long iommu_type;
	/*
	 * For devices bound to vfio-pci in noiommu mode, i.e. not behind an
	 * IOMMU. Such devices DMA to the addresses programmed into them as
	 * physical addresses.
	 */
	bool noiommu;
};

extern const char *default_iommu_mode;

struct dma_region {
	struct list_head link;
	void *vaddr;
	iova_t iova;
	u64 size;
};

struct iommu {
	const struct iommu_mode *mode;
	int container_fd;
	int iommufd;
	u32 ioas_id;
	struct list_head dma_regions;
};

struct iommu *iommu_init(const char *iommu_mode);
void iommu_cleanup(struct iommu *iommu);

int __iommu_map(struct iommu *iommu, struct dma_region *region);

static inline void iommu_map(struct iommu *iommu, struct dma_region *region)
{
	VFIO_ASSERT_EQ(__iommu_map(iommu, region), 0);
}

int __iommu_unmap(struct iommu *iommu, struct dma_region *region, u64 *unmapped);

static inline void iommu_unmap(struct iommu *iommu, struct dma_region *region)
{
	VFIO_ASSERT_EQ(__iommu_unmap(iommu, region, NULL), 0);
}

int __iommu_unmap_all(struct iommu *iommu, u64 *unmapped);

static inline void iommu_unmap_all(struct iommu *iommu)
{
	VFIO_ASSERT_EQ(__iommu_unmap_all(iommu, NULL), 0);
}

int __iommu_hva2iova(struct iommu *iommu, void *vaddr, iova_t *iova);
iova_t iommu_hva2iova(struct iommu *iommu, void *vaddr);

struct iommu_iova_range *iommu_iova_ranges(struct iommu *iommu, u32 *nranges);

#define MODE_VFIO_TYPE1_IOMMU "vfio_type1_iommu"
#define MODE_VFIO_TYPE1V2_IOMMU "vfio_type1v2_iommu"
#define MODE_IOMMUFD_COMPAT_TYPE1 "iommufd_compat_type1"
#define MODE_IOMMUFD_COMPAT_TYPE1V2 "iommufd_compat_type1v2"
#define MODE_IOMMUFD "iommufd"

/*
 * iommufd with a device bound to vfio-pci in noiommu mode
 * (vfio.enable_unsafe_noiommu_mode=1 and CONFIG_IOMMUFD_NOIOMMU), whose cdev
 * is /dev/vfio/devices/noiommu-vfioX. Opening it requires CAP_SYS_RAWIO.
 *
 * Not part of FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(): tests must opt in, since
 * the device DMAs to IOVAs as physical addresses, and tests that DMA to
 * arbitrary or unmapped IOVAs would corrupt memory.
 */
#define MODE_IOMMUFD_NOIOMMU "iommufd_noiommu"

/*
 * Generator for VFIO selftests fixture variants that replicate across all
 * possible IOMMU modes. Tests must define FIXTURE_VARIANT_ADD_IOMMU_MODE()
 * which should then use FIXTURE_VARIANT_ADD() to create the variant.
 */
#define FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(...) \
FIXTURE_VARIANT_ADD_IOMMU_MODE(vfio_type1_iommu, ##__VA_ARGS__); \
FIXTURE_VARIANT_ADD_IOMMU_MODE(vfio_type1v2_iommu, ##__VA_ARGS__); \
FIXTURE_VARIANT_ADD_IOMMU_MODE(iommufd_compat_type1, ##__VA_ARGS__); \
FIXTURE_VARIANT_ADD_IOMMU_MODE(iommufd_compat_type1v2, ##__VA_ARGS__); \
FIXTURE_VARIANT_ADD_IOMMU_MODE(iommufd, ##__VA_ARGS__)

#endif /* SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_IOMMU_H */
