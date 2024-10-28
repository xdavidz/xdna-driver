// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024, Advanced Micro Devices, Inc.
 */

#include <linux/timekeeping.h>
#include <drm/drm_syncobj.h>

#include "amdxdna_ctx.h"
#include "amdxdna_gem.h"
#include "amdxdna_trace.h"
#include "aie2_pci.h"
#include "aie2_solver.h"
#include "aie2_msg_priv.h"

#ifdef AMDXDNA_DEVEL
#include "amdxdna_devel.h"
#endif

bool force_cmdlist;
module_param(force_cmdlist, bool, 0600);
MODULE_PARM_DESC(force_cmdlist, "Force use command list (Default false)");

static inline int
aie2_hwctx_add_job(struct amdxdna_hwctx *hwctx, struct amdxdna_sched_job *job)
{
	struct amdxdna_sched_job *other;
	int idx;

	idx = get_job_idx(hwctx->submitted);
	/* When pending list full, hwctx->submitted points to oldest fence */
	other = hwctx->priv->pending[idx];
	if (other && other->fence)
		return -EAGAIN;

	if (other) {
		dma_fence_put(other->out_fence);
		amdxdna_job_put(other);
	}

	hwctx->priv->pending[idx] = job;
	job->seq = hwctx->submitted++;
	kref_get(&job->refcnt);

	return 0;
}

static inline struct amdxdna_sched_job *
aie2_hwctx_get_job(struct amdxdna_hwctx *hwctx, u64 seq)
{
	int idx;

	if (seq >= hwctx->submitted)
		return ERR_PTR(-EINVAL);

	if (seq + HWCTX_MAX_CMDS < hwctx->submitted)
		return NULL;

	idx = get_job_idx(seq);
	return hwctx->priv->pending[idx];
}

static void aie2_hwctx_stop(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx,
			    struct drm_sched_job *bad_job)
{
	drm_sched_stop(&hwctx->priv->sched, bad_job);
	aie2_destroy_context(xdna->dev_handle, hwctx);
}

static int aie2_hwctx_restart(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_gem_obj *heap = hwctx->priv->heap;
	int ret;

	ret = aie2_create_context(xdna->dev_handle, hwctx);
	if (ret) {
		XDNA_ERR(xdna, "Create hwctx failed, ret %d", ret);
		goto out;
	}

	ret = aie2_map_host_buf(xdna->dev_handle, hwctx->fw_ctx_id,
				heap->mem.userptr, heap->mem.size);
	if (ret) {
		XDNA_ERR(xdna, "Map host buf failed, ret %d", ret);
		goto out;
	}

	if (hwctx->old_status != HWCTX_STATE_READY) {
		XDNA_DBG(xdna, "hwctx is not ready, status %d", hwctx->status);
		goto out;
	}

#ifdef AMDXDNA_DEVEL
	if (priv_load) {
		ret = aie2_legacy_config_cu(hwctx);
		if (ret) {
			XDNA_ERR(xdna, "Legacy config cu failed, ret %d", ret);
			goto out;
		}
		goto skip_config_cu;
	}
#endif
	ret = aie2_config_cu(hwctx);
	if (ret) {
		XDNA_ERR(xdna, "Config cu failed, ret %d", ret);
		goto out;
	}
#ifdef AMDXDNA_DEVEL
skip_config_cu:
#endif
out:
	/*
	 * Even above commands might failed, we still needs to restart DRM
	 * scheduler, to signal those commands in the pending list.
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	drm_sched_start(&hwctx->priv->sched, true);
#else
	drm_sched_start(&hwctx->priv->sched);
#endif
	XDNA_DBG(xdna, "%s restarted, ret %d", hwctx->name, ret);
	return ret;
}

static const char *
aie2_fence_state2str(struct dma_fence *fence)
{
	if (!fence)
		return "not-exist";
	return dma_fence_is_signaled(fence) ? "signaled" : "unsignaled";
}

static void
aie2_hwctx_dump(struct amdxdna_dev *xdna, struct amdxdna_hwctx *hwctx)
{
	u64 sub = hwctx->submitted;
	u64 comp = hwctx->completed;

	XDNA_ERR(xdna, "Dumping ctx %s, sub=%lld, comp=%lld", hwctx->name, sub, comp);
	for (int i = 0; i < HWCTX_MAX_CMDS; i++) {
		struct amdxdna_sched_job *j = hwctx->priv->pending[i];
		if (!j)
			continue;
		XDNA_ERR(xdna, "JOB[%d]:", i);
		XDNA_ERR(xdna, "\tseq: %lld", j->seq);
		XDNA_ERR(xdna, "\top: 0x%x", j->opcode);
		XDNA_ERR(xdna, "\tmsg: 0x%x", j->msg_id);
		XDNA_ERR(xdna, "\tfence: %s", aie2_fence_state2str(j->fence));
		XDNA_ERR(xdna, "\tout_fence: %s", aie2_fence_state2str(j->out_fence));
	}
}

void aie2_dump_ctx(struct amdxdna_client *client)
{
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_hwctx *hwctx;
	int next = 0;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	mutex_lock(&client->hwctx_lock);
	idr_for_each_entry_continue(&client->hwctx_idr, hwctx, next)
		aie2_hwctx_dump(xdna, hwctx);
	mutex_unlock(&client->hwctx_lock);
}

void aie2_stop_ctx(struct amdxdna_client *client)
{
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_hwctx *hwctx;
	int next = 0;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	mutex_lock(&client->hwctx_lock);
	idr_for_each_entry_continue(&client->hwctx_idr, hwctx, next) {
		aie2_hwctx_stop(xdna, hwctx, NULL);
		hwctx->old_status = hwctx->status;
		hwctx->status = HWCTX_STATE_STOP;
		XDNA_DBG(xdna, "Stop %s", hwctx->name);
	}
	mutex_unlock(&client->hwctx_lock);
}

void aie2_restart_ctx(struct amdxdna_client *client)
{
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_hwctx *hwctx;
	int next = 0;
	int err;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	mutex_lock(&client->hwctx_lock);
	idr_for_each_entry_continue(&client->hwctx_idr, hwctx, next) {
		if (hwctx->status != HWCTX_STATE_STOP)
			continue;

		XDNA_DBG(xdna, "Resetting %s", hwctx->name);
		err = aie2_hwctx_restart(xdna, hwctx);
		if (!err) {
			hwctx->status = hwctx->old_status;
			continue;
		}

		XDNA_WARN(xdna, "Failed to restart %s status %d err %d",
			  hwctx->name, hwctx->status, err);
	}
	mutex_unlock(&client->hwctx_lock);
}

static int aie2_hwctx_wait_for_idle(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_sched_job *job;

	mutex_lock(&hwctx->priv->io_lock);
	if (!hwctx->submitted) {
		mutex_unlock(&hwctx->priv->io_lock);
		return 0;
	}

	job = aie2_hwctx_get_job(hwctx, hwctx->submitted - 1);
	if (IS_ERR_OR_NULL(job)) {
		mutex_unlock(&hwctx->priv->io_lock);
		XDNA_WARN(hwctx->client->xdna, "Corrupted pending list");
		return 0;
	}
	mutex_unlock(&hwctx->priv->io_lock);

	wait_event(hwctx->priv->job_free_wq, !job->fence);

	return 0;
}

void aie2_hwctx_suspend(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;

	/*
	 * Command timeout is unlikely. But if it happens, it doesn't
	 * break the system. aie2_hwctx_stop() will destroy mailbox
	 * and abort all commands.
	 */
	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	aie2_hwctx_wait_for_idle(hwctx);
	aie2_hwctx_stop(xdna, hwctx, NULL);
	hwctx->old_status = hwctx->status;
	hwctx->status = HWCTX_STATE_STOP;
}

