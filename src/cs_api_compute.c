/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_api_compute.c
 * @brief CS API Compute Pipeline, Batch and Event Implementation
 */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_internal.h"
#include "cs_programs.h"

/**
 * Compute Operations
 */
csStatus
csQueueComputeRequest(csCSxHandle csx_handle, const csComputeDesc *desc,
		      csEventHandle event, csComputeCallback callback,
		      void *user_data)
{
	if (!desc) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!dev_ctx->backend || !dev_ctx->backend->compute_execute) {
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	/* Get event context if provided */
	cs_event_ctx_t *evt_ctx = NULL;
	if (event != CS_INVALID_HANDLE) {
		status = cs_event_lookup(event, &evt_ctx);
		if (status != CS_STATUS_SUCCESS) {
			cs_device_release(dev_ctx);
			return status;
		}
	}

	uint16_t prog_index = cs_prog_index_from_csf(desc->csf_id);

	volatile int *completion = evt_ctx ? (volatile int *)(void *)&evt_ctx->completion_flag : NULL;

	if (desc->nvm_nsid > 0) {
		if (!dev_ctx->backend->compute_execute_nvm) {
			cs_device_release(dev_ctx);
			return CS_STATUS_ERROR_NOT_SUPPORTED;
		}
		status = dev_ctx->backend->compute_execute_nvm(
			dev_ctx->backend_ctx,
			prog_index,
			desc->nvm_nsid, desc->nvm_slba,
			desc->slm_nsid, desc->slm_byte_off,
			desc->input_size,
			desc->output_buffer, desc->output_size,
			desc->threshold, desc->batch,
			completion);
		cs_device_release(dev_ctx);
		return status;
	}

	bool async_mode = (event != CS_INVALID_HANDLE || callback != NULL);

	if (async_mode && dev_ctx->backend->compute_submit && completion) {
		status = dev_ctx->backend->compute_submit(
			dev_ctx->backend_ctx,
			prog_index,
			desc->input_buffer, desc->input_size,
			desc->output_buffer, desc->output_size,
			desc->args, desc->args_size,
			completion);

		if (status == CS_STATUS_PENDING) {
			if (evt_ctx) {
				evt_ctx->callback = callback;
				evt_ctx->user_data = user_data;
				evt_ctx->callback_invoked = false;
				cs_event_release(evt_ctx);
			}
			cs_device_release(dev_ctx);
			return CS_STATUS_PENDING;
		}

		if (evt_ctx) {
			atomic_store(&evt_ctx->status, status == CS_STATUS_SUCCESS ?
				     CS_EVENT_STATUS_COMPLETE : CS_EVENT_STATUS_ERROR);
			atomic_store(&evt_ctx->result, status);
			cs_event_release(evt_ctx);
		}
		cs_device_release(dev_ctx);
		if (callback) {
			callback(status, user_data);
		}
		return status;
	}

	status = dev_ctx->backend->compute_execute(
		dev_ctx->backend_ctx,
		prog_index,
		desc->input_buffer, desc->input_size,
		desc->output_buffer, desc->output_size,
		desc->args, desc->args_size,
		completion);

	/* For async operation with event */
	if (status == CS_STATUS_PENDING && evt_ctx) {
		/* Completion callback won't release these; release them now */
		cs_device_release(dev_ctx);
		return CS_STATUS_PENDING;
	}

	/* For sync operation or immediate completion */
	if (evt_ctx) {
		atomic_store(&evt_ctx->status, status == CS_STATUS_SUCCESS ?
			     CS_EVENT_STATUS_COMPLETE : CS_EVENT_STATUS_ERROR);
		atomic_store(&evt_ctx->result, status);
		cs_event_release(evt_ctx);
	}

	cs_device_release(dev_ctx);

	/* Call callback if provided */
	if (callback) {
		callback(status, user_data);
	}

	return status;
}

csStatus
csHelperSetComputeArg(void *args_buffer, size_t args_size,
		      uint32_t arg_index, const void *arg_value,
		      size_t value_size)
{
	if (!args_buffer || !arg_value) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	/* Simple linear argument layout: each arg is 8-byte aligned */
	size_t offset = (size_t)arg_index * 8;

	if (offset < arg_index || offset + value_size < offset ||
	    offset + value_size > args_size) {
		return CS_STATUS_ERROR_LIMIT;
	}

	memcpy((uint8_t *)args_buffer + offset, arg_value, value_size);
	return CS_STATUS_SUCCESS;
}

