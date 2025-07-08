// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/liveupdate.h>

#include "iommufd_private.h"

int iommufd_hwpt_liveupdate_mark_preserve(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_liveupdate_mark_preserve *cmd = ucmd->cmd;
	struct iommufd_ctx *ictx = ucmd->ictx;
	struct iommufd_hwpt_paging *hwpt;
	struct iommufd_object *obj;
	unsigned long index;
	bool marked = false;
	int rc = 0;

	hwpt = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt))
		return PTR_ERR(hwpt);

	mutex_lock(&ictx->liveupdate_mutex);

	xa_lock(&ictx->objects);

	/* PRI use cases are not supported. */
	if (hwpt->common.fault) {
		rc = -EOPNOTSUPP;
		goto out_unlock;
	}

	xa_for_each_marked(&ictx->objects, index, obj, IOMMUFD_OBJ_LIVEUPDATE_MARK) {
		struct iommufd_hwpt_paging *hwpt_marked;

		if (WARN_ON_ONCE(obj->type != IOMMUFD_OBJ_HWPT_PAGING))
			continue;

		hwpt_marked = to_hwpt_paging(container_of(obj, struct iommufd_hw_pagetable, obj));

		if (hwpt_marked == hwpt)
			marked = true;

		if (hwpt_marked->liveupdate_token == cmd->hwpt_token) {
			if (hwpt_marked == hwpt)
				goto out_unlock;

			rc = -EADDRINUSE;
			goto out_unlock;
		}
	}

	__xa_set_mark(&ictx->objects, hwpt->common.obj.id, IOMMUFD_OBJ_LIVEUPDATE_MARK);

	if (marked)
		pr_warn_ratelimited("Overwriting HWPT liveupdate token from: %llu to %llu\n",
				    hwpt->liveupdate_token, cmd->hwpt_token);
	hwpt->liveupdate_token = cmd->hwpt_token;

out_unlock:
	xa_unlock(&ictx->objects);
	mutex_unlock(&ictx->liveupdate_mutex);
	iommufd_put_object(ictx, &hwpt->common.obj);
	return rc;
}