void aie2_hwctx_resume(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	int err;

	/*
	 * The resume path cannot guarantee that mailbox channel can be
	 * regenerated. If this happen, when submit message to this
	 * mailbox channel, error will return.
	 */
	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	err = aie2_hwctx_restart(xdna, hwctx);
	if (!err)
		hwctx->status = hwctx->old_status;
	else
		XDNA_WARN(xdna, "Failed to resume %s status %d err %d",
			  hwctx->name, hwctx->status, err);
}

static void
aie2_sched_notify(struct amdxdna_sched_job *job)
{
	struct amdxdna_hwctx *hwctx = job->hwctx;
	struct dma_fence *fence = job->fence;

#ifdef AMDXDNA_DRM_USAGE
	amdxdna_update_stats(hwctx->client, ktime_get(), false);
#endif
	hwctx->completed++;
	trace_xdna_job(&job->base, hwctx->name, "signaling fence", job->seq, job->opcode);
	dma_fence_signal(fence);
	mmput(job->mm);
	amdxdna_job_put(job);
}

static int
aie2_sched_resp_handler(void *handle, const u32 *data, size_t size)
{
	struct amdxdna_sched_job *job = handle;
	struct amdxdna_gem_obj *cmd_abo;
	u32 ret = 0;
	u32 status;

	cmd_abo = job->cmd_bo;

	if (unlikely(!data))
		goto out;

	if (unlikely(size != sizeof(u32))) {
		amdxdna_cmd_set_state(cmd_abo, ERT_CMD_STATE_ABORT);
		ret = -EINVAL;
		goto out;
	}

	status = *data;
	XDNA_DBG(job->hwctx->client->xdna, "Response status 0x%x", status);
	if (status == AIE2_STATUS_SUCCESS)
		amdxdna_cmd_set_state(cmd_abo, ERT_CMD_STATE_COMPLETED);
	else
		amdxdna_cmd_set_state(cmd_abo, ERT_CMD_STATE_ERROR);

out:
	aie2_sched_notify(job);
	return ret;
}

static int
aie2_sched_nocmd_resp_handler(void *handle, const u32 *data, size_t size)
{
	struct amdxdna_sched_job *job = handle;
	u32 ret = 0;
	u32 status;

	if (unlikely(!data))
		goto out;

	if (unlikely(size != sizeof(u32))) {
		ret = -EINVAL;
		goto out;
	}

	status = *data;
	XDNA_DBG(job->hwctx->client->xdna, "Response status 0x%x", status);

out:
	aie2_sched_notify(job);
	return ret;
}

