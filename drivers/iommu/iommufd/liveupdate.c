// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/liveupdate.h>

#include "iommufd_private.h"

int iommufd_hwpt_lu_set_preserve(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_lu_set_preserve *cmd = ucmd->cmd;
	struct iommufd_hwpt_paging *hwpt_target, *hwpt;
	struct iommufd_ctx *ictx = ucmd->ictx;
	struct iommufd_object *obj;
	unsigned long index;
	int rc = 0;

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
		if (!hwpt->lu_preserve)
			continue;
		if (hwpt->lu_token == cmd->hwpt_token) {
			rc = -EADDRINUSE;
			goto out;
		}
	}

	hwpt_target->lu_preserve = true;
	hwpt_target->lu_token = cmd->hwpt_token;

out:
	xa_unlock(&ictx->objects);
	iommufd_put_object(ictx, &hwpt_target->common.obj);
	return rc;
}

