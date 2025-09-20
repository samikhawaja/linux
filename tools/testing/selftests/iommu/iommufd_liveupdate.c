// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Samiullah Khawaja <skhawaja@google.com>
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <unistd.h>

#define __EXPORTED_HEADERS__
#include <linux/iommufd.h>
#include <linux/types.h>
#include <linux/vfio.h>
#include <linux/sizes.h>
#include <libliveupdate.h>

#include "../kselftest.h"

#define ksft_assert(condition) \
	do { if (!(condition)) \
	ksft_exit_fail_msg("Failed: %s at %s %d: %s\n", \
	#condition, __FILE__, __LINE__, strerror(errno)); } while (0)

int setup_cdev(const char *vfio_cdev_path)
{
	int cdev_fd;

	cdev_fd = open(vfio_cdev_path, O_RDWR);
	if (cdev_fd < 0)
		ksft_exit_skip("Failed to open VFIO cdev: %s\n", vfio_cdev_path);

	return cdev_fd;
}

int open_iommufd(void)
{
	int iommufd;

	iommufd = open("/dev/iommu", O_RDWR);
	if (iommufd < 0)
		ksft_exit_skip("Failed to open /dev/iommu. IOMMUFD support not enabled.\n");

	return iommufd;
}

int setup_iommufd(int iommufd, int memfd, int cdev_fd, int hwpt_token)
{
	int ret;

	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.flags = 0,
	};
	struct iommu_ioas_alloc alloc_data = {
		.size = sizeof(alloc_data),
		.flags = 0,
	};
	struct iommu_hwpt_alloc hwpt_alloc = {
		.size = sizeof(hwpt_alloc),
		.flags = 0,
	};
	struct vfio_device_attach_iommufd_pt attach_data = {
		.argsz = sizeof(attach_data),
		.flags = 0,
	};
	struct iommu_hwpt_lu_set_preserve set_preserve = {
		.size = sizeof(set_preserve),
		.hwpt_token = hwpt_token,
	};
	struct iommu_ioas_map_file map_file = {
		.size = sizeof(map_file),
		.length = SZ_1M,
		.flags = IOMMU_IOAS_MAP_WRITEABLE | IOMMU_IOAS_MAP_READABLE,
		.iova = SZ_4G,
		.fd = memfd,
		.start = 0,
	};

	bind.iommufd = iommufd;
	ret = ioctl(cdev_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
	ksft_assert(!ret);

	ret = ioctl(iommufd, IOMMU_IOAS_ALLOC, &alloc_data);
	ksft_assert(!ret);

	hwpt_alloc.dev_id = bind.out_devid;
	hwpt_alloc.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(iommufd, IOMMU_HWPT_ALLOC, &hwpt_alloc);
	ksft_assert(!ret);

	attach_data.pt_id = hwpt_alloc.out_hwpt_id;
	ret = ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	ksft_assert(!ret);

	map_file.ioas_id = alloc_data.out_ioas_id;
	ret = ioctl(iommufd, IOMMU_IOAS_MAP_FILE, &map_file);
	ksft_assert(!ret);

	set_preserve.hwpt_id = attach_data.pt_id;
	ret = ioctl(iommufd, IOMMU_HWPT_LU_SET_PRESERVE, &set_preserve);
	ksft_assert(!ret);

	return ret;
}

static int create_sealed_memfd(size_t size)
{
	int fd, ret;

	fd = memfd_create("buffer", MFD_ALLOW_SEALING);
	ksft_assert(fd > 0);

	ret = ftruncate(fd, size);
	ksft_assert(!ret);

	ret = fcntl(fd, F_ADD_SEALS,
		    F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
	ksft_assert(!ret);

	return fd;
}

int luo_session_finish(int session_fd)
{
	struct liveupdate_session_finish arg = { .size = sizeof(arg) };

	if (ioctl(session_fd, LIVEUPDATE_SESSION_FINISH, &arg) < 0)
		return -errno;

	return 0;
}

int restore_iommufd(int session, int iommufd, int cdev_fd, int hwpt_token)
{
	int ret;

	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.flags = 0,
	};
	struct iommu_ioas_alloc alloc_data  = {
		.size = sizeof(alloc_data),
		.flags = 0,
	};
	struct iommu_hwpt_alloc hwpt_alloc = {
		.size = sizeof(hwpt_alloc),
		.flags = 0,
	};
	struct iommu_hwpt_lu_restore restore = {
		.size = sizeof(restore),
		.hwpt_token = hwpt_token,
		.hwpt_alloc_flags = 0,
	};
	struct vfio_device_attach_iommufd_pt attach_data = {
		.argsz = sizeof(attach_data),
		.flags = 0,
	};

	bind.iommufd = iommufd;
	ret = ioctl(cdev_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
	ksft_assert(!ret);

	ret = ioctl(iommufd, IOMMU_IOAS_ALLOC, &alloc_data);
	ksft_assert(!ret);

	ret = ioctl(iommufd, IOMMU_HWPT_LU_RESTORE, &restore);
	ksft_assert(!ret);

	/* Should fail */
	ret = luo_session_finish(session);
	ksft_assert(ret);

	hwpt_alloc.pt_id = bind.out_devid;
	hwpt_alloc.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(iommufd, IOMMU_HWPT_ALLOC, &hwpt_alloc);
	ksft_assert(ret);

	attach_data.pt_id = hwpt_alloc.pt_id;
	ret = ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	ksft_assert(!ret);
	attach_data.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	ksft_assert(!ret);

	return ret;
}

int open_liveupdate_orchestrator(void)
{
	int luo;

	luo = open("/dev/liveupdate", O_RDWR);
	ksft_assert(luo > 0);

	return luo;
}

int luo_create_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_create_session arg = { .size = sizeof(arg) };
	int ret;

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);
	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_CREATE_SESSION, &arg);
	ksft_assert(!ret);
	ksft_assert(arg.fd > 0);

	return arg.fd;
}