static int
aie2_sched_cmdlist_resp_handler(void *handle, const u32 *data, size_t size)
{
	struct amdxdna_sched_job *job = handle;
	struct amdxdna_gem_obj *cmd_abo;
	struct cmd_chain_resp *resp;
	struct amdxdna_dev *xdna;
	u32 fail_cmd_status;
	u32 fail_cmd_idx;
	u32 ret = 0;

	cmd_abo = job->cmd_bo;
	if (unlikely(!data) || unlikely(size != sizeof(u32) * 3)) {
		amdxdna_cmd_set_state(cmd_abo, ERT_CMD_STATE_ABORT);
		ret = -EINVAL;
		goto out;
	}

	resp = (struct cmd_chain_resp *)data;
	xdna = job->hwctx->client->xdna;
	XDNA_DBG(xdna, "Status 0x%x", resp->status);
	if (resp->status == AIE2_STATUS_SUCCESS) {
		amdxdna_cmd_set_state(cmd_abo, ERT_CMD_STATE_COMPLETED);
		goto out;
	}

	/* Slow path to handle error, read from ringbuf on BAR */
	fail_cmd_idx = resp->fail_cmd_idx;
	fail_cmd_status = resp->fail_cmd_status;
	XDNA_DBG(xdna, "Failed cmd idx %d, status 0x%x",
		 fail_cmd_idx, fail_cmd_status);

	if (fail_cmd_status == AIE2_STATUS_SUCCESS) {
		amdxdna_cmd_set_state(cmd_abo, ERT_CMD_STATE_ABORT);
		ret = -EINVAL;
		goto out;
	}
	amdxdna_cmd_set_state(cmd_abo, fail_cmd_status);

	if (amdxdna_cmd_get_op(cmd_abo) == ERT_CMD_CHAIN) {
		struct amdxdna_cmd_chain *cc = amdxdna_cmd_get_payload(cmd_abo, NULL);

		cc->error_index = fail_cmd_idx;
		if (cc->error_index >= cc->command_count)
			cc->error_index = 0;
	}
out:
	aie2_sched_notify(job);
	return ret;
}

static struct dma_fence *
aie2_sched_job_run(struct drm_sched_job *sched_job)
{
	struct amdxdna_sched_job *job = drm_job_to_xdna_job(sched_job);
	struct amdxdna_gem_obj *cmd_abo = job->cmd_bo;
	struct amdxdna_hwctx *hwctx = job->hwctx;
	struct dma_fence *fence;
	int ret = 0;

	trace_xdna_job(sched_job, hwctx->name, "job run", job->seq, job->opcode);

	if (!mmget_not_zero(job->mm))
		return ERR_PTR(-ESRCH);

	if (!hwctx->priv->mbox_chann)
		return ERR_PTR(-ENODEV);

	kref_get(&job->refcnt);
	fence = dma_fence_get(job->fence);

	switch (job->opcode) {
	case OP_SYNC_BO:
		ret = aie2_sync_bo(hwctx, job, aie2_sched_nocmd_resp_handler);
		goto out;
	case OP_REG_DEBUG_BO:
	case OP_UNREG_DEBUG_BO:
		ret = aie2_config_debug_bo(hwctx, job, aie2_sched_nocmd_resp_handler);
		goto out;
	case OP_NOOP:
		// Call notify since we did not really send it down
		aie2_sched_notify(job);
		goto out;
	}

	if (amdxdna_cmd_get_op(cmd_abo) == ERT_CMD_CHAIN)
		ret = aie2_cmdlist_multi_execbuf(hwctx, job, aie2_sched_cmdlist_resp_handler);
	else if (force_cmdlist)
		ret = aie2_cmdlist_single_execbuf(hwctx, job, aie2_sched_cmdlist_resp_handler);
	else
		ret = aie2_execbuf(hwctx, job, aie2_sched_resp_handler);

out:
	if (ret) {
		dma_fence_put(job->fence);
		amdxdna_job_put(job);
		mmput(job->mm);
		fence = ERR_PTR(ret);
	}
#ifdef AMDXDNA_DRM_USAGE
	else
		amdxdna_update_stats(hwctx->client, ktime_get(), true);
#endif

	return fence;
}

static void aie2_sched_job_free(struct drm_sched_job *sched_job)
{
	struct amdxdna_sched_job *job = drm_job_to_xdna_job(sched_job);
	struct amdxdna_hwctx *hwctx = job->hwctx;

	trace_xdna_job(sched_job, hwctx->name, "job free", job->seq, job->opcode);
	drm_sched_job_cleanup(sched_job);
	dma_fence_put(job->fence);
	job->fence = NULL;
	amdxdna_job_put(job);

	wake_up(&hwctx->priv->job_free_wq);
}

const struct drm_sched_backend_ops sched_ops = {
	.run_job = aie2_sched_job_run,
	.free_job = aie2_sched_job_free,
};

