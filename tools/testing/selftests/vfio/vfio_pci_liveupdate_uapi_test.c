// SPDX-License-Identifier: GPL-2.0-only

#include <fcntl.h>
#include <linux/pci_regs.h>
#include <libliveupdate.h>
#include <libvfio.h>
#include <kselftest_harness.h>

static const char *device_bdf;
static bool device_is_noiommu;

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
FIXTURE_VARIANT_ADD_IOMMU_MODE(iommufd_noiommu);
#undef FIXTURE_VARIANT_ADD_IOMMU_MODE

FIXTURE_SETUP(vfio_pci_liveupdate_uapi_test)
{
	bool variant_is_noiommu;

	variant_is_noiommu = !strcmp(variant->iommu_mode, MODE_IOMMUFD_NOIOMMU);
	if (variant_is_noiommu != device_is_noiommu)
		SKIP(return, "%s does not apply to a %s device",
		     variant->iommu_mode,
		     device_is_noiommu ? "noiommu" : "non-noiommu");

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
	int expected;
	int ret;

	ret = luo_session_preserve_fd(self->session_fd, self->device->fd, 0);

	/*
	 * Only VFIO cdev files of devices in noiommu mode can be preserved;
	 * device files obtained through a VFIO group never can.
	 */
	if (device_is_noiommu)
		expected = 0;
	else
		expected = self->iommu->iommufd ? -EOPNOTSUPP : -ENOENT;

	ASSERT_EQ(ret, expected);
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_unbound_fd_fails)
{
	const char *cdev_path;
	int unbound_fd;
	int ret;

	if (!self->iommu->iommufd)
		SKIP(return, "Only iommufd modes use VFIO cdev files");

	cdev_path = __vfio_pci_get_cdev_path(device_bdf, device_is_noiommu);
	ASSERT_NE(cdev_path, NULL);

	unbound_fd = open(cdev_path, O_RDWR);
	ASSERT_GE(unbound_fd, 0);
	free((void *)cdev_path);

	/*
	 * A second cdev FD that has not been bound via VFIO_DEVICE_BIND_IOMMUFD
	 * must be rejected even while another FD for the same device is bound.
	 */
	ret = luo_session_preserve_fd(self->session_fd, unbound_fd, 0);
	ASSERT_EQ(ret, -ENOENT);
	ASSERT_EQ(close(unbound_fd), 0);

	/* Verify the failed attempt had no side effects on the bound FD. */
	ret = luo_session_preserve_fd(self->session_fd, self->device->fd, 0);
	ASSERT_EQ(ret, device_is_noiommu ? 0 : -EOPNOTSUPP);
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_never_bound_fails)
{
	const char *cdev_path;
	int unbound_fd;
	int ret;

	if (!self->iommu->iommufd)
		SKIP(return, "Only iommufd modes use VFIO cdev files");

	vfio_pci_device_cleanup(self->device);
	self->device = NULL;

	cdev_path = __vfio_pci_get_cdev_path(device_bdf, device_is_noiommu);
	ASSERT_NE(cdev_path, NULL);

	unbound_fd = open(cdev_path, O_RDWR);
	ASSERT_GE(unbound_fd, 0);
	free((void *)cdev_path);

	ret = luo_session_preserve_fd(self->session_fd, unbound_fd, 0);
	ASSERT_EQ(ret, -ENOENT);
	ASSERT_EQ(close(unbound_fd), 0);
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_after_unpreserve)
{
	u16 vendor_id;

	if (!device_is_noiommu)
		SKIP(return, "Only noiommu devices can be preserved");

	ASSERT_EQ(luo_session_preserve_fd(self->session_fd, self->device->fd, 0), 0);

	ASSERT_EQ(close(self->session_fd), 0);
	self->session_fd = luo_create_session(self->luo_fd, "session2");
	ASSERT_GT(self->session_fd, 0);

	ASSERT_EQ(luo_session_preserve_fd(self->session_fd, self->device->fd, 0), 0);

	vendor_id = vfio_pci_config_readw(self->device, PCI_VENDOR_ID);
	ASSERT_NE(vendor_id, 0xffff);
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
	const char *cdev_path;
	int fd;

	fd = luo_open_device();
	if (fd < 0)
		ksft_exit_skip("open(%s) failed: %s, skipping\n",
			       LUO_DEVICE, strerror(errno));

	close(fd);

	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	cdev_path = __vfio_pci_get_cdev_path(device_bdf, true);
	if (cdev_path) {
		device_is_noiommu = true;
	} else {
		cdev_path = vfio_pci_get_cdev_path(device_bdf);
		device_is_noiommu = false;
	}
	free((void *)cdev_path);

	return test_harness_run(argc, argv);
}
