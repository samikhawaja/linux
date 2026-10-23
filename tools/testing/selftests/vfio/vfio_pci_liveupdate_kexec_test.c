// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/sizes.h>
#include <linux/vfio.h>

#include <libliveupdate.h>
#include <libvfio.h>

#define DRIVER_REGION_SIZE	SZ_2M
#define MEMCPY_FOLIO_SIZE	SZ_2M
#define NR_MEMFD_FOLIOS		3
#define MEMFD_SIZE		(NR_MEMFD_FOLIOS * SZ_2M)

#define MEMCPY_TIMEOUT_S	(30 * 60)

static struct dma_region src_region;
static struct dma_region dst_region;
static void *memfd_vaddr;
static u64 folio_pa[NR_MEMFD_FOLIOS];

static const char *device_bdf;

static char state_session[LIVEUPDATE_SESSION_NAME_LENGTH];
static char data_session[LIVEUPDATE_SESSION_NAME_LENGTH];
static char device_session[LIVEUPDATE_SESSION_NAME_LENGTH];

enum {
	STATE_TOKEN,
	DEVICE_TOKEN,
	MEMFD_TOKEN,
	DATA_TOKEN,
};

static u64 memcpy_size(struct vfio_pci_device *device)
{
	return min_t(u64, src_region.size, device->driver.max_memcpy_size);
}

static bool is_filled(const char *buf, u64 size, char c)
{
	return buf[0] == c && !memcmp(buf, buf + 1, size - 1);
}

static void dma_memcpy_one(struct vfio_pci_device *device, char c)
{
	u64 size = memcpy_size(device);
	void *src = src_region.vaddr;
	void *dst = dst_region.vaddr;

	memset(src, c, size);
	memset(dst, 0, size);

	VFIO_ASSERT_EQ(vfio_pci_driver_memcpy(device, to_iova(device, src),
					      to_iova(device, dst), size), 0);
	VFIO_ASSERT_TRUE(is_filled(dst, size, c));
}

static void dma_memcpy_start(struct vfio_pci_device *device)
{
	u64 size = memcpy_size(device);
	void *src = src_region.vaddr;
	void *dst = dst_region.vaddr;
	u64 count;

	/*
	 * Rough Math: If we assume the device will perform memcpy at a rate of
	 * 30GB/s then 7200GB of transfers will run for about 4 minutes.
	 */
	count = (u64)7200 * SZ_1G / size;
	count = min_t(u64, count, device->driver.max_memcpy_count);

	memset(src, 'x', size);
	memset(dst, 0, size);

	ksft_print_msg("Starting %lu DMA memcpy operations of size 0x%lx\n",
		       count, size);
	vfio_pci_driver_memcpy_start(device, to_iova(device, src),
				     to_iova(device, dst), size, count);
}

static void dma_memcpy_wait(struct vfio_pci_device *device)
{
	u64 size = memcpy_size(device);
	void *src = src_region.vaddr;
	void *dst = dst_region.vaddr;
	struct timespec start, end;
	long ms;

	VFIO_ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &start), 0);

	/* Fail rather than hang if the device stopped, e.g. as it was reset. */
	alarm(MEMCPY_TIMEOUT_S);
	VFIO_ASSERT_EQ(vfio_pci_driver_memcpy_wait(device), 0);
	alarm(0);

	VFIO_ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &end), 0);
	ms = (end.tv_sec - start.tv_sec) * 1000 +
	     (end.tv_nsec - start.tv_nsec) / 1000000;

	VFIO_ASSERT_TRUE(is_filled(src, size, 'x'));
	VFIO_ASSERT_TRUE(is_filled(dst, size, 'x'));

	ksft_print_msg("Pre-kexec DMA memcpy operations completed after %ld ms\n",
		       ms);
}

static void check_noiommu(const char *bdf)
{
	const char *cdev_path = __vfio_pci_get_cdev_path(bdf, true);
	int fd;

	if (!cdev_path)
		ksft_exit_skip("Device %s is not bound to vfio-pci in noiommu mode\n",
			       bdf);

	fd = open(cdev_path, O_RDWR);
	if (fd < 0)
		ksft_exit_skip("open(%s) failed: %s\n", cdev_path,
			       strerror(errno));

	close(fd);
	free((void *)cdev_path);
}