static int aie2_hwctx_col_list(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_dev_hdl *ndev;
	int start, end, first, last;
	u32 width = 1, entries = 0;
	int i;

	if (!hwctx->num_tiles) {
		XDNA_ERR(xdna, "Number of tiles is zero");
		return -EINVAL;
	}

	ndev = xdna->dev_handle;
	if (unlikely(!ndev->metadata.core.row_count)) {
		XDNA_WARN(xdna, "Core tile row count is zero");
		return -EINVAL;
	}

	hwctx->num_col = hwctx->num_tiles / ndev->metadata.core.row_count;
	if (!hwctx->num_col || hwctx->num_col > ndev->total_col) {
		XDNA_ERR(xdna, "Invalid num_col %d", hwctx->num_col);
		return -EINVAL;
	}

	if (ndev->priv->col_align == COL_ALIGN_NATURE)
		width = hwctx->num_col;

#ifdef AMDXDNA_DEVEL
	if (start_col_index >= 0) {
		if (start_col_index + hwctx->num_col > ndev->total_col) {
			XDNA_ERR(xdna, "Invalid start_col_index %d, num col %d",
				 start_col_index, hwctx->num_col);
			return -EINVAL;
		}
		entries = 1;
		first = start_col_index;
		goto skip_list_cal;
	}
#endif
	/*
	 * In range [start, end], find out columns that is multiple of width.
	 *	'first' is the first column,
	 *	'last' is the last column,
	 *	'entries' is the total number of columns.
	 */
	start =  xdna->dev_info->first_col;
	end =  ndev->total_col - hwctx->num_col;
	if (start > 0 && end == 0) {
		XDNA_DBG(xdna, "Force start from col 0");
		start = 0;
	}
	first = start + (width - start % width) % width;
	last = end - end % width;
	if (last >= first)
		entries = (last - first) / width + 1;
	XDNA_DBG(xdna, "start %d end %d first %d last %d",
		 start, end, first, last);

	if (unlikely(!entries)) {
		XDNA_ERR(xdna, "Start %d end %d width %d",
			 start, end, width);
		return -EINVAL;
	}

#ifdef AMDXDNA_DEVEL
skip_list_cal:
#endif
	hwctx->col_list = kmalloc_array(entries, sizeof(*hwctx->col_list), GFP_KERNEL);
	if (!hwctx->col_list)
		return -ENOMEM;

	hwctx->col_list_len = entries;
	hwctx->col_list[0] = first;
	for (i = 1; i < entries; i++)
		hwctx->col_list[i] = hwctx->col_list[i - 1] + width;

	print_hex_dump_debug("col_list: ", DUMP_PREFIX_OFFSET, 16, 4, hwctx->col_list,
			     entries * sizeof(*hwctx->col_list), false);
	return 0;
}

static int aie2_alloc_resource(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct alloc_requests *xrs_req;
	int ret;

	xrs_req = kzalloc(sizeof(*xrs_req), GFP_KERNEL);
	if (!xrs_req)
		return -ENOMEM;

	xrs_req->cdo.start_cols = hwctx->col_list;
	xrs_req->cdo.cols_len = hwctx->col_list_len;
	xrs_req->cdo.ncols = hwctx->num_col;
	xrs_req->cdo.qos_cap.opc = hwctx->max_opc;

	xrs_req->rqos.gops = hwctx->qos.gops;
	xrs_req->rqos.fps = hwctx->qos.fps;
	xrs_req->rqos.dma_bw = hwctx->qos.dma_bandwidth;
	xrs_req->rqos.latency = hwctx->qos.latency;
	xrs_req->rqos.exec_time = hwctx->qos.frame_exec_time;
	xrs_req->rqos.priority = hwctx->qos.priority;

	xrs_req->rid = (uintptr_t)hwctx;

	ret = xrs_allocate_resource(xdna->xrs_hdl, xrs_req, hwctx);
	if (ret)
		XDNA_ERR(xdna, "Allocate AIE resource failed, ret %d", ret);

	kfree(xrs_req);
	return ret;
}

static void aie2_release_resource(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	int ret;

	ret = xrs_release_resource(xdna->xrs_hdl, (uintptr_t)hwctx);
	if (ret)
		XDNA_ERR(xdna, "Release AIE resource failed, ret %d", ret);
}

static void aie2_ctx_syncobj_create(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct drm_file *filp = hwctx->client->filp;
	struct drm_syncobj *syncobj;
	u32 hdl;
	int ret;

	hwctx->priv->syncobj = NULL;
	hwctx->syncobj_hdl = AMDXDNA_INVALID_FENCE_HANDLE;

	ret = drm_syncobj_create(&syncobj, 0, NULL);
	if (ret) {
		XDNA_ERR(xdna, "Create ctx syncobj failed, ret %d", ret);
		return;
	}
	ret = drm_syncobj_get_handle(filp, syncobj, &hdl);
	drm_syncobj_put(syncobj);
	if (ret) {
		XDNA_ERR(xdna, "Create ctx syncobj handle failed, ret %d", ret);
		return;
	}
	hwctx->priv->syncobj = syncobj;
	hwctx->syncobj_hdl = hdl;
}

static void aie2_ctx_syncobj_destroy(struct amdxdna_hwctx *hwctx)
{
	struct drm_file *filp = hwctx->client->filp;
	u32 hdl = hwctx->syncobj_hdl;
	struct drm_syncobj *syncobj;

	if (hdl == AMDXDNA_INVALID_FENCE_HANDLE)
		return;

	hwctx->priv->syncobj = NULL;
	hwctx->syncobj_hdl = AMDXDNA_INVALID_FENCE_HANDLE;

	spin_lock(&filp->syncobj_table_lock);
	syncobj = idr_remove(&filp->syncobj_idr, hdl);
	spin_unlock(&filp->syncobj_table_lock);
	drm_syncobj_put(syncobj);
}

static void aie2_ctx_syncobj_add_fence(struct amdxdna_hwctx *hwctx,
				       struct dma_fence *ofence, u64 seq)
{
	struct drm_syncobj *syncobj = hwctx->priv->syncobj;
	struct dma_fence_chain *chain;

