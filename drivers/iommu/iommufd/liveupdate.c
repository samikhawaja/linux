// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/liveupdate.h>

#include "iommufd_private.h"

int iommufd_hwpt_lu_set_preserved(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_lu_set_preserved *cmd = ucmd->cmd;
	struct iommufd_hwpt_paging *hwpt_target, *hwpt;
	struct iommufd_ctx *ictx = ucmd->ictx;
	struct iommufd_object *obj;
	unsigned long index;
	int rc = 0;

	/* TODO: return error if iommufd is already preserved. */

	hwpt_target = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt_target))
		return PTR_ERR(hwpt_target);

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index, obj) {
		if (obj->type != IOMMUFD_OBJ_HWPT_PAGING)
			continue;

		hwpt = container_of(obj, struct iommufd_hwpt_paging, common.obj);

		if (hwpt == hwpt_target)
			continue;
		if (!hwpt->lu_preserved)
			continue;
		if (hwpt->lu_token == cmd->hwpt_token) {
			rc = -EADDRINUSE;
			goto out;
		}
	}

	hwpt_target->lu_preserved = cmd->preserved;
	hwpt_target->lu_token = cmd->hwpt_token;

out:
	xa_unlock(&ictx->objects);
	iommufd_put_object(ictx, &hwpt_target->common.obj);
	return rc;
}

static int iommufd_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static int iommufd_liveupdate_freeze(struct liveupdate_file_op_args *args)
{
	/* No-Op; everything should be made read-only */
	return 0;
}

static void iommufd_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
}

static int iommufd_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static bool iommufd_liveupdate_can_finish(struct liveupdate_file_op_args *args)
{
	return false;
}

int iommufd_hwpt_lu_restore(struct iommufd_ucmd *ucmd)
{
	return -ENOTTY;
}

static void iommufd_liveupdate_finish(struct liveupdate_file_op_args *args)
{
}

static bool iommufd_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					    struct file *file)
{
	return false;
}

static struct liveupdate_file_ops iommufd_lu_file_ops = {
	.can_preserve = iommufd_liveupdate_can_preserve,
	.preserve = iommufd_liveupdate_preserve,
	.unpreserve = iommufd_liveupdate_unpreserve,
	.freeze = iommufd_liveupdate_freeze,
	.retrieve = iommufd_liveupdate_retrieve,
	.can_finish = iommufd_liveupdate_can_finish,
	.finish = iommufd_liveupdate_finish,
};

static struct liveupdate_file_handler iommufd_lu_handler = {
	.compatible = "iommufd-v1",
	.ops = &iommufd_lu_file_ops,
};

int iommufd_liveupdate_register_lufs(void)
{
	return liveupdate_register_file_handler(&iommufd_lu_handler);
}

int iommufd_liveupdate_unregister_lufs(void)
{
	return liveupdate_unregister_file_handler(&iommufd_lu_handler);
}
