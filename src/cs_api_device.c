/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_api_device.c
 * @brief CS API Device, Memory and Data Movement Implementation
 */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_internal.h"
#include "cs_programs.h"

static csStatus
cs_resolve_mem_ptr(void *ptr_or_handle, void **ptr_out, size_t *size_out, bool *is_mem)
{
	if (!ptr_or_handle || !ptr_out) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_mem_ctx_t *mem_ctx = NULL;
	csStatus status = cs_mem_lookup((csMemHandle)(uintptr_t)ptr_or_handle, &mem_ctx);
	if (status == CS_STATUS_SUCCESS) {
		if (mem_ctx->type != CS_MEM_TYPE_AFDM) {
			cs_mem_release(mem_ctx);
			return CS_STATUS_ERROR_NOT_SUPPORTED;
		}
		*ptr_out = mem_ctx->ptr;
		if (size_out) {
			*size_out = mem_ctx->size;
		}
		if (is_mem) {
			*is_mem = true;
		}
		cs_mem_release(mem_ctx);
		return CS_STATUS_SUCCESS;
	}

	*ptr_out = ptr_or_handle;
	if (size_out) {
		*size_out = 0;
	}
	if (is_mem) {
		*is_mem = false;
	}
	return CS_STATUS_SUCCESS;
}

/**
 * Device Discovery and Management
 */
csStatus
csQueryCSxList(csDeviceInfo *devices, uint32_t *count)
{
	csStatus status = cs_ensure_init();
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!count) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	const cs_backend_ops_t *backend = cs_backend_get_default();
	if (!backend) {
		*count = 0;
		return CS_STATUS_SUCCESS;
	}

	if (!backend->dev_query) {
		*count = 0;
		return CS_STATUS_SUCCESS;
	}

	return backend->dev_query(devices, count);
}

csStatus
csOpenCSx(const char *uri, csCSxHandle *handle)
{
	csStatus status = cs_ensure_init();
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!uri || !handle) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	*handle = CS_INVALID_HANDLE;

	/* Try the preferred (default) backend first, then fall back to others */
	const cs_backend_ops_t *backend = cs_backend_get_default();
	if (!backend) {
		return CS_STATUS_ERROR_NO_DEVICE;
	}

	/* Allocate device context */
	cs_device_ctx_t *ctx;
	uint32_t index;
	status = cs_device_alloc(&ctx, &index);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	/* Open device via backend — try default first, then remaining backends */
	status = CS_STATUS_ERROR_NO_DEVICE;
	if (backend->dev_open) {
		status = backend->dev_open(uri, &ctx->backend_ctx);
	}
	if (status != CS_STATUS_SUCCESS) {
		/* Default backend failed; try each registered backend */
		pthread_mutex_lock(&g_state.backends_lock);
		for (uint32_t i = 0; i < g_state.backend_count; i++) {
			if (g_state.backends[i] == backend) {
				continue; /* already tried */
			}
			if (!g_state.backends[i]->dev_open) {
				continue;
			}
			csStatus try_status = g_state.backends[i]->dev_open(
				uri, &ctx->backend_ctx);
			if (try_status == CS_STATUS_SUCCESS) {
				backend = g_state.backends[i];
				status = CS_STATUS_SUCCESS;
				break;
			}
		}
		pthread_mutex_unlock(&g_state.backends_lock);
	}
	if (status != CS_STATUS_SUCCESS) {
		cs_device_free(index);
		return status;
	}

	ctx->backend = backend;
	strncpy(ctx->uri, uri, sizeof(ctx->uri) - 1);
	ctx->uri[sizeof(ctx->uri) - 1] = '\0';

	/* Get device info */
	if (backend->dev_get_info) {
		backend->dev_get_info(ctx->backend_ctx, &ctx->info);
	}

	*handle = cs_handle_encode(CS_HANDLE_TYPE_DEVICE, ctx->generation, index);

	CS_LOG_INFO("Opened device: %s (handle=0x%lx)", uri, *handle);
	return CS_STATUS_SUCCESS;
}