int luo_retrieve_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_retrieve_session arg = { .size = sizeof(arg) };
	int ret;

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);
	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_RETRIEVE_SESSION, &arg);
	ksft_assert(!ret || errno == ENOENT);

	if (ret && errno == ENOENT)
		return -errno;

	return arg.fd;
}

int liveupdate_preserve_fd(int session_fd, int fd, int token)
{
	struct liveupdate_session_preserve_fd preserve;
	int ret;

	preserve.fd = fd;
	preserve.token = token;
	preserve.size = sizeof(preserve);

	ret = ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &preserve);
	ksft_assert(!ret);

	return ret;
}

int liveupdate_restore_fd(int session_fd, int token)
{
	struct liveupdate_session_retrieve_fd arg = { .size = sizeof(arg) };
	int ret;

	arg.token = token;

	ret = ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, &arg);
	ksft_assert(!ret);
	ksft_assert(arg.fd > 0);

	return arg.fd;
}

int main(int argc, char *argv[])
{
	int iommufd, cdev_fd, memfd, luo, session, ret;
	const int token = 0x123456;
	const int cdev_token = 0x654321;
	const int hwpt_token = 0x789012;
	const int memfd_token = 0x890123;

	if (argc < 2) {
		printf("Usage: ./iommufd_liveupdate <vfio_cdev_path>\n");
		return 1;
	}

	luo = luo_open_device();
	ksft_assert(luo > 0);

	session = luo_retrieve_session(luo, "iommufd-test");
	if (session == -ENOENT) {
		session = luo_create_session(luo, "iommufd-test");

		iommufd = open_iommufd();
		memfd = create_sealed_memfd(SZ_1M);
		cdev_fd = setup_cdev(argv[1]);

		ret = setup_iommufd(iommufd, memfd, cdev_fd, hwpt_token);
		ksft_assert(!ret);

		/* Cannot preserve cdev without iommufd */
		ret = luo_session_preserve_fd(session, cdev_fd, cdev_token);
		ksft_assert(ret);

		/* Cannot preserve iommufd without preserving memfd. */
		ret = luo_session_preserve_fd(session, iommufd, token);
		ksft_assert(ret);

		ret = luo_session_preserve_fd(session, memfd, memfd_token);
		ksft_assert(!ret);

		ret = luo_session_preserve_fd(session, iommufd, token);
		ksft_assert(!ret);

		ret = luo_session_preserve_fd(session, cdev_fd, cdev_token);
		ksft_assert(!ret);

		close(session);
		session = luo_create_session(luo, "iommufd-test");

		ret = luo_session_preserve_fd(session, memfd, memfd_token);
		ksft_assert(!ret);

		ret = luo_session_preserve_fd(session, iommufd, token);
		ksft_assert(!ret);

		ret = luo_session_preserve_fd(session, cdev_fd, cdev_token);
		ksft_assert(!ret);

		daemonize_and_wait();
	} else {
		struct vfio_device_bind_iommufd bind = {
			.argsz = sizeof(bind),
			.flags = 0,
		};

		cdev_fd = luo_session_retrieve_fd(session, cdev_token);
		ksft_assert(cdev_fd > 0);

		iommufd = luo_session_retrieve_fd(session, token);
		ksft_assert(iommufd < 0);

		iommufd = open_iommufd();

		bind.iommufd = iommufd;
		ret = ioctl(cdev_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
		ksft_assert(ret);
		ksft_assert(errno == EPERM);

		iommufd = liveupdate_restore_fd(session, token);
		cdev_fd = liveupdate_restore_fd(session, cdev_token);

		/* Should fail */
		ret = luo_session_finish(session);
		ksft_assert(ret);

		ret = restore_iommufd(session, iommufd, cdev_fd, hwpt_token);
		ksft_assert(!ret);

		ret = luo_session_finish(session);
		ksft_assert(!ret);
	}

	return 0;
}