csStatus
csAbortRequest(csCSxHandle csx_handle, csEventHandle event)
{
	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	cs_event_ctx_t *evt_ctx;
	status = cs_event_lookup(event, &evt_ctx);
	if (status != CS_STATUS_SUCCESS) {
		cs_device_release(dev_ctx);
		return status;
	}

	if (dev_ctx->backend && dev_ctx->backend->abort_request) {
		status = dev_ctx->backend->abort_request(dev_ctx->backend_ctx,
							 (volatile int *)(void *)&evt_ctx->completion_flag);
	} else {
		status = CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	if (status == CS_STATUS_SUCCESS) {
		atomic_store(&evt_ctx->status, CS_EVENT_STATUS_ERROR);
		atomic_store(&evt_ctx->result, CS_STATUS_ERROR_IO);
	}

	cs_event_release(evt_ctx);
	cs_device_release(dev_ctx);
	return status;
}

/**
 * Batch Operations
 */
csStatus
csQueueBatchRequest(csCSxHandle csx_handle, csComputeCallback callback,
		    csEventHandle event, const csRequest *batch,
		    uint32_t count)
{
	if (!batch || count == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t dev_index = cs_handle_get_index(csx_handle);
	cs_batch_ctx_t *ctx;
	csBatchHandle batch_handle;
	status = cs_batch_alloc_ctx(dev_index, CS_BATCH_MODE_SERIALIZED,
				    count, &ctx, &batch_handle);
	if (status != CS_STATUS_SUCCESS) {
		cs_device_release(dev_ctx);
		return status;
	}

	for (uint32_t i = 0; i < count; i++) {
		ctx->entries[i] = batch[i];
	}
	ctx->entry_count = count;

	status = csSubmitBatchRequest(csx_handle, batch_handle, callback, event);

	csStatus free_status = csFreeBatchRequest(csx_handle, batch_handle);
	if (status == CS_STATUS_SUCCESS) {
		status = free_status;
	}

	cs_batch_release(ctx);
	cs_device_release(dev_ctx);
	return status;
}

csStatus
csAllocBatchRequest(csCSxHandle csx_handle, csBatchMode mode,
		    uint32_t max_entries, csBatchHandle *batch)
{
	if (!batch || max_entries == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	*batch = CS_INVALID_HANDLE;

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t dev_index = cs_handle_get_index(csx_handle);

	if (dev_ctx->backend && dev_ctx->backend->batch_alloc) {
		status = dev_ctx->backend->batch_alloc(dev_ctx->backend_ctx, mode,
						       max_entries, batch);
		cs_device_release(dev_ctx);
		return status;
	}

	cs_batch_ctx_t *ctx;
	status = cs_batch_alloc_ctx(dev_index, mode, max_entries, &ctx, batch);
	cs_device_release(dev_ctx);
	return status;
}

csStatus
csAddBatchEntry(csBatchHandle batch, const csRequest *entry)
{
	if (!entry) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_batch_ctx_t *ctx;
	csStatus status = cs_batch_lookup(batch, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (ctx->entry_count >= ctx->max_entries) {
		cs_batch_release(ctx);
		return CS_STATUS_ERROR_LIMIT;
	}

	ctx->entries[ctx->entry_count] = *entry;
	ctx->entry_count++;
	cs_batch_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csHelperReconfigureBatchEntry(csBatchHandle batch, uint32_t entry_index,
			      const csRequest *new_entry)
{
	if (!new_entry) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_batch_ctx_t *ctx;
	csStatus status = cs_batch_lookup(batch, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (entry_index >= ctx->entry_count) {
		cs_batch_release(ctx);
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	ctx->entries[entry_index] = *new_entry;
	cs_batch_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csHelperResizeBatchRequest(csBatchHandle batch, uint32_t new_size)
{
	if (new_size > CS_MAX_BATCH_ENTRIES) {
		return CS_STATUS_ERROR_LIMIT;
	}

	cs_batch_ctx_t *ctx;
	csStatus status = cs_batch_lookup(batch, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (new_size < ctx->entry_count) {
		ctx->entry_count = new_size;
	}
	ctx->max_entries = new_size;
	cs_batch_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csFreeBatchRequest(csCSxHandle csx_handle, csBatchHandle batch)
{
	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (dev_ctx->backend && dev_ctx->backend->batch_free) {
		status = dev_ctx->backend->batch_free(dev_ctx->backend_ctx, batch);
	} else {
		status = cs_batch_free_ctx(batch);
	}

	cs_device_release(dev_ctx);
	return status;
}

csStatus
csSubmitBatchRequest(csCSxHandle csx_handle, csBatchHandle batch,
		     csComputeCallback callback, csEventHandle event)
{
	if (batch == CS_INVALID_HANDLE) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (dev_ctx->backend && dev_ctx->backend->batch_submit) {
		status = dev_ctx->backend->batch_submit(dev_ctx->backend_ctx,
							batch, callback, event);
		cs_device_release(dev_ctx);
		return status;
	}

	/* Fallback: iterate internal batch entries sequentially */
	cs_batch_ctx_t *ctx;
	status = cs_batch_lookup(batch, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		cs_device_release(dev_ctx);
		return status;
	}

	cs_event_ctx_t *evt_ctx = NULL;
	if (event != CS_INVALID_HANDLE) {
		csStatus ev_status = cs_event_lookup(event, &evt_ctx);
		if (ev_status != CS_STATUS_SUCCESS) {
			cs_batch_release(ctx);
			cs_device_release(dev_ctx);
			return ev_status;
		}
	}

	for (uint32_t i = 0; i < ctx->entry_count; i++) {
		const csRequest *req = &ctx->entries[i];
		switch (req->type) {
		case CS_REQ_TYPE_COPY_MEM:
			status = csQueueCopyMemRequest(
				csx_handle, NULL, CS_INVALID_HANDLE,
				req->u.copy.src, req->u.copy.dst,
				req->u.copy.size, req->u.copy.dir);
			break;
		case CS_REQ_TYPE_STORAGE:
			status = csQueueStorageRequest(
				csx_handle, NULL, CS_INVALID_HANDLE,
				req->u.storage.mem, req->u.storage.lba,
				req->u.storage.blocks, req->u.storage.op);
			break;
		case CS_REQ_TYPE_COMPUTE:
			status = csQueueComputeRequest(
				csx_handle, &req->u.compute.desc,
				CS_INVALID_HANDLE, NULL, NULL);
			break;
		default:
			status = CS_STATUS_ERROR_INVALID_PARAM;
			break;
		}

		if (status != CS_STATUS_SUCCESS) {
			if (evt_ctx) {
				atomic_store(&evt_ctx->status,
					     CS_EVENT_STATUS_ERROR);
				atomic_store(&evt_ctx->result, status);
				cs_event_release(evt_ctx);
			}
			cs_batch_release(ctx);
			cs_device_release(dev_ctx);
			if (callback) {
				callback(status, NULL);
			}
			return status;
		}
	}

	if (evt_ctx) {
		atomic_store(&evt_ctx->status, CS_EVENT_STATUS_COMPLETE);
		atomic_store(&evt_ctx->result, CS_STATUS_SUCCESS);
		cs_event_release(evt_ctx);
	}

	cs_batch_release(ctx);
	cs_device_release(dev_ctx);

	if (callback) {
		callback(CS_STATUS_SUCCESS, NULL);
	}

	return CS_STATUS_SUCCESS;
}
csStatus
csCreateEvent(csCSxHandle csx_handle, csEventHandle *event)
{
	if (!event) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	*event = CS_INVALID_HANDLE;

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	cs_event_ctx_t *evt_ctx;
	uint32_t dev_index = cs_handle_get_index(csx_handle);
	status = cs_event_alloc_ctx(dev_index, &evt_ctx, event);
	cs_device_release(dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	CS_LOG_DEBUG("Created event handle=0x%lx", *event);
	return CS_STATUS_SUCCESS;
}

csStatus
csDestroyEvent(csCSxHandle csx_handle, csEventHandle event)
{
	(void)csx_handle; /* Unused for now */

	csStatus status = cs_event_free_ctx(event);
	if (status == CS_STATUS_SUCCESS) {
		CS_LOG_DEBUG("Destroyed event handle=0x%lx", event);
	}
	return status;
}

csStatus
csPollEvent(csCSxHandle csx_handle, csEventHandle event,
	    csEventStatus *status, csStatus *result)
{
	if (!status || !result) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *dev_ctx;
	csStatus ret = cs_device_lookup(csx_handle, &dev_ctx);
	if (ret != CS_STATUS_SUCCESS) {
		return ret;
	}

	cs_event_ctx_t *evt_ctx;
	ret = cs_event_lookup(event, &evt_ctx);
	if (ret != CS_STATUS_SUCCESS) {
		cs_device_release(dev_ctx);
		return ret;
	}

	/* Poll backend if available */
	if (dev_ctx->backend && dev_ctx->backend->poll) {
		dev_ctx->backend->poll(dev_ctx->backend_ctx, 0);
	}

	/* Check completion flag set by backend */
	int cflag = atomic_load(&evt_ctx->completion_flag);
	if (cflag != 0) {
		int cpl_status = cflag > 0 ?
				 CS_STATUS_SUCCESS : CS_STATUS_ERROR_IO;

		if (dev_ctx->backend && dev_ctx->backend->compute_reap) {
			csStatus reap_status = dev_ctx->backend->compute_reap(
				dev_ctx->backend_ctx, (volatile int *)(void *)&evt_ctx->completion_flag);
			if (reap_status != CS_STATUS_PENDING && reap_status != CS_STATUS_SUCCESS) {
				cpl_status = reap_status;
			} else if (reap_status == CS_STATUS_SUCCESS) {
				cpl_status = CS_STATUS_SUCCESS;
			}
		}

		atomic_store(&evt_ctx->status, cpl_status == CS_STATUS_SUCCESS ?
			     CS_EVENT_STATUS_COMPLETE : CS_EVENT_STATUS_ERROR);
		atomic_store(&evt_ctx->result, cpl_status);

		if (!evt_ctx->callback_invoked && evt_ctx->callback) {
			evt_ctx->callback_invoked = true;
			csComputeCallback cb = evt_ctx->callback;
			void *ud = evt_ctx->user_data;
			*status = atomic_load(&evt_ctx->status);
			*result = atomic_load(&evt_ctx->result);
			cs_event_release(evt_ctx);
			cs_device_release(dev_ctx);
			cb(cpl_status, ud);
			return CS_STATUS_SUCCESS;
		}
	}

	*status = atomic_load(&evt_ctx->status);
	*result = atomic_load(&evt_ctx->result);

	cs_event_release(evt_ctx);
	cs_device_release(dev_ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csWaitEvent(csCSxHandle csx_handle, csEventHandle event,
	    uint32_t timeout_ms, csStatus *result)
{
	if (!result) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *dev_ctx;
	csStatus ret = cs_device_lookup(csx_handle, &dev_ctx);
	if (ret != CS_STATUS_SUCCESS) {
		return ret;
	}

	cs_event_ctx_t *evt_ctx;
	ret = cs_event_lookup(event, &evt_ctx);
	if (ret != CS_STATUS_SUCCESS) {
		cs_device_release(dev_ctx);
		return ret;
	}

	cs_event_release(evt_ctx);
	cs_device_release(dev_ctx);

	/* Simple polling loop with timeout */
	uint32_t elapsed = 0;
	const uint32_t poll_interval = 1; /* 1ms */

	while (timeout_ms == 0 || elapsed < timeout_ms) {
		csEventStatus status;
		ret = csPollEvent(csx_handle, event, &status, result);
		if (ret != CS_STATUS_SUCCESS) {
			return ret;
		}

		if (status != CS_EVENT_STATUS_PENDING) {
			return CS_STATUS_SUCCESS;
		}

		/* Sleep briefly */
		struct timespec ts = { 0, poll_interval * 1000000 };
		nanosleep(&ts, NULL);
		elapsed += poll_interval;
	}

	return CS_STATUS_ERROR_TIMEOUT;
}