csStatus
csCloseCSx(csCSxHandle handle)
{
	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t index = cs_handle_get_index(handle);

	/*
	 * TOCTOU fix (residual gap on top of the earlier Bug 2 fix): the
	 * check-then-destroy sequence below must be serialized against
	 * cs_device_lookup() using the SAME lock cs_device_lookup() uses for
	 * its own refcount-increment check -- devices_lock, not ctx->lock.
	 * ctx->lock alone cannot help here: cs_device_lookup() never
	 * acquires it, so a fresh lookup racing against this function could
	 * previously succeed (find the slot still `in_use`, bump refcount)
	 * at any point between backend->dev_close() running and
	 * cs_device_free() removing the slot, regardless of ctx->lock.
	 *
	 * The fix: check refcount and set ctx->closing atomically, both
	 * under devices_lock. cs_device_lookup() also checks `closing` under
	 * devices_lock (see cs_device_lookup() above), so once we observe
	 * refcount == 1 (i.e. we are the sole holder via our own lookup
	 * above) and set closing = true in the same critical section, no
	 * subsequent lookup can succeed -- there is no gap left for one to
	 * land in between the check and the destructive backend call.
	 */
	pthread_mutex_lock(&g_state.devices_lock);

	if (atomic_load(&ctx->refcount) > 1) {
		cs_device_release(ctx);
		pthread_mutex_unlock(&g_state.devices_lock);
		return CS_STATUS_ERROR_BUSY;
	}

	ctx->closing = true;
	pthread_mutex_unlock(&g_state.devices_lock);

	/* Close via backend, outside devices_lock (may block). Any
	 * concurrent cs_device_lookup() from this point on observes
	 * `closing == true` and fails with CS_STATUS_ERROR_INVALID_HANDLE
	 * instead of handing out a reference to a closing/closed session. */
	if (ctx->backend && ctx->backend->dev_close) {
		status = ctx->backend->dev_close(ctx->backend_ctx);
		if (status != CS_STATUS_SUCCESS) {
			CS_LOG_WARN("Backend dev_close failed: %d", status);
		}
	}

	cs_device_release(ctx);

	/* cs_device_free() re-validates refcount == 0 under devices_lock
	 * before destroying ctx->lock and removing the slot. */
	cs_device_free(index);

	CS_LOG_INFO("Closed device handle=0x%lx", handle);
	return CS_STATUS_SUCCESS;
}

