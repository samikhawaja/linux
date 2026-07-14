// SPDX-License-Identifier: GPL-2.0-only

#include <libliveupdate.h>
#include <libvfio.h>
#include <kselftest_harness.h>

static const char *device_bdf;

FIXTURE(vfio_pci_liveupdate_uapi_test) {
	int luo_fd;
	int session_fd;
	struct iommu *iommu;
	struct vfio_pci_device *device;
};

/*
 * LUO fails with -ENOENT if no file handler accepts a file. For the bound VFIO
 * cdev file of a device, that means that the kernel lacks VFIO PCI Live Update
 * support (CONFIG_VFIO_PCI_LIVEUPDATE=n) or that vfio-pci does not support
 * preserving the device (e.g. an SR-IOV VF).
 */
static bool device_preserve_supported(int luo_fd, int device_fd)
{
	int session_fd;
	int ret;

	session_fd = luo_create_session(luo_fd, "probe");
	VFIO_ASSERT_GE(session_fd, 0);

	ret = luo_session_preserve_fd(session_fd, device_fd, 0);

	/* Closing the session unpreserves the device. */
	VFIO_ASSERT_EQ(close(session_fd), 0);

	return ret != -ENOENT;
}

FIXTURE_VARIANT(vfio_pci_liveupdate_uapi_test) {
	const char *iommu_mode;
};

#define FIXTURE_VARIANT_ADD_IOMMU_MODE(_iommu_mode)			\
FIXTURE_VARIANT_ADD(vfio_pci_liveupdate_uapi_test, _iommu_mode) {	\
	.iommu_mode = #_iommu_mode,					\
}

FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES();
#undef FIXTURE_VARIANT_ADD_IOMMU_MODE

FIXTURE_SETUP(vfio_pci_liveupdate_uapi_test)
{
	self->luo_fd = luo_open_device();
	ASSERT_GT(self->luo_fd, 0);

	self->session_fd = luo_create_session(self->luo_fd, "session");
	ASSERT_GT(self->session_fd, 0);

	self->iommu = iommu_init(variant->iommu_mode);
	self->device = vfio_pci_device_init(device_bdf, self->iommu);

	if (self->iommu->iommufd &&
	    !device_preserve_supported(self->luo_fd, self->device->fd))
		SKIP(return, "Live Update of %s is not supported", device_bdf);
}

FIXTURE_TEARDOWN(vfio_pci_liveupdate_uapi_test)
{
	if (self->device)
		vfio_pci_device_cleanup(self->device);
	if (self->iommu)
		iommu_cleanup(self->iommu);
	if (self->session_fd > 0)
		close(self->session_fd);
	if (self->luo_fd > 0)
		close(self->luo_fd);
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_device)
{
	int ret;

	ret = luo_session_preserve_fd(self->session_fd, self->device->fd, 0);

	/*
	 * Only VFIO cdev files of devices in noiommu mode can be preserved;
	 * device files obtained through a VFIO group never can.
	 */
	ASSERT_EQ(ret, self->iommu->iommufd ? -EOPNOTSUPP : -ENOENT);
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_group_fails)
{
	int ret;

	if (self->iommu->iommufd)
		SKIP(return, "iommufd-mode does not have group files");

	ret = luo_session_preserve_fd(self->session_fd, self->device->group_fd, 0);
	ASSERT_EQ(ret, -ENOENT);
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_container_fails)
{
	int ret;

	if (self->iommu->iommufd)
		SKIP(return, "iommufd-mode does not have container files");

	ret = luo_session_preserve_fd(self->session_fd, self->iommu->container_fd, 0);
	ASSERT_EQ(ret, -ENOENT);
}

int main(int argc, char *argv[])
{
	int fd;

	fd = luo_open_device();
	if (fd < 0)
		ksft_exit_skip("open(%s) failed: %s, skipping\n",
			       LUO_DEVICE, strerror(errno));

	close(fd);

	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