	if (!syncobj)
		return;

	chain = dma_fence_chain_alloc();
	if (!chain)
		return;

	drm_syncobj_add_point(syncobj, chain, ofence, seq);
}

int aie2_hwctx_init(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_dev *xdna = client->xdna;
	struct drm_gpu_scheduler *sched;
	struct amdxdna_hwctx_priv *priv;
	struct amdxdna_gem_obj *heap;
	unsigned int wq_flags;
	int i, ret;

	priv = kzalloc(sizeof(*hwctx->priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	hwctx->priv = priv;

	mutex_lock(&client->mm_lock);
	heap = client->dev_heap;
	if (!heap) {
		XDNA_ERR(xdna, "The client dev heap object not exist");
		mutex_unlock(&client->mm_lock);
		ret = -ENOENT;
		goto free_priv;
	}
	drm_gem_object_get(to_gobj(heap));
	mutex_unlock(&client->mm_lock);
	priv->heap = heap;

	ret = amdxdna_gem_pin(heap);
	if (ret) {
		XDNA_ERR(xdna, "Dev heap pin failed, ret %d", ret);
		goto put_heap;
	}

	for (i = 0; i < ARRAY_SIZE(priv->cmd_buf); i++) {
		struct amdxdna_gem_obj *abo;
		struct amdxdna_drm_create_bo args = {
			.flags = 0,
			.type = AMDXDNA_BO_DEV,
			.vaddr = 0,
			.size = MAX_CHAIN_CMDBUF_SIZE,
		};

		abo = amdxdna_drm_alloc_dev_bo(&xdna->ddev, &args, client->filp, true);
		if (IS_ERR(abo)) {
			ret = PTR_ERR(abo);
			goto free_cmd_bufs;
		}

		XDNA_DBG(xdna, "Command buf %d addr 0x%llx size 0x%lx",
			 i, abo->mem.dev_addr, abo->mem.size);
		priv->cmd_buf[i] = abo;
	}

	sched = &priv->sched;
	mutex_init(&priv->io_lock);

	wq_flags = __WQ_ORDERED;
	if (!aie2_pm_is_turbo(xdna->dev_handle))
		wq_flags |= WQ_UNBOUND;
	priv->submit_wq = alloc_workqueue(hwctx->name, wq_flags, 1);
	if (!priv->submit_wq) {
		XDNA_ERR(xdna, "Failed to alloc submit wq");
		goto free_cmd_bufs;
	}
	ret = drm_sched_init(sched, &sched_ops, priv->submit_wq, DRM_SCHED_PRIORITY_COUNT,
			     HWCTX_MAX_CMDS, 0, MAX_SCHEDULE_TIMEOUT,
			     NULL, NULL, hwctx->name, xdna->ddev.dev);
	if (ret) {
		XDNA_ERR(xdna, "Failed to init DRM scheduler. ret %d", ret);
		goto free_wq;
	}

	ret = drm_sched_entity_init(&priv->entity, DRM_SCHED_PRIORITY_NORMAL,
				    &sched, 1, NULL);
	if (ret) {
		XDNA_ERR(xdna, "Failed to initial sched entiry. ret %d", ret);
		goto free_sched;
	}
	init_waitqueue_head(&priv->job_free_wq);

	ret = aie2_hwctx_col_list(hwctx);
	if (ret) {
		XDNA_ERR(xdna, "Create col list failed, ret %d", ret);
		goto free_entity;
	}

	ret = aie2_alloc_resource(hwctx);
	if (ret) {
		XDNA_ERR(xdna, "Alloc hw resource failed, ret %d", ret);
		goto free_col_list;
	}

#ifdef AMDXDNA_DEVEL
	if (iommu_mode == AMDXDNA_IOMMU_NO_PASID) {
		ret = aie2_map_host_buf(xdna->dev_handle, hwctx->fw_ctx_id,
					heap->mem.dma_addr, heap->mem.size);
		goto skip;
	}
#endif
	ret = aie2_map_host_buf(xdna->dev_handle, hwctx->fw_ctx_id,
				heap->mem.userptr, heap->mem.size);
#ifdef AMDXDNA_DEVEL
skip:
#endif
	if (ret) {
		XDNA_ERR(xdna, "Map host buffer failed, ret %d", ret);
		goto release_resource;
	}

	aie2_ctx_syncobj_create(hwctx);
	hwctx->status = HWCTX_STATE_INIT;

	XDNA_DBG(xdna, "hwctx %s init completed", hwctx->name);

	return 0;

release_resource:
	aie2_release_resource(hwctx);
free_col_list:
	kfree(hwctx->col_list);
free_entity:
	drm_sched_entity_destroy(&priv->entity);
free_sched:
	drm_sched_fini(&priv->sched);
free_wq:
	destroy_workqueue(priv->submit_wq);
free_cmd_bufs:
	for (i = 0; i < ARRAY_SIZE(priv->cmd_buf); i++) {
		if (!priv->cmd_buf[i])
			continue;
		drm_gem_object_put(to_gobj(priv->cmd_buf[i]));
	}
	amdxdna_gem_unpin(heap);
put_heap:
	drm_gem_object_put(to_gobj(heap));
free_priv:
	kfree(priv);
	return ret;
}

void aie2_hwctx_fini(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_sched_job *job;
	struct amdxdna_dev *xdna;
	int idx;

	aie2_ctx_syncobj_destroy(hwctx);

	xdna = hwctx->client->xdna;
	drm_sched_wqueue_stop(&hwctx->priv->sched);

	/* Now, scheduler will not send command to device. */
	aie2_release_resource(hwctx);

	/*
	 * All submitted commands are aborted.
	 * Restart scheduler queues to cleanup jobs. The amdxdna_sched_job_run()
	 * will return NODEV if it is called.
	 */
	drm_sched_wqueue_start(&hwctx->priv->sched);

	aie2_hwctx_wait_for_idle(hwctx);
	drm_sched_entity_destroy(&hwctx->priv->entity);
	drm_sched_fini(&hwctx->priv->sched);
	destroy_workqueue(hwctx->priv->submit_wq);

	for (idx = 0; idx < HWCTX_MAX_CMDS; idx++) {
		job = hwctx->priv->pending[idx];
		if (!job)
			continue;

		dma_fence_put(job->out_fence);
		amdxdna_job_put(job);
	}
	XDNA_DBG(xdna, "%s total completed jobs %lld", hwctx->name, hwctx->completed);

	for (idx = 0; idx < ARRAY_SIZE(hwctx->priv->cmd_buf); idx++)
		drm_gem_object_put(to_gobj(hwctx->priv->cmd_buf[idx]));
	amdxdna_gem_unpin(hwctx->priv->heap);
	drm_gem_object_put(to_gobj(hwctx->priv->heap));
#ifdef AMDXDNA_DEVEL
	if (priv_load)
		aie2_unregister_pdis(hwctx);
#endif

	mutex_destroy(&hwctx->priv->io_lock);
	kfree(hwctx->col_list);
	kfree(hwctx->priv);
	kfree(hwctx->cus);
}

static int aie2_hwctx_cu_config(struct amdxdna_hwctx *hwctx, void *buf, u32 size)
{
	struct amdxdna_hwctx_param_config_cu *config = buf;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	u32 total_size;
	int ret;

	XDNA_DBG(xdna, "Config %d CU to %s", config->num_cus, hwctx->name);
	if (hwctx->status != HWCTX_STATE_INIT) {
		XDNA_ERR(xdna, "Not support re-config CU");
		return -EINVAL;
	}

	if (!config->num_cus) {
		XDNA_ERR(xdna, "Number of CU is zero");
		return -EINVAL;
	}

	total_size = struct_size(config, cu_configs, config->num_cus);
	if (total_size > size) {
		XDNA_ERR(xdna, "CU config larger than size");
		return -EINVAL;
	}

	hwctx->cus = kmemdup(config, total_size, GFP_KERNEL);
	if (!hwctx->cus)
		return -ENOMEM;

#ifdef AMDXDNA_DEVEL
	if (priv_load) {
		ret = aie2_register_pdis(hwctx);
		if (ret) {
			XDNA_ERR(xdna, "Register PDIs failed, ret %d", ret);
			goto free_cus;
		}

		ret = aie2_legacy_config_cu(hwctx);
		if (ret) {
			XDNA_ERR(xdna, "Legacy config cu failed, ret %d", ret);
			aie2_unregister_pdis(hwctx);
			goto free_cus;
		}

		goto skip_config_cu;
	}
#endif
	ret = aie2_config_cu(hwctx);
	if (ret) {
		XDNA_ERR(xdna, "Configu CU to firmware failed, ret %d", ret);
		goto free_cus;
	}

#ifdef AMDXDNA_DEVEL
skip_config_cu:
#endif
	wmb(); /* To avoid locking in command submit when check status */
	hwctx->status = HWCTX_STATE_READY;

	return 0;

free_cus:
	kfree(hwctx->cus);
	hwctx->cus = NULL;
	return ret;
}

static int aie2_hwctx_attach_debug_bo(struct amdxdna_hwctx *hwctx, u32 bo_hdl)
{
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_gem_obj *abo;
	u64 seq;
	int ret;

	abo = amdxdna_gem_get_obj(client, bo_hdl, AMDXDNA_BO_DEV);
	if (!abo) {
		XDNA_ERR(xdna, "Get bo %d failed", bo_hdl);
		ret = -EINVAL;
		goto err_out;
	}

	ret = amdxdna_gem_set_assigned_hwctx(client, bo_hdl, hwctx->id);
	if (ret) {
		XDNA_ERR(xdna, "Failed to attach debug BO %d to %s: %d", bo_hdl, hwctx->name, ret);
		goto put_obj;
	}

	ret = amdxdna_cmd_submit(client, OP_REG_DEBUG_BO, AMDXDNA_INVALID_BO_HANDLE,
				 &bo_hdl, 1, NULL, NULL, 0, hwctx->id, &seq);
	if (ret) {
		XDNA_ERR(xdna, "Submit command failed");
		goto clear_ctx;
	}

	ret = amdxdna_cmd_wait(client, hwctx->id, seq, 3000 /* ms */);
	if (ret)
		goto clear_ctx;
	XDNA_DBG(xdna, "Attached debug BO %d to %s", bo_hdl, hwctx->name);
	amdxdna_gem_put_obj(abo);
	return 0;

clear_ctx:
	amdxdna_gem_clear_assigned_hwctx(client, bo_hdl);
put_obj:
	amdxdna_gem_put_obj(abo);
err_out:
	return ret;
}

static int aie2_hwctx_detach_debug_bo(struct amdxdna_hwctx *hwctx, u32 bo_hdl)
{
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_dev *xdna = client->xdna;
	u64 seq;
	int ret;

	if (amdxdna_gem_get_assigned_hwctx(client, bo_hdl) != hwctx->id) {
		XDNA_ERR(xdna, "Debug BO %d isn't attached to %s", bo_hdl, hwctx->name);
		return -EINVAL;
	}

	amdxdna_gem_clear_assigned_hwctx(client, bo_hdl);

	ret = amdxdna_cmd_submit(client, OP_UNREG_DEBUG_BO, AMDXDNA_INVALID_BO_HANDLE,
				 &bo_hdl, 1, NULL, NULL, 0, hwctx->id, &seq);
	if (unlikely(ret)) {
		XDNA_ERR(xdna, "Submit command failed");
		return ret;
	}

	ret = amdxdna_cmd_wait(client, hwctx->id, seq, 3000 /* ms */);
	XDNA_DBG(xdna, "Detached debug BO %d from %s, ret %d", bo_hdl, hwctx->name, ret);
	return ret;
}

int aie2_hwctx_config(struct amdxdna_hwctx *hwctx, u32 type, u64 value, void *buf, u32 size)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	switch (type) {
	case DRM_AMDXDNA_HWCTX_CONFIG_CU:
		return aie2_hwctx_cu_config(hwctx, buf, size);
	case DRM_AMDXDNA_HWCTX_ASSIGN_DBG_BUF:
		return aie2_hwctx_attach_debug_bo(hwctx, (u32)value);
	case DRM_AMDXDNA_HWCTX_REMOVE_DBG_BUF:
		return aie2_hwctx_detach_debug_bo(hwctx, (u32)value);
	default:
		XDNA_DBG(xdna, "Not supported type %d", type);
		return -EOPNOTSUPP;
	}
}

static int aie2_populate_range(struct amdxdna_gem_obj *abo)
{
	struct amdxdna_dev *xdna = to_xdna_dev(to_gobj(abo)->dev);
	struct mm_struct *mm = abo->mem.notifier.mm;
	struct hmm_range range = { 0 };
	unsigned long timeout;
	int ret;

	XDNA_INFO_ONCE(xdna, "populate memory range %llx size %lx",
		       abo->mem.userptr, abo->mem.size);
	range.notifier = &abo->mem.notifier;
	range.start = abo->mem.userptr;
	range.end = abo->mem.userptr + abo->mem.size;
	range.hmm_pfns = abo->mem.pfns;
	range.default_flags = HMM_PFN_REQ_FAULT;

	if (!mmget_not_zero(mm))
		return -EFAULT;

	timeout = jiffies + msecs_to_jiffies(HMM_RANGE_DEFAULT_TIMEOUT);
again:
	range.notifier_seq = mmu_interval_read_begin(&abo->mem.notifier);
	mmap_read_lock(mm);
	ret = hmm_range_fault(&range);
	mmap_read_unlock(mm);
	if (ret) {
		if (time_after(jiffies, timeout)) {
			ret = -ETIME;
			goto put_mm;
		}

		if (ret == -EBUSY)
			goto again;

		goto put_mm;
	}

	mutex_lock(&abo->mem.notify_lock);
	if (mmu_interval_read_retry(&abo->mem.notifier, range.notifier_seq)) {
		mutex_unlock(&abo->mem.notify_lock);
		goto again;
	}
	abo->mem.map_invalid = false;
	mutex_unlock(&abo->mem.notify_lock);

put_mm:
	mmput(mm);
	return ret;
}

static int aie2_add_job_dependency(struct amdxdna_sched_job *job, u32 *syncobj_hdls,
				   u64 *syncobj_points, u32 syncobj_cnt)
{
	struct amdxdna_client *client = job->hwctx->client;
	int ret = 0;
	u32 hdl;
	u64 pt;
	int i;

	for (i = 0; ret == 0 && i < syncobj_cnt; i++) {
		hdl = syncobj_hdls[i];
		pt = syncobj_points[i];
		ret = drm_sched_job_add_syncobj_dependency(&job->base, client->filp, hdl, pt);
		if (ret) {
			XDNA_ERR(client->xdna,
				 "Failed to add syncobj (%d@%lld) as dependency, ret %d",
				 hdl, pt, ret);
		}
	}
	return ret;
}

int aie2_cmd_submit(struct amdxdna_hwctx *hwctx, struct amdxdna_sched_job *job,
		    u32 *syncobj_hdls, u64 *syncobj_points, u32 syncobj_cnt, u64 *seq)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct ww_acquire_ctx acquire_ctx;
	struct amdxdna_gem_obj *abo;
	unsigned long timeout = 0;
	struct dma_fence *ofence;
	int ret, i;

	ret = drm_sched_job_init(&job->base, &hwctx->priv->entity, 1, hwctx);
	if (ret) {
		XDNA_ERR(xdna, "DRM job init failed, ret %d", ret);
		return ret;
	}

	drm_sched_job_arm(&job->base);
	job->out_fence = dma_fence_get(&job->base.s_fence->finished);

	ret = aie2_add_job_dependency(job, syncobj_hdls, syncobj_points, syncobj_cnt);
	if (ret) {
		XDNA_ERR(xdna, "Failed to add dependency, ret %d", ret);
		goto put_fence;
	}

retry:
	ret = amdxdna_lock_objects(job, &acquire_ctx);
	if (ret) {
		XDNA_WARN(xdna, "Failed to lock objects, ret %d", ret);
		goto put_fence;
	}

	for (i = 0; i < job->bo_cnt; i++) {
		abo = to_xdna_obj(job->bos[i].obj);
		ret = dma_resv_reserve_fences(job->bos[i].obj->resv, 1);
		if (ret) {
			XDNA_WARN(xdna, "Failed to reserve fences %d", ret);
			amdxdna_unlock_objects(job, &acquire_ctx);
			goto put_fence;
		}

		mutex_lock(&abo->mem.notify_lock);
		if (abo->mem.map_invalid) {
			mutex_unlock(&abo->mem.notify_lock);
			amdxdna_unlock_objects(job, &acquire_ctx);
			if (!timeout) {
				timeout = jiffies +
					msecs_to_jiffies(HMM_RANGE_DEFAULT_TIMEOUT);
			} else if (time_after(jiffies, timeout)) {
				ret = -ETIME;
				goto put_fence;
			}

			ret = aie2_populate_range(abo);
			if (ret)
				goto put_fence;
			goto retry;
		}

		dma_resv_add_fence(job->bos[i].obj->resv, job->out_fence, DMA_RESV_USAGE_WRITE);
		mutex_unlock(&abo->mem.notify_lock);
	}

	amdxdna_unlock_objects(job, &acquire_ctx);

again:
	mutex_lock(&hwctx->priv->io_lock);
	ret = aie2_hwctx_add_job(hwctx, job);
	if (ret) {
		mutex_unlock(&hwctx->priv->io_lock);
		if (ret == -EAGAIN) {
			// Waiting for the first pending cmd to complete before trying again.
			aie2_cmd_wait(hwctx, hwctx->submitted - HWCTX_MAX_CMDS, 0);
			goto again;
		}
		goto signal_fence;
	}

	*seq = job->seq;
	ofence = dma_fence_get(job->out_fence);

	drm_sched_entity_push_job(&job->base);
	mutex_unlock(&hwctx->priv->io_lock);

	aie2_ctx_syncobj_add_fence(hwctx, ofence, *seq);
	dma_fence_put(ofence);
	return 0;

signal_fence:
	dma_fence_signal(job->out_fence);
put_fence:
	dma_fence_put(job->out_fence);
	drm_sched_job_cleanup(&job->base);
	return ret;
}