csStatus
csGetCSxInfo(csCSxHandle handle, csDeviceInfo *info)
{
	if (!info) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	*info = ctx->info;
	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csQueryDeviceProperties(csCSxHandle csx_handle, csDeviceProperties *props)
{
	if (!props) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (ctx->backend && ctx->backend->query_device_properties) {
		status = ctx->backend->query_device_properties(ctx->backend_ctx, props);
	} else {
		memset(props, 0, sizeof(*props));
		props->num_cses = 1;
		props->max_concurrent_progs = ctx->info.max_programs;
		props->afdm_total_bytes = (size_t)ctx->info.max_memory_mb * 1024 * 1024;
		props->csf_count = 11;
		status = CS_STATUS_SUCCESS;
	}

	cs_device_release(ctx);
	return status;
}

csStatus
csQueryDeviceStatistics(csCSxHandle csx_handle, csDeviceStatistics *stats)
{
	if (!stats) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	stats->total_executes = ctx->stats_executes;
	stats->total_bytes_processed = ctx->stats_bytes_processed;
	stats->total_errors = ctx->stats_errors;
	stats->avg_latency_ns = ctx->stats_executes
		? ctx->stats_latency_accum_ns / ctx->stats_executes : 0;

	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csConfig(csCSxHandle csx_handle, uint32_t config_id, const void *data, size_t size)
{
	if (!data || size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (ctx->backend && ctx->backend->device_config) {
		status = ctx->backend->device_config(ctx->backend_ctx, config_id, data, size);
	} else {
		status = CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	cs_device_release(ctx);
	return status;
}

csStatus
csReset(csCSxHandle csx_handle, uint32_t reset_type)
{
	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (ctx->backend && ctx->backend->device_reset) {
		status = ctx->backend->device_reset(ctx->backend_ctx, reset_type);
	} else {
		status = CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	cs_device_release(ctx);
	return status;
}

#define CS_MAX_NOTIFIERS 32
#define CS_NOTIFY_EVENT_ALL 0xFFFFFFFFu

struct cs_notify_entry {
	bool in_use;
	uint32_t device_index;
	uint32_t event_type;
	csNotifyCallback callback;
	void *user_data;
};

static struct cs_notify_entry g_notifiers[CS_MAX_NOTIFIERS];
static pthread_mutex_t g_notifiers_lock = PTHREAD_MUTEX_INITIALIZER;

csStatus
csRegisterNotify(csCSxHandle csx_handle, uint32_t event_type,
		 csNotifyCallback callback, void *user_data)
{
	if (!callback) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t dev_index = cs_handle_get_index(csx_handle);
	cs_device_release(dev_ctx);

	pthread_mutex_lock(&g_notifiers_lock);

	for (uint32_t i = 0; i < CS_MAX_NOTIFIERS; i++) {
		if (!g_notifiers[i].in_use) {
			g_notifiers[i].in_use = true;
			g_notifiers[i].device_index = dev_index;
			g_notifiers[i].event_type = event_type;
			g_notifiers[i].callback = callback;
			g_notifiers[i].user_data = user_data;
			pthread_mutex_unlock(&g_notifiers_lock);
			return CS_STATUS_SUCCESS;
		}
	}

	pthread_mutex_unlock(&g_notifiers_lock);
	return CS_STATUS_ERROR_LIMIT;
}

csStatus
csDeregisterNotify(csCSxHandle csx_handle, uint32_t event_type)
{
	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t dev_index = cs_handle_get_index(csx_handle);
	cs_device_release(dev_ctx);

	pthread_mutex_lock(&g_notifiers_lock);

	csStatus result = CS_STATUS_ERROR_NOT_FOUND;
	for (uint32_t i = 0; i < CS_MAX_NOTIFIERS; i++) {
		if (g_notifiers[i].in_use &&
		    g_notifiers[i].device_index == dev_index &&
		    (event_type == CS_NOTIFY_EVENT_ALL ||
		     g_notifiers[i].event_type == event_type)) {
			g_notifiers[i].in_use = false;
			g_notifiers[i].callback = NULL;
			result = CS_STATUS_SUCCESS;
		}
	}

	pthread_mutex_unlock(&g_notifiers_lock);
	return result;
}

csStatus
csAllocMem(csCSxHandle csx_handle, const csMemDesc *desc,
	   csMemHandle *mem_handle, void **ptr)
{
	if (!desc || !mem_handle || !ptr) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (desc->type != CS_MEM_TYPE_AFDM) {
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	*mem_handle = CS_INVALID_HANDLE;
	*ptr = NULL;

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!dev_ctx->backend || !dev_ctx->backend->mem_alloc) {
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	/* Allocate memory context */
	cs_mem_ctx_t *mem_ctx;
	uint32_t dev_index = cs_handle_get_index(csx_handle);
	status = cs_mem_alloc_ctx(dev_index, &mem_ctx, mem_handle);
	if (status != CS_STATUS_SUCCESS) {
		cs_device_release(dev_ctx);
		return status;
	}

	/* Allocate via backend */
	status = dev_ctx->backend->mem_alloc(dev_ctx->backend_ctx, desc->size,
					     desc->alignment, ptr);
	if (status != CS_STATUS_SUCCESS) {
		cs_mem_free_ctx(*mem_handle);
		*mem_handle = CS_INVALID_HANDLE;
		cs_device_release(dev_ctx);
		return status;
	}

	mem_ctx->ptr = *ptr;
	mem_ctx->size = desc->size;
	mem_ctx->type = desc->type;

	cs_device_release(dev_ctx);
	CS_LOG_DEBUG("Allocated %zu bytes at %p (handle=0x%lx)",
		     desc->size, *ptr, *mem_handle);
	return CS_STATUS_SUCCESS;
}

csStatus
csFreeMem(csCSxHandle csx_handle, csMemHandle mem_handle)
{
	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	cs_mem_ctx_t *mem_ctx;
	status = cs_mem_lookup(mem_handle, &mem_ctx);
	if (status != CS_STATUS_SUCCESS) {
		cs_device_release(dev_ctx);
		return status;
	}

	/*
	 * TOCTOU fix (residual gap on top of the earlier Bug 2 fix): serialize
	 * the check-then-destroy sequence against cs_mem_lookup() using the
	 * SAME lock cs_mem_lookup() uses for its own refcount-increment check
	 * -- allocations_lock, not dev_ctx->lock. dev_ctx->lock never helped
	 * here either: cs_mem_lookup() doesn't acquire it, so a fresh lookup
	 * could previously race in between backend->mem_free() and
	 * cs_mem_free_ctx() removing the slot.
	 *
	 * As with csCloseCSx()/cs_device_lookup(), check refcount and set
	 * mem_ctx->closing atomically under allocations_lock; cs_mem_lookup()
	 * checks `closing` in that same critical section, so once we commit
	 * to freeing there is no window left for a lookup to land in.
	 */
	pthread_mutex_lock(&g_state.allocations_lock);

	if (atomic_load(&mem_ctx->refcount) > 1) {
		cs_mem_release(mem_ctx);
		pthread_mutex_unlock(&g_state.allocations_lock);
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_BUSY;
	}

	mem_ctx->closing = true;
	pthread_mutex_unlock(&g_state.allocations_lock);

	/* Free via backend, outside allocations_lock. Any concurrent
	 * cs_mem_lookup() from this point on observes `closing == true` and
	 * fails with CS_STATUS_ERROR_INVALID_HANDLE instead of handing out a
	 * reference to memory that is being (or was just) freed. */
	if (dev_ctx->backend && dev_ctx->backend->mem_free && mem_ctx->ptr) {
		status = dev_ctx->backend->mem_free(dev_ctx->backend_ctx, mem_ctx->ptr);
		if (status != CS_STATUS_SUCCESS) {
			CS_LOG_WARN("Backend mem_free failed: %d", status);
		}
	}

	cs_mem_release(mem_ctx);
	cs_device_release(dev_ctx);

	/* cs_mem_free_ctx() re-validates refcount == 0 under allocations_lock
	 * before removing the slot. */
	cs_mem_free_ctx(mem_handle);
	return CS_STATUS_SUCCESS;
}

csStatus
csInitMem(csCSxHandle csx_handle, csMemHandle mem_handle,
	  uint8_t fill_value, size_t size)
{
	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	cs_mem_ctx_t *mem_ctx;
	status = cs_mem_lookup(mem_handle, &mem_ctx);
	if (status != CS_STATUS_SUCCESS) {
		cs_device_release(dev_ctx);
		return status;
	}

	size_t fill_size = (size == 0) ? mem_ctx->size : size;
	if (fill_size > mem_ctx->size) {
		cs_mem_release(mem_ctx);
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_LIMIT;
	}

	if (dev_ctx->backend && dev_ctx->backend->mem_init) {
		status = dev_ctx->backend->mem_init(dev_ctx->backend_ctx,
						    mem_ctx->ptr, fill_value, fill_size);
	} else {
		memset(mem_ctx->ptr, fill_value, fill_size);
		status = CS_STATUS_SUCCESS;
	}

	cs_mem_release(mem_ctx);
	cs_device_release(dev_ctx);
	return status;
}

csStatus
csGetMemInfo(csCSxHandle csx_handle, csMemInfo *info)
{
	if (!info) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t dev_index = cs_handle_get_index(csx_handle);
	uint32_t count = 0;
	size_t total = 0;

	pthread_mutex_lock(&g_state.allocations_lock);
	for (uint32_t i = 0; i < CS_MAX_ALLOCATIONS; i++) {
		if (g_state.allocations[i].in_use &&
		    g_state.allocations[i].device_index == dev_index) {
			count++;
			total += g_state.allocations[i].size;
		}
	}
	pthread_mutex_unlock(&g_state.allocations_lock);

	info->allocation_count = count;
	info->max_allocations = CS_MAX_ALLOCATIONS;
	info->total_allocated_bytes = total;

	cs_device_release(dev_ctx);
	return CS_STATUS_SUCCESS;
}

/**
 * Data Movement
 */
csStatus
csQueueCopyMemRequest(csCSxHandle csx_handle, csComputeCallback callback,
		      csEventHandle event, void *src, void *dst,
		      size_t size, csCopyDirection dir)
{
	if (!src || !dst || size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	(void)dev_ctx;

	cs_event_ctx_t *evt_ctx = NULL;
	if (event != CS_INVALID_HANDLE) {
		status = cs_event_lookup(event, &evt_ctx);
		if (status != CS_STATUS_SUCCESS) {
			cs_device_release(dev_ctx);
			return status;
		}
	}

	void *src_ptr = NULL;
	void *dst_ptr = NULL;
	size_t src_size = 0;
	size_t dst_size = 0;
	bool src_is_mem = false;
	bool dst_is_mem = false;
	uint32_t dev_index = cs_handle_get_index(csx_handle);

	status = cs_resolve_mem_ptr(src, &src_ptr, &src_size, &src_is_mem);
	if (status != CS_STATUS_SUCCESS) {
		if (evt_ctx) cs_event_release(evt_ctx);
		cs_device_release(dev_ctx);
		return status;
	}

	status = cs_resolve_mem_ptr(dst, &dst_ptr, &dst_size, &dst_is_mem);
	if (status != CS_STATUS_SUCCESS) {
		if (evt_ctx) cs_event_release(evt_ctx);
		cs_device_release(dev_ctx);
		return status;
	}

	cs_mem_ctx_t *src_ctx = NULL;
	cs_mem_ctx_t *dst_ctx = NULL;

	if (src_is_mem) {
		status = cs_mem_lookup((csMemHandle)(uintptr_t)src, &src_ctx);
		if (status != CS_STATUS_SUCCESS) {
			if (evt_ctx) cs_event_release(evt_ctx);
			cs_device_release(dev_ctx);
			return status;
		}
		if (src_ctx->device_index != dev_index) {
			cs_mem_release(src_ctx);
			if (evt_ctx) cs_event_release(evt_ctx);
			cs_device_release(dev_ctx);
			return CS_STATUS_ERROR_INVALID_HANDLE;
		}
	}

	if (dst_is_mem) {
		status = cs_mem_lookup((csMemHandle)(uintptr_t)dst, &dst_ctx);
		if (status != CS_STATUS_SUCCESS) {
			if (src_ctx) cs_mem_release(src_ctx);
			if (evt_ctx) cs_event_release(evt_ctx);
			cs_device_release(dev_ctx);
			return status;
		}
		if (dst_ctx->device_index != dev_index) {
			cs_mem_release(dst_ctx);
			if (src_ctx) cs_mem_release(src_ctx);
			if (evt_ctx) cs_event_release(evt_ctx);
			cs_device_release(dev_ctx);
			return CS_STATUS_ERROR_INVALID_HANDLE;
		}
	}

	switch (dir) {
	case CS_COPY_HOST_TO_DEVICE:
		if (src_is_mem || !dst_is_mem) {
			if (dst_ctx) cs_mem_release(dst_ctx);
			if (src_ctx) cs_mem_release(src_ctx);
			if (evt_ctx) cs_event_release(evt_ctx);
			cs_device_release(dev_ctx);
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		break;
	case CS_COPY_DEVICE_TO_HOST:
		if (!src_is_mem || dst_is_mem) {
			if (dst_ctx) cs_mem_release(dst_ctx);
			if (src_ctx) cs_mem_release(src_ctx);
			if (evt_ctx) cs_event_release(evt_ctx);
			cs_device_release(dev_ctx);
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		break;
	case CS_COPY_DEVICE_TO_DEVICE:
		if (!src_is_mem || !dst_is_mem) {
			if (dst_ctx) cs_mem_release(dst_ctx);
			if (src_ctx) cs_mem_release(src_ctx);
			if (evt_ctx) cs_event_release(evt_ctx);
			cs_device_release(dev_ctx);
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		break;
	case CS_COPY_HOST_TO_HOST:
		if (src_is_mem || dst_is_mem) {
			if (dst_ctx) cs_mem_release(dst_ctx);
			if (src_ctx) cs_mem_release(src_ctx);
			if (evt_ctx) cs_event_release(evt_ctx);
			cs_device_release(dev_ctx);
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		break;
	default:
		if (dst_ctx) cs_mem_release(dst_ctx);
		if (src_ctx) cs_mem_release(src_ctx);
		if (evt_ctx) cs_event_release(evt_ctx);
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (src_is_mem && size > src_size) {
		if (dst_ctx) cs_mem_release(dst_ctx);
		if (src_ctx) cs_mem_release(src_ctx);
		if (evt_ctx) cs_event_release(evt_ctx);
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_LIMIT;
	}
	if (dst_is_mem && size > dst_size) {
		if (dst_ctx) cs_mem_release(dst_ctx);
		if (src_ctx) cs_mem_release(src_ctx);
		if (evt_ctx) cs_event_release(evt_ctx);
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_LIMIT;
	}

	memcpy(dst_ptr, src_ptr, size);

	if (evt_ctx) {
		atomic_store(&evt_ctx->status, CS_EVENT_STATUS_COMPLETE);
		atomic_store(&evt_ctx->result, CS_STATUS_SUCCESS);
		cs_event_release(evt_ctx);
	}

	if (dst_ctx) cs_mem_release(dst_ctx);
	if (src_ctx) cs_mem_release(src_ctx);
	cs_device_release(dev_ctx);

	if (callback) {
		callback(CS_STATUS_SUCCESS, NULL);
	}

	return CS_STATUS_SUCCESS;
}

csStatus
csQueueStorageRequest(csCSxHandle csx_handle, csComputeCallback callback,
		      csEventHandle event, csMemHandle mem,
		      uint64_t lba, uint32_t blocks, csStorageOp op)
{
	if (mem == CS_INVALID_HANDLE || blocks == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *dev_ctx;
	csStatus status = cs_device_lookup(csx_handle, &dev_ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	cs_mem_ctx_t *mem_ctx;
	status = cs_mem_lookup(mem, &mem_ctx);
	if (status != CS_STATUS_SUCCESS) {
		cs_device_release(dev_ctx);
		return status;
	}

	if (mem_ctx->type != CS_MEM_TYPE_AFDM) {
		cs_mem_release(mem_ctx);
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}
	if (mem_ctx->device_index != cs_handle_get_index(csx_handle)) {
		cs_mem_release(mem_ctx);
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	if (!dev_ctx->backend || !dev_ctx->backend->storage_io) {
		cs_mem_release(mem_ctx);
		cs_device_release(dev_ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	cs_event_ctx_t *evt_ctx = NULL;
	if (event != CS_INVALID_HANDLE) {
		status = cs_event_lookup(event, &evt_ctx);
		if (status != CS_STATUS_SUCCESS) {
			cs_mem_release(mem_ctx);
			cs_device_release(dev_ctx);
			return status;
		}
	}

	volatile int *completion = evt_ctx ? (volatile int *)(void *)&evt_ctx->completion_flag : NULL;

	status = dev_ctx->backend->storage_io(dev_ctx->backend_ctx, op, lba, blocks,
					      mem_ctx->ptr, mem_ctx->size, completion);

	if (status == CS_STATUS_PENDING && evt_ctx) {
		/* Completion callback won't release these; release them now */
		cs_mem_release(mem_ctx);
		cs_device_release(dev_ctx);
		return CS_STATUS_PENDING;
	}

	if (evt_ctx) {
		atomic_store(&evt_ctx->status, status == CS_STATUS_SUCCESS ?
			     CS_EVENT_STATUS_COMPLETE : CS_EVENT_STATUS_ERROR);
		atomic_store(&evt_ctx->result, status);
		cs_event_release(evt_ctx);
	}

	cs_mem_release(mem_ctx);
	cs_device_release(dev_ctx);

	if (callback) {
		callback(status, NULL);
	}

	return status;
}
