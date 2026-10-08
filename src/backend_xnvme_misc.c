/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file backend_xnvme_misc.c
 * @brief xNVMe Backend: queries, device ops, backend vtable, registration
 *
 * Split from backend_xnvme.c (mechanical move, no behavior change).
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cs_internal.h"
#include "cs_programs.h"

/* xNVMe headers */
#include <libxnvme.h>
#include <libxnvme_nvm.h>
#include <libxnvme_slm.h>
#include <libxnvme_cs.h>

#include "backend_xnvme_internal.h"

csStatus
xnvme_backend_prog_get_name(struct cs_backend_ctx *ctx, uint16_t prog_index,
			    char *name, size_t name_size)
{
	(void)ctx;

	const char *prog_name = cs_prog_name_from_pind(prog_index);
	if (!prog_name) {
		return CS_STATUS_ERROR_NOT_FOUND;
	}
	snprintf(name, name_size, "%s", prog_name);
	return CS_STATUS_SUCCESS;
}

csStatus
xnvme_backend_query_csf_list(struct cs_backend_ctx *ctx, csCSFInfo *csfs, uint32_t *count)
{
	(void)ctx;
	if (!count) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	uint32_t total;
	const cs_csf_entry_t *tbl = cs_csf_table(&total);

	if (!csfs) {
		*count = total;
		return CS_STATUS_SUCCESS;
	}

	if (*count < total) {
		*count = total;
		return CS_STATUS_ERROR_LIMIT;
	}

	for (uint32_t i = 0; i < total; i++) {
		memset(&csfs[i], 0, sizeof(csfs[i]));
		snprintf(csfs[i].name, sizeof(csfs[i].name), "%s", tbl[i].name);
		csfs[i].version = 1;
	}
	*count = total;
	return CS_STATUS_SUCCESS;
}

csStatus
xnvme_backend_query_device_properties(struct cs_backend_ctx *ctx,
				       csDeviceProperties *props)
{
	if (!ctx || !props) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	csDeviceInfo info;
	csStatus status = xnvme_backend_dev_get_info(ctx, &info);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	memset(props, 0, sizeof(*props));
	props->num_cses = 1;
	props->num_csees = 1;
	props->fdm_total_bytes = 0;
	props->afdm_total_bytes = (size_t)info.max_memory_mb * 1024 * 1024;
	props->max_concurrent_progs = info.max_programs;
	props->csf_count = 15;

	return CS_STATUS_SUCCESS;
}

csStatus
xnvme_backend_query_device_statistics(struct cs_backend_ctx *ctx,
				     csDeviceStatistics *stats)
{
	if (!ctx || !stats) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	memset(stats, 0, sizeof(*stats));
	return CS_STATUS_ERROR_NOT_SUPPORTED;
}

csStatus
xnvme_backend_device_config(struct cs_backend_ctx *ctx, uint32_t config_id,
			   const void *data, size_t size)
{
	if (!ctx || !data || size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	(void)config_id;
	return CS_STATUS_ERROR_NOT_SUPPORTED;
}

csStatus
xnvme_backend_device_reset(struct cs_backend_ctx *ctx, uint32_t reset_type)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	(void)reset_type;
	return CS_STATUS_ERROR_NOT_SUPPORTED;
}

