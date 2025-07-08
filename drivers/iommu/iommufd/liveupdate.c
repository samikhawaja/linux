// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/liveupdate.h>

#include "iommufd_private.h"

int iommufd_hwpt_lu_mark_preserve(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_lu_mark_preserve *cmd = ucmd->cmd;
	struct iommufd_hwpt_paging *hwpt_target;
	struct iommufd_ctx *ictx = ucmd->ictx;
	void *curr;
	int rc = 0;

	hwpt_target = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt_target))
		return PTR_ERR(hwpt_target);

	/*
	 * Use xa_cmpxchg to safely store only if the token is not already in
	 * use.
	 */
	curr = xa_cmpxchg(&ictx->liveupdate_tokens, cmd->hwpt_token, NULL,
			  hwpt_target, GFP_KERNEL);
	if (xa_is_err(curr)) {
		rc = xa_err(curr);
		goto err;
	} else if (curr) {
		rc = -EADDRINUSE;
		goto err;
	}

	hwpt_target->liveupdate_preserve = true;
	hwpt_target->liveupdate_token = cmd->hwpt_token;

	/*
	 * liveupdate_tokens xarray still holds a reference to the HWPT to make
	 * sure it is not destroyed.
	 */
	return 0;
err:
	iommufd_put_object(ictx, &hwpt_target->common.obj);
	return rc;
}