static void dma_memfd_regions_map(struct vfio_pci_device *device, int fd,
				  bool before_kexec)
{
	struct dma_region *regions[NR_MEMFD_FOLIOS] = {
		&device->driver.region,
		&src_region,
		&dst_region,
	};
	int i;

	memfd_vaddr = vfio_memfd_mmap_aligned(fd, MEMFD_SIZE, SZ_2M);

	for (i = 0; i < NR_MEMFD_FOLIOS; i++) {
		struct dma_region *r = regions[i];

		r->vaddr = memfd_vaddr + (u64)i * SZ_2M;
		r->size = SZ_2M;
		iommu_map_identity(device->iommu, r);

		if (before_kexec)
			folio_pa[i] = r->iova;
		else
			VFIO_ASSERT_EQ(r->iova, folio_pa[i],
				       "Folio %d moved across kexec (0x%lx != 0x%lx)\n",
				       i, r->iova, folio_pa[i]);
	}
}

static void dma_memfd_recheck_pa(struct vfio_pci_device *device)
{
	struct dma_region *regions[NR_MEMFD_FOLIOS] = {
		&device->driver.region,
		&src_region,
		&dst_region,
	};
	int i;

	for (i = 0; i < NR_MEMFD_FOLIOS; i++) {
		u64 pa, len;

		VFIO_ASSERT_EQ(__iommu_noiommu_get_pa(device->iommu,
						      regions[i]->iova, SZ_2M,
						      &pa, &len), 0);
		VFIO_ASSERT_EQ(pa, folio_pa[i]);
		VFIO_ASSERT_EQ(len, (u64)SZ_2M);
	}
}

static void dma_memfd_regions_unmap(struct vfio_pci_device *device)
{
	iommu_unmap(device->iommu, &dst_region);
	iommu_unmap(device->iommu, &src_region);
	iommu_unmap(device->iommu, &device->driver.region);
	VFIO_ASSERT_EQ(munmap(memfd_vaddr, MEMFD_SIZE), 0);
	memfd_vaddr = NULL;
}

static void save_data_session(int luo_fd)
{
	char buf[256];
	int data_fd;

	data_fd = luo_create_session(luo_fd, data_session);
	VFIO_ASSERT_GE(data_fd, 0);

	snprintf_assert(buf, sizeof(buf), "0x%lx 0x%lx 0x%lx",
			folio_pa[0], folio_pa[1], folio_pa[2]);
	VFIO_ASSERT_EQ(create_and_preserve_memfd(data_fd, DATA_TOKEN, buf), 0);
}

static void load_data_session(int luo_fd)
{
	char buf[256] = {};
	char *end, *cur;
	int data_fd;
	int mfd;
	int i;

	data_fd = luo_retrieve_session(luo_fd, data_session);
	VFIO_ASSERT_GE(data_fd, 0);

	mfd = luo_session_retrieve_fd(data_fd, DATA_TOKEN);
	VFIO_ASSERT_GE(mfd, 0);
	VFIO_ASSERT_GT(pread(mfd, buf, sizeof(buf) - 1, 0), 0);
	VFIO_ASSERT_EQ(close(mfd), 0);

	cur = buf;
	for (i = 0; i < NR_MEMFD_FOLIOS; i++) {
		errno = 0;
		folio_pa[i] = strtoull(cur, &end, 0);
		VFIO_ASSERT_EQ(errno, 0);
		VFIO_ASSERT_NE(end, cur);
		cur = end;
	}

	VFIO_ASSERT_EQ(luo_session_finish(data_fd), 0);
	VFIO_ASSERT_EQ(close(data_fd), 0);
}