struct dma_fence *aie2_cmd_get_out_fence(struct amdxdna_hwctx *hwctx, u64 seq)
{
	struct amdxdna_sched_job *job;
	struct dma_fence *out_fence;

	mutex_lock(&hwctx->priv->io_lock);
	job = aie2_hwctx_get_job(hwctx, seq);
	if (IS_ERR_OR_NULL(job)) {
		mutex_unlock(&hwctx->priv->io_lock);
		return ERR_CAST(job);
	}

	out_fence = dma_fence_get(job->out_fence);
	mutex_unlock(&hwctx->priv->io_lock);
	return out_fence;
}

int aie2_cmd_wait(struct amdxdna_hwctx *hwctx, u64 seq, u32 timeout)
{
	struct dma_fence *out_fence = aie2_cmd_get_out_fence(hwctx, seq);
	signed long remaining = MAX_SCHEDULE_TIMEOUT;
	long ret;

	if (timeout)
		remaining = msecs_to_jiffies(timeout);
	ret = dma_fence_wait_timeout(out_fence, true, remaining);
	if (!ret)
		ret = -ETIME;
	else if (ret > 0)
		ret = 0;
	dma_fence_put(out_fence);
	return ret;
}

void aie2_hmm_invalidate(struct amdxdna_gem_obj *abo,
			 unsigned long cur_seq)
{
	struct amdxdna_dev *xdna = to_xdna_dev(to_gobj(abo)->dev);
	struct drm_gem_object *gobj = to_gobj(abo);
	long ret;

	mutex_lock(&abo->mem.notify_lock);
	abo->mem.map_invalid = true;
	mmu_interval_set_seq(&abo->mem.notifier, cur_seq);
	mutex_unlock(&abo->mem.notify_lock);
	ret = dma_resv_wait_timeout(gobj->resv, DMA_RESV_USAGE_BOOKKEEP,
				    true, MAX_SCHEDULE_TIMEOUT);
	if (!ret || ret == -ERESTARTSYS)
		XDNA_ERR(xdna, "Failed to wait for bo, ret %ld", ret);
}

int aie2_xrs_load_hwctx(struct amdxdna_hwctx *hwctx, struct xrs_action_load *action)
{
	struct amdxdna_dev *xdna;
	int ret;

	xdna = hwctx->client->xdna;

	hwctx->start_col = action->part.start_col;
	hwctx->num_col = action->part.ncols;
	ret = aie2_create_context(xdna->dev_handle, hwctx);
	if (ret)
		XDNA_ERR(xdna, "create context failed, ret %d", ret);

	return ret;
}

int aie2_xrs_unload_hwctx(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev *xdna;
	int ret;

	xdna = hwctx->client->xdna;

	ret = aie2_destroy_context(xdna->dev_handle, hwctx);
	if (ret)
		XDNA_ERR(xdna, "destroy context failed, ret %d", ret);

	return ret;
}