csStatus
xnvme_backend_batch_submit(struct cs_backend_ctx *ctx, csBatchHandle batch,
			   csComputeCallback callback, csEventHandle event)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_batch_ctx_t *bctx;
	csStatus status = cs_batch_lookup(batch, &bctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	cs_event_ctx_t *evt_ctx = NULL;
	if (event != CS_INVALID_HANDLE) {
		csStatus ev_status = cs_event_lookup(event, &evt_ctx);
		if (ev_status != CS_STATUS_SUCCESS) {
			cs_batch_release(bctx);
			return ev_status;
		}
	}

	/* ponytail: sequential submit — replace with single SPDK batch API
	 * call when the target gains native batch support.  Each entry gets
	 * a fresh completion flag so synchronous compute_execute doesn't see
	 * a stale "done" from the previous entry. */
	for (uint32_t i = 0; i < bctx->entry_count; i++) {
		const csRequest *req = &bctx->entries[i];
		volatile int entry_comp = 0;

		switch (req->type) {
		case CS_REQ_TYPE_COMPUTE: {
			uint16_t pind = cs_prog_index_from_csf(
				req->u.compute.desc.csf_id);
			status = xnvme_backend_compute_execute(ctx, pind,
				req->u.compute.desc.input_buffer,
				req->u.compute.desc.input_size,
				req->u.compute.desc.output_buffer,
				req->u.compute.desc.output_size,
				req->u.compute.desc.args,
				req->u.compute.desc.args_size,
				&entry_comp);
			break;
		}
		case CS_REQ_TYPE_STORAGE: {
			cs_mem_ctx_t *mem_ctx;
			status = cs_mem_lookup(req->u.storage.mem, &mem_ctx);
			if (status != CS_STATUS_SUCCESS) {
				goto batch_done;
			}
			status = xnvme_backend_storage_io(ctx,
				req->u.storage.op,
				req->u.storage.lba,
				req->u.storage.blocks,
				mem_ctx->ptr, mem_ctx->size,
				&entry_comp);
			cs_mem_release(mem_ctx);
			break;
		}
		case CS_REQ_TYPE_COPY_MEM:
			if (req->u.copy.src && req->u.copy.dst &&
			    req->u.copy.size > 0) {
				memcpy(req->u.copy.dst, req->u.copy.src,
				       req->u.copy.size);
			} else {
				status = CS_STATUS_ERROR_INVALID_PARAM;
			}
			break;
		default:
			status = CS_STATUS_ERROR_INVALID_PARAM;
			break;
		}

		if (status != CS_STATUS_SUCCESS) {
			break;
		}
	}

batch_done:
	if (evt_ctx) {
		evt_ctx->completion_flag = (status == CS_STATUS_SUCCESS) ? 1 : -1;
		atomic_store(&evt_ctx->status,
			status == CS_STATUS_SUCCESS
				? CS_EVENT_STATUS_COMPLETE
				: CS_EVENT_STATUS_ERROR);
		atomic_store(&evt_ctx->result, status);
		cs_event_release(evt_ctx);
	}

	cs_batch_release(bctx);

	if (callback) {
		callback(status, NULL);
	}

	return status;
}

/**
 * Backend operations vtable
 */
static const cs_backend_ops_t xnvme_backend_ops = {
	.name = "xnvme",

	.init = xnvme_backend_init,
	.shutdown = xnvme_backend_shutdown,

	.dev_query = xnvme_backend_dev_query,
	.dev_open = xnvme_backend_dev_open,
	.dev_close = xnvme_backend_dev_close,
	.dev_get_info = xnvme_backend_dev_get_info,

	.mem_alloc = xnvme_backend_mem_alloc,
	.mem_free = xnvme_backend_mem_free,
	.mem_init = xnvme_backend_mem_init,

	.prog_load = xnvme_backend_prog_load,
	.prog_activate = xnvme_backend_prog_activate,
	.prog_deactivate = xnvme_backend_prog_deactivate,
	.prog_get_name = xnvme_backend_prog_get_name,
	.query_csf_list = xnvme_backend_query_csf_list,

	.compute_execute = xnvme_backend_compute_execute,
	.compute_execute_nvm = xnvme_backend_compute_execute_nvm,
	.compute_submit = xnvme_backend_compute_submit,
	.compute_reap = xnvme_backend_compute_reap,
	.abort_request = xnvme_backend_abort_request,
	.storage_io = xnvme_backend_storage_io,
	.poll = xnvme_backend_poll,

	.batch_submit = xnvme_backend_batch_submit,

	.query_device_properties = xnvme_backend_query_device_properties,
	.query_device_statistics = xnvme_backend_query_device_statistics,
	.device_config = xnvme_backend_device_config,
	.device_reset = xnvme_backend_device_reset,
};

/**
 * Register the xNVMe backend
 */
void
cs_backend_xnvme_register(void)
{
	csStatus status = cs_backend_register(&xnvme_backend_ops);
	if (status != CS_STATUS_SUCCESS && status != CS_STATUS_ERROR_ALREADY_EXISTS) {
		CS_LOG_ERROR("Failed to register xNVMe backend: %d", status);
	}
}