static void before_kexec(int luo_fd)
{
	struct vfio_pci_device *device;
	struct iommu *iommu;
	void *init_vaddr = NULL;
	int session_fd;
	int memfd;
	int ret;

	check_noiommu(device_bdf);

	memfd = vfio_thp_memfd_alloc(MEMFD_SIZE, &init_vaddr);
	if (memfd < 0)
		ksft_exit_skip("Failed to allocate 2MB THP memfd: %s\n",
			       strerror(-memfd));
	VFIO_ASSERT_EQ(munmap(init_vaddr, MEMFD_SIZE), 0);

	iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	device = vfio_pci_device_init(device_bdf, iommu);

	if (!device->driver.ops || !device->driver.ops->attach)
		ksft_exit_skip("No driver that supports attach() found for %s\n",
			       device_bdf);

	dma_memfd_regions_map(device, memfd, true);

	create_state_file(luo_fd, state_session, STATE_TOKEN, /*next_stage=*/2);

	session_fd = luo_create_session(luo_fd, device_session);
	VFIO_ASSERT_GE(session_fd, 0);

	/*
	 * Preserve the DMA memfd in the same session before the device so that
	 * LUO's LIFO finish order resets an unclaimed device before freeing
	 * the memfd folios.
	 */
	ret = luo_session_preserve_fd(session_fd, memfd, MEMFD_TOKEN);
	VFIO_ASSERT_EQ(ret, 0);

	dma_memfd_recheck_pa(device);

	ret = luo_session_preserve_fd(session_fd, device->fd, DEVICE_TOKEN);
	VFIO_ASSERT_EQ(ret, 0);

	vfio_pci_driver_init(device);
	dma_memcpy_one(device, 'a');

	dma_memcpy_start(device);

	/*
	 * Disable interrupts on the device or freeze() will fail. The memcpy
	 * operations do not generate any.
	 */
	vfio_pci_msix_disable(device);

	save_data_session(luo_fd);

	close(luo_fd);
	daemonize_and_wait();
}

static void check_open_vfio_device_fails(void)
{
	const char *cdev_path = __vfio_pci_get_cdev_path(device_bdf, true);
	char group_path[PATH_MAX];
	int group_fd;
	int group;
	int ret;

	VFIO_ASSERT_NOT_NULL(cdev_path);

	ret = open(cdev_path, O_RDWR);
	VFIO_ASSERT_EQ(ret, -1);
	VFIO_ASSERT_EQ(errno, EBUSY);
	free((void *)cdev_path);

	if (__sysfs_iommu_group_get(device_bdf, &group))
		return;

	snprintf_assert(group_path, sizeof(group_path), "/dev/vfio/noiommu-%d",
			group);
	group_fd = open(group_path, O_RDWR);
	if (group_fd < 0) {
		VFIO_ASSERT_EQ(errno, ENOENT);
		return;
	}

	ret = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, device_bdf);
	VFIO_ASSERT_EQ(ret, -1);
	VFIO_ASSERT_EQ(errno, EBUSY);

	VFIO_ASSERT_EQ(close(group_fd), 0);
}

static void after_kexec(int luo_fd, int state_session_fd)
{
	struct vfio_pci_device *device;
	struct iommu *iommu;
	int session_fd;
	int device_fd;
	int memfd;
	int stage;

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	VFIO_ASSERT_EQ(stage, 2);

	load_data_session(luo_fd);

	check_open_vfio_device_fails();

	session_fd = luo_retrieve_session(luo_fd, device_session);
	VFIO_ASSERT_GE(session_fd, 0);

	device_fd = luo_session_retrieve_fd(session_fd, DEVICE_TOKEN);
	VFIO_ASSERT_GE(device_fd, 0);

	iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	device = __vfio_pci_device_init(device_bdf, iommu, device_fd);

	memfd = luo_session_retrieve_fd(session_fd, MEMFD_TOKEN);
	VFIO_ASSERT_GE(memfd, 0);

	dma_memfd_regions_map(device, memfd, false);

	vfio_pci_driver_attach(device, /*memcpy_in_progress=*/true);
	dma_memcpy_wait(device);

	/* The device was not reset, so it can be used without reinitializing it. */
	dma_memcpy_one(device, 'y');

	VFIO_ASSERT_EQ(luo_session_finish(session_fd), 0);
	VFIO_ASSERT_EQ(close(session_fd), 0);

	vfio_pci_driver_remove(device);
	dma_memfd_regions_unmap(device);
	VFIO_ASSERT_EQ(close(memfd), 0);

	vfio_pci_device_cleanup(device);
	iommu_cleanup(iommu);
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	snprintf_assert(device_session, sizeof(device_session),
			"vfio-lu-device-%s", device_bdf);
	snprintf_assert(data_session, sizeof(data_session),
			"vfio-lu-data-%s", device_bdf);
	snprintf_assert(state_session, sizeof(state_session),
			"vfio-lu-state-%s", device_bdf);

	return luo_test(argc, argv, state_session, before_kexec, after_kexec);
}
