/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_api.c
 * @brief CS API Core Implementation
 */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_internal.h"
#include "cs_programs.h"

#define CS_VERSION "1.0.0"

/**
 * Global state singleton
 */
static cs_global_state_t g_state = {
	.initialized = false,
	.init_lock = PTHREAD_MUTEX_INITIALIZER,
	.devices_lock = PTHREAD_MUTEX_INITIALIZER,
	.allocations_lock = PTHREAD_MUTEX_INITIALIZER,
	.events_lock = PTHREAD_MUTEX_INITIALIZER,
	.batches_lock = PTHREAD_MUTEX_INITIALIZER,
	.backends_lock = PTHREAD_MUTEX_INITIALIZER,
	.groups_lock = PTHREAD_MUTEX_INITIALIZER,
	.backend_count = 0,
};

/**
 * Debug logging
 */
static atomic_int g_log_level = -1; /* -1 = uninitialized */

void
cs_log(cs_log_level_t level, const char *fmt, ...)
{
	int cur = atomic_load(&g_log_level);
	if (cur < 0) {
		const char *env = getenv("LIBCS_DEBUG");
		int val = env ? atoi(env) : 0;
		atomic_compare_exchange_strong(&g_log_level, &cur, val);
		cur = atomic_load(&g_log_level);
	}

	if ((int)level > cur) {
		return;
	}

	const char *level_str[] = { "ERROR", "WARN", "INFO", "DEBUG" };
	if ((unsigned)level >= sizeof(level_str) / sizeof(level_str[0])) {
		level = CS_LOG_DEBUG;
	}
	fprintf(stderr, "[CS:%s] ", level_str[level]);

	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fprintf(stderr, "\n");
}

/**
 * Error mapping
 */
csStatus
cs_map_errno(int err)
{
	switch (err) {
	case 0:
		return CS_STATUS_SUCCESS;
	case ENOMEM:
		return CS_STATUS_ERROR_NO_MEMORY;
	case ENODEV:
	case ENOENT:
		return CS_STATUS_ERROR_NOT_FOUND;
	case EBUSY:
		return CS_STATUS_ERROR_BUSY;
	case ETIMEDOUT:
		return CS_STATUS_ERROR_TIMEOUT;
	case EIO:
		return CS_STATUS_ERROR_IO;
#if defined(ENOTSUP) && defined(EOPNOTSUPP) && (ENOTSUP != EOPNOTSUPP)
	case ENOTSUP:
	case EOPNOTSUPP:
#elif defined(ENOTSUP)
	case ENOTSUP:
#elif defined(EOPNOTSUPP)
	case EOPNOTSUPP:
#endif
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	case EPERM:
	case EACCES:
		return CS_STATUS_ERROR_PERMISSION;
	case EINVAL:
		return CS_STATUS_ERROR_INVALID_PARAM;
	case EEXIST:
		return CS_STATUS_ERROR_ALREADY_EXISTS;
	default:
		return CS_STATUS_ERROR_BACKEND;
	}
}

csStatus
cs_map_nvme_status(uint16_t sc, uint16_t sct)
{
	if (sc == 0 && sct == 0) {
		return CS_STATUS_SUCCESS;
	}

	/* Generic command status */
	if (sct == 0) {
		switch (sc) {
		case 0x00:
			return CS_STATUS_SUCCESS;
		case 0x01: /* Invalid Command Opcode */
		case 0x02: /* Invalid Field */
			return CS_STATUS_ERROR_INVALID_PARAM;
		case 0x09: /* Capacity Exceeded */
			return CS_STATUS_ERROR_LIMIT;
		default:
			return CS_STATUS_ERROR_IO;
		}
	}

	return CS_STATUS_ERROR_BACKEND;
}

const char *
csStatusToString(csStatus status)
{
	switch (status) {
	case CS_STATUS_SUCCESS:             return "SUCCESS";
	case CS_STATUS_ERROR_INVALID_PARAM: return "INVALID_PARAM";
	case CS_STATUS_ERROR_NO_MEMORY:     return "NO_MEMORY";
	case CS_STATUS_ERROR_NOT_FOUND:     return "NOT_FOUND";
	case CS_STATUS_ERROR_BUSY:          return "BUSY";
	case CS_STATUS_ERROR_TIMEOUT:       return "TIMEOUT";
	case CS_STATUS_ERROR_IO:            return "IO";
	case CS_STATUS_ERROR_NOT_SUPPORTED: return "NOT_SUPPORTED";
	case CS_STATUS_ERROR_PERMISSION:    return "PERMISSION";
	case CS_STATUS_ERROR_ALREADY_EXISTS: return "ALREADY_EXISTS";
	case CS_STATUS_ERROR_INVALID_HANDLE: return "INVALID_HANDLE";
	case CS_STATUS_ERROR_LIMIT:         return "LIMIT";
	case CS_STATUS_ERROR_BACKEND:       return "BACKEND";
	default:                            return "UNKNOWN";
	}
}

/**
 * Global state access
 */
cs_global_state_t *
cs_get_state(void)
{
	return &g_state;
}

csStatus
cs_ensure_init(void)
{
	if (g_state.initialized) {
		return CS_STATUS_SUCCESS;
	}
	return csInit();
}

/**
 * Device context management
 */
csStatus
cs_device_alloc(cs_device_ctx_t **ctx, uint32_t *index)
{
	pthread_mutex_lock(&g_state.devices_lock);

	for (uint32_t i = 0; i < CS_MAX_DEVICES; i++) {
		if (!g_state.devices[i].in_use) {
			g_state.devices[i].in_use = true;
			g_state.devices[i].generation++;
			g_state.devices[i].closing = false;
			atomic_store(&g_state.devices[i].refcount, 0);
			pthread_mutex_init(&g_state.devices[i].lock, NULL);
			/* Reset program-slot bookkeeping; slot 0 is reserved
			 * for the device's built-in/default program. */
			for (uint32_t p = 0; p < CS_MAX_PROGRAM_SLOTS; p++) {
				atomic_store(&g_state.devices[i].prog_slot_used[p],
					     p == 0);
			}
			*ctx = &g_state.devices[i];
			*index = i;
			pthread_mutex_unlock(&g_state.devices_lock);
			return CS_STATUS_SUCCESS;
		}
	}

	pthread_mutex_unlock(&g_state.devices_lock);
	return CS_STATUS_ERROR_LIMIT;
}

csStatus
cs_device_free(uint32_t index)
{
	if (index >= CS_MAX_DEVICES) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_state.devices_lock);

	if (!g_state.devices[index].in_use) {
		pthread_mutex_unlock(&g_state.devices_lock);
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	if (atomic_load(&g_state.devices[index].refcount) > 0) {
		pthread_mutex_unlock(&g_state.devices_lock);
		return CS_STATUS_ERROR_BUSY;
	}

	pthread_mutex_destroy(&g_state.devices[index].lock);
	g_state.devices[index].in_use = false;
	g_state.devices[index].closing = false;
	g_state.devices[index].backend = NULL;
	g_state.devices[index].backend_ctx = NULL;

	pthread_mutex_unlock(&g_state.devices_lock);
	return CS_STATUS_SUCCESS;
}

csStatus
cs_device_lookup(csCSxHandle handle, cs_device_ctx_t **ctx)
{
	if (handle == CS_INVALID_HANDLE) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	cs_handle_type_t type = cs_handle_get_type(handle);
	if (type != CS_HANDLE_TYPE_DEVICE) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	uint32_t index = cs_handle_get_index(handle);
	uint32_t gen = cs_handle_get_gen(handle);

	if (index >= CS_MAX_DEVICES) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_state.devices_lock);

	cs_device_ctx_t *dev = &g_state.devices[index];
	/*
	 * `closing` is checked here, under the same devices_lock critical
	 * section as the refcount increment below, so it cannot interleave
	 * with csCloseCSx()'s own busy-check + closing-flag-set (also done
	 * under devices_lock). Once a close has committed, no fresh lookup
	 * can succeed -- this is what closes the residual TOCTOU window.
	 */
	if (!dev->in_use || dev->generation != gen || dev->closing) {
		pthread_mutex_unlock(&g_state.devices_lock);
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	atomic_fetch_add(&dev->refcount, 1);
	*ctx = dev;
	pthread_mutex_unlock(&g_state.devices_lock);
	return CS_STATUS_SUCCESS;
}

void
cs_device_release(cs_device_ctx_t *ctx)
{
	atomic_fetch_sub(&ctx->refcount, 1);
}

/**
 * Memory context management
 */
csStatus
cs_mem_alloc_ctx(uint32_t device_index, cs_mem_ctx_t **ctx, csMemHandle *handle)
{
	pthread_mutex_lock(&g_state.allocations_lock);

	for (uint32_t i = 0; i < CS_MAX_ALLOCATIONS; i++) {
		if (!g_state.allocations[i].in_use) {
			g_state.allocations[i].in_use = true;
			g_state.allocations[i].generation++;
			g_state.allocations[i].closing = false;
			atomic_store(&g_state.allocations[i].refcount, 0);
			g_state.allocations[i].device_index = device_index;
			*ctx = &g_state.allocations[i];
			*handle = cs_handle_encode(CS_HANDLE_TYPE_MEMORY,
						   g_state.allocations[i].generation, i);
			pthread_mutex_unlock(&g_state.allocations_lock);
			return CS_STATUS_SUCCESS;
		}
	}

	pthread_mutex_unlock(&g_state.allocations_lock);
	return CS_STATUS_ERROR_LIMIT;
}

csStatus
cs_mem_free_ctx(csMemHandle handle)
{
	if (handle == CS_INVALID_HANDLE) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	cs_handle_type_t type = cs_handle_get_type(handle);
	if (type != CS_HANDLE_TYPE_MEMORY) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	uint32_t index = cs_handle_get_index(handle);
	uint32_t gen = cs_handle_get_gen(handle);

	if (index >= CS_MAX_ALLOCATIONS) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_state.allocations_lock);

	cs_mem_ctx_t *mem = &g_state.allocations[index];
	if (!mem->in_use || mem->generation != gen) {
		pthread_mutex_unlock(&g_state.allocations_lock);
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	if (atomic_load(&mem->refcount) > 0) {
		pthread_mutex_unlock(&g_state.allocations_lock);
		return CS_STATUS_ERROR_BUSY;
	}

	mem->in_use = false;
	mem->closing = false;
	mem->ptr = NULL;
	mem->size = 0;

	pthread_mutex_unlock(&g_state.allocations_lock);
	return CS_STATUS_SUCCESS;
}

csStatus
cs_mem_lookup(csMemHandle handle, cs_mem_ctx_t **ctx)
{
	if (handle == CS_INVALID_HANDLE) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	cs_handle_type_t type = cs_handle_get_type(handle);
	if (type != CS_HANDLE_TYPE_MEMORY) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	uint32_t index = cs_handle_get_index(handle);
	uint32_t gen = cs_handle_get_gen(handle);

	if (index >= CS_MAX_ALLOCATIONS) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_state.allocations_lock);

	cs_mem_ctx_t *mem = &g_state.allocations[index];
	/*
	 * `closing` is checked here, under the same allocations_lock critical
	 * section as the refcount increment below, so it cannot interleave
	 * with csFreeMem()'s own busy-check + closing-flag-set (also done
	 * under allocations_lock). See cs_device_lookup() for the analogous
	 * device-side fix.
	 */
	if (!mem->in_use || mem->generation != gen || mem->closing) {
		pthread_mutex_unlock(&g_state.allocations_lock);
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	atomic_fetch_add(&mem->refcount, 1);
	*ctx = mem;
	pthread_mutex_unlock(&g_state.allocations_lock);
	return CS_STATUS_SUCCESS;
}

void
cs_mem_release(cs_mem_ctx_t *ctx)
{
	atomic_fetch_sub(&ctx->refcount, 1);
}

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
 * Event context management
 */
csStatus
cs_event_alloc_ctx(uint32_t device_index, cs_event_ctx_t **ctx, csEventHandle *handle)
{
	pthread_mutex_lock(&g_state.events_lock);

	for (uint32_t i = 0; i < CS_MAX_EVENTS; i++) {
		if (!g_state.events[i].in_use) {
			g_state.events[i].in_use = true;
			g_state.events[i].generation++;
			atomic_store(&g_state.events[i].refcount, 0);
			g_state.events[i].device_index = device_index;
			atomic_store(&g_state.events[i].status, CS_EVENT_STATUS_PENDING);
			atomic_store(&g_state.events[i].result, CS_STATUS_SUCCESS);
			g_state.events[i].completion_flag = 0;
			g_state.events[i].callback = NULL;
			g_state.events[i].user_data = NULL;
			g_state.events[i].callback_invoked = false;
			*ctx = &g_state.events[i];
			*handle = cs_handle_encode(CS_HANDLE_TYPE_EVENT,
						   g_state.events[i].generation, i);
			pthread_mutex_unlock(&g_state.events_lock);
			return CS_STATUS_SUCCESS;
		}
	}

	pthread_mutex_unlock(&g_state.events_lock);
	return CS_STATUS_ERROR_LIMIT;
}

csStatus
cs_event_free_ctx(csEventHandle handle)
{
	if (handle == CS_INVALID_HANDLE) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	cs_handle_type_t type = cs_handle_get_type(handle);
	if (type != CS_HANDLE_TYPE_EVENT) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	uint32_t index = cs_handle_get_index(handle);
	uint32_t gen = cs_handle_get_gen(handle);

	if (index >= CS_MAX_EVENTS) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_state.events_lock);

	cs_event_ctx_t *evt = &g_state.events[index];
	if (!evt->in_use || evt->generation != gen) {
		pthread_mutex_unlock(&g_state.events_lock);
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	if (atomic_load(&evt->refcount) > 0) {
		pthread_mutex_unlock(&g_state.events_lock);
		return CS_STATUS_ERROR_BUSY;
	}

	evt->in_use = false;

	pthread_mutex_unlock(&g_state.events_lock);
	return CS_STATUS_SUCCESS;
}

csStatus
cs_event_lookup(csEventHandle handle, cs_event_ctx_t **ctx)
{
	if (handle == CS_INVALID_HANDLE) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	cs_handle_type_t type = cs_handle_get_type(handle);
	if (type != CS_HANDLE_TYPE_EVENT) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	uint32_t index = cs_handle_get_index(handle);
	uint32_t gen = cs_handle_get_gen(handle);

	if (index >= CS_MAX_EVENTS) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_state.events_lock);

	cs_event_ctx_t *evt = &g_state.events[index];
	if (!evt->in_use || evt->generation != gen) {
		pthread_mutex_unlock(&g_state.events_lock);
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	atomic_fetch_add(&evt->refcount, 1);
	*ctx = evt;
	pthread_mutex_unlock(&g_state.events_lock);
	return CS_STATUS_SUCCESS;
}

void
cs_event_release(cs_event_ctx_t *ctx)
{
	atomic_fetch_sub(&ctx->refcount, 1);
}

csStatus
cs_batch_alloc_ctx(uint32_t device_index, csBatchMode mode,
		   uint32_t max_entries, cs_batch_ctx_t **ctx,
		   csBatchHandle *handle)
{
	if (max_entries == 0 || max_entries > CS_MAX_BATCH_ENTRIES) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&g_state.batches_lock);

	for (uint32_t i = 0; i < CS_MAX_BATCHES; i++) {
		if (!g_state.batches[i].in_use) {
			cs_batch_ctx_t *b = &g_state.batches[i];
			b->in_use = true;
			b->generation++;
			b->device_index = device_index;
			b->mode = mode;
			b->max_entries = max_entries;
			b->entry_count = 0;
			atomic_store(&b->refcount, 1);

			*ctx = b;
			*handle = cs_handle_encode(CS_HANDLE_TYPE_BATCH, b->generation, i);
			pthread_mutex_unlock(&g_state.batches_lock);
			return CS_STATUS_SUCCESS;
		}
	}

	pthread_mutex_unlock(&g_state.batches_lock);
	return CS_STATUS_ERROR_NO_MEMORY;
}

csStatus
cs_batch_free_ctx(csBatchHandle handle)
{
	if (handle == CS_INVALID_HANDLE) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}
	if (cs_handle_get_type(handle) != CS_HANDLE_TYPE_BATCH) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	uint32_t index = cs_handle_get_index(handle);
	if (index >= CS_MAX_BATCHES) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_state.batches_lock);

	cs_batch_ctx_t *b = &g_state.batches[index];
	if (!b->in_use || b->generation != cs_handle_get_gen(handle)) {
		pthread_mutex_unlock(&g_state.batches_lock);
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	if (atomic_load(&b->refcount) > 1) {
		pthread_mutex_unlock(&g_state.batches_lock);
		return CS_STATUS_ERROR_BUSY;
	}

	b->in_use = false;
	pthread_mutex_unlock(&g_state.batches_lock);
	return CS_STATUS_SUCCESS;
}

csStatus
cs_batch_lookup(csBatchHandle handle, cs_batch_ctx_t **ctx)
{
	if (handle == CS_INVALID_HANDLE) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}
	if (cs_handle_get_type(handle) != CS_HANDLE_TYPE_BATCH) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	uint32_t index = cs_handle_get_index(handle);
	if (index >= CS_MAX_BATCHES) {
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	pthread_mutex_lock(&g_state.batches_lock);

	cs_batch_ctx_t *b = &g_state.batches[index];
	if (!b->in_use || b->generation != cs_handle_get_gen(handle)) {
		pthread_mutex_unlock(&g_state.batches_lock);
		return CS_STATUS_ERROR_INVALID_HANDLE;
	}

	atomic_fetch_add(&b->refcount, 1);
	*ctx = b;
	pthread_mutex_unlock(&g_state.batches_lock);
	return CS_STATUS_SUCCESS;
}

void
cs_batch_release(cs_batch_ctx_t *ctx)
{
	atomic_fetch_sub(&ctx->refcount, 1);
}

csStatus
cs_group_alloc_ctx(uint32_t device_index, cs_group_ctx_t **ctx,
		    uint32_t *group_id)
{
	pthread_mutex_lock(&g_state.groups_lock);

	for (uint32_t i = 0; i < CS_MAX_GROUPS; i++) {
		if (!g_state.groups[i].in_use) {
			cs_group_ctx_t *g = &g_state.groups[i];
			g->in_use = true;
			g->generation++;
			g->device_index = device_index;
			g->csf_count = 0;
			*ctx = g;
			*group_id = i;
			pthread_mutex_unlock(&g_state.groups_lock);
			return CS_STATUS_SUCCESS;
		}
	}

	pthread_mutex_unlock(&g_state.groups_lock);
	return CS_STATUS_ERROR_NO_MEMORY;
}

csStatus
cs_group_free_ctx(uint32_t group_id)
{
	if (group_id >= CS_MAX_GROUPS) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&g_state.groups_lock);

	cs_group_ctx_t *g = &g_state.groups[group_id];
	if (!g->in_use) {
		pthread_mutex_unlock(&g_state.groups_lock);
		return CS_STATUS_ERROR_NOT_FOUND;
	}

	g->in_use = false;
	pthread_mutex_unlock(&g_state.groups_lock);
	return CS_STATUS_SUCCESS;
}

csStatus
cs_group_lookup(uint32_t group_id, cs_group_ctx_t **ctx)
{
	if (group_id >= CS_MAX_GROUPS) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&g_state.groups_lock);

	cs_group_ctx_t *g = &g_state.groups[group_id];
	if (!g->in_use) {
		pthread_mutex_unlock(&g_state.groups_lock);
		return CS_STATUS_ERROR_NOT_FOUND;
	}

	*ctx = g;
	pthread_mutex_unlock(&g_state.groups_lock);
	return CS_STATUS_SUCCESS;
}

void
cs_group_release(cs_group_ctx_t *ctx)
{
	(void)ctx;
}

/**
 * Backend registry
 */
csStatus
cs_backend_register(const cs_backend_ops_t *ops)
{
	if (!ops || !ops->name) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&g_state.backends_lock);

	/* Check for duplicate */
	for (uint32_t i = 0; i < g_state.backend_count; i++) {
		if (strcmp(g_state.backends[i]->name, ops->name) == 0) {
			pthread_mutex_unlock(&g_state.backends_lock);
			return CS_STATUS_ERROR_ALREADY_EXISTS;
		}
	}

	/* Add backend */
	if (g_state.backend_count >= 8) {
		pthread_mutex_unlock(&g_state.backends_lock);
		return CS_STATUS_ERROR_LIMIT;
	}

	g_state.backends[g_state.backend_count++] = ops;

	pthread_mutex_unlock(&g_state.backends_lock);

	CS_LOG_INFO("Registered backend: %s", ops->name);
	return CS_STATUS_SUCCESS;
}

const cs_backend_ops_t *
cs_backend_get(const char *name)
{
	if (!name) {
		return NULL;
	}

	pthread_mutex_lock(&g_state.backends_lock);

	for (uint32_t i = 0; i < g_state.backend_count; i++) {
		if (strcmp(g_state.backends[i]->name, name) == 0) {
			const cs_backend_ops_t *ops = g_state.backends[i];
			pthread_mutex_unlock(&g_state.backends_lock);
			return ops;
		}
	}

	pthread_mutex_unlock(&g_state.backends_lock);
	return NULL;
}

const cs_backend_ops_t *
cs_backend_get_default(void)
{
	/* Check environment variable for backend selection */
	const char *backend_name = getenv("LIBCS_BACKEND");

	pthread_mutex_lock(&g_state.backends_lock);

	if (backend_name) {
		for (uint32_t i = 0; i < g_state.backend_count; i++) {
			if (strcmp(g_state.backends[i]->name, backend_name) == 0) {
				const cs_backend_ops_t *ops = g_state.backends[i];
				pthread_mutex_unlock(&g_state.backends_lock);
				return ops;
			}
		}
	}

	/* Return first registered backend */
	const cs_backend_ops_t *ops = g_state.backend_count > 0 ?
					g_state.backends[0] : NULL;

	pthread_mutex_unlock(&g_state.backends_lock);
	return ops;
}

csStatus
cs_backend_list(const char **names, uint32_t *count)
{
	if (!count) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&g_state.backends_lock);

	if (!names) {
		*count = g_state.backend_count;
		pthread_mutex_unlock(&g_state.backends_lock);
		return CS_STATUS_SUCCESS;
	}

	uint32_t to_copy = *count < g_state.backend_count ?
			   *count : g_state.backend_count;

	for (uint32_t i = 0; i < to_copy; i++) {
		names[i] = g_state.backends[i]->name;
	}

	*count = g_state.backend_count;

	pthread_mutex_unlock(&g_state.backends_lock);
	return CS_STATUS_SUCCESS;
}

/**
 * Library initialization
 */

/* Forward declaration of xNVMe backend registration */
#ifdef WITH_XNVME_BACKEND
extern void cs_backend_xnvme_register(void);
#endif
#ifdef WITH_SIM_BACKEND
extern void backend_sim_register(void);
#endif
#ifdef WITH_IOCTL_BACKEND
extern void backend_ioctl_register(void);
#endif
#ifdef WITH_IO_URING_BACKEND
extern void backend_io_uring_register(void);
#endif
#ifdef WITH_LIBNVME_BACKEND
extern void backend_libnvme_register(void);
#endif

static _Thread_local uint64_t tl_last_device_result;

void
cs_set_last_device_result(uint64_t value)
{
	tl_last_device_result = value;
}

uint64_t
cs_get_last_device_result(void)
{
	return tl_last_device_result;
}

csStatus
csInit(void)
{
	pthread_mutex_lock(&g_state.init_lock);

	if (g_state.initialized) {
		pthread_mutex_unlock(&g_state.init_lock);
		return CS_STATUS_SUCCESS;
	}

	CS_LOG_INFO("Initializing CS API library v%s", CS_VERSION);

	/* Initialize handle tables */
	memset(g_state.devices, 0, sizeof(g_state.devices));
	memset(g_state.allocations, 0, sizeof(g_state.allocations));
	memset(g_state.events, 0, sizeof(g_state.events));

	/* Register built-in backends */
#ifdef WITH_XNVME_BACKEND
	cs_backend_xnvme_register();
#endif
#ifdef WITH_SIM_BACKEND
	backend_sim_register();
#endif
#ifdef WITH_IOCTL_BACKEND
	backend_ioctl_register();
#endif
#ifdef WITH_IO_URING_BACKEND
	backend_io_uring_register();
#endif
#ifdef WITH_LIBNVME_BACKEND
	backend_libnvme_register();
#endif

	/* Initialize default backend */
	const cs_backend_ops_t *backend = cs_backend_get_default();
	if (backend && backend->init) {
		csStatus status = backend->init();
		if (status != CS_STATUS_SUCCESS) {
			CS_LOG_WARN("Backend init failed: %d", status);
		}
	}

	g_state.initialized = true;

	pthread_mutex_unlock(&g_state.init_lock);
	return CS_STATUS_SUCCESS;
}

csStatus
csShutdown(void)
{
	pthread_mutex_lock(&g_state.init_lock);

	if (!g_state.initialized) {
		pthread_mutex_unlock(&g_state.init_lock);
		return CS_STATUS_SUCCESS;
	}

	CS_LOG_INFO("Shutting down CS API library");

	/* Close any still-open device handles */
	for (uint32_t i = 0; i < CS_MAX_DEVICES; i++) {
		if (g_state.devices[i].in_use) {
			CS_LOG_WARN("Device handle still open at shutdown, force-closing");
			cs_device_ctx_t *dev = &g_state.devices[i];
			if (dev->backend && dev->backend->dev_close) {
				csStatus s = dev->backend->dev_close(dev->backend_ctx);
				if (s != CS_STATUS_SUCCESS) {
					CS_LOG_WARN("Force close backend failed: %d", s);
				}
			}
			dev->in_use = false;
			dev->backend = NULL;
			dev->backend_ctx = NULL;
		}
	}

	/* Shutdown backends */
	for (uint32_t i = 0; i < g_state.backend_count; i++) {
		if (g_state.backends[i]->shutdown) {
			g_state.backends[i]->shutdown();
		}
	}

	g_state.initialized = false;

	pthread_mutex_unlock(&g_state.init_lock);
	return CS_STATUS_SUCCESS;
}

const char *
csGetVersion(void)
{
	return CS_VERSION;
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

	if (ctx->backend && ctx->backend->query_device_statistics) {
		status = ctx->backend->query_device_statistics(ctx->backend_ctx, stats);
	} else {
		memset(stats, 0, sizeof(*stats));
		status = CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	cs_device_release(ctx);
	return status;
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

csStatus
csCSFDownload(csCSxHandle csx_handle, const void *csf_binary,
	      size_t size, csCSFId *csf_id)
{
	if (!csf_binary || size == 0 || !csf_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	*csf_id = 0;

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	/*
	 * Bug 1 fix: atomically find and claim a free program slot under
	 * ctx->lock. Slot 0 is reserved (see cs_device_alloc()) so the scan
	 * starts at 1. Without this, csGetCSFId() used to always report
	 * success, so this scan never found a "free" slot and every
	 * download silently collided on prog_index == 0.
	 */
	uint16_t limit = ctx->info.max_programs;
	if (limit > CS_MAX_PROGRAM_SLOTS) {
		limit = CS_MAX_PROGRAM_SLOTS;
	}

	uint16_t prog_index = 0;
	bool found = false;
	pthread_mutex_lock(&ctx->lock);
	for (uint16_t i = 1; i < limit; i++) {
		if (!atomic_load(&ctx->prog_slot_used[i])) {
			atomic_store(&ctx->prog_slot_used[i], true);
			prog_index = i;
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&ctx->lock);

	if (!found) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_LIMIT;
	}

	status = csLoadProgram(csx_handle, prog_index, csf_binary, size);
	if (status != CS_STATUS_SUCCESS) {
		atomic_store(&ctx->prog_slot_used[prog_index], false);
		cs_device_release(ctx);
		return status;
	}

	status = csActivateProgram(csx_handle, prog_index);
	if (status != CS_STATUS_SUCCESS) {
		atomic_store(&ctx->prog_slot_used[prog_index], false);
		cs_device_release(ctx);
		return status;
	}

	*csf_id = (csCSFId)prog_index;
	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csCSEEDownload(csCSxHandle csx_handle, const void *image,
	       size_t size, uint32_t *csee_id)
{
	if (!image || size == 0 || !csee_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	*csee_id = 0;

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	/* Bug 1 fix: see csCSFDownload() above — atomically find and claim a
	 * free program slot instead of relying on the old always-succeeds
	 * csGetCSFId() stub. */
	uint16_t limit = ctx->info.max_programs;
	if (limit > CS_MAX_PROGRAM_SLOTS) {
		limit = CS_MAX_PROGRAM_SLOTS;
	}

	uint16_t prog_index = 0;
	bool found = false;
	pthread_mutex_lock(&ctx->lock);
	for (uint16_t i = 1; i < limit; i++) {
		if (!atomic_load(&ctx->prog_slot_used[i])) {
			atomic_store(&ctx->prog_slot_used[i], true);
			prog_index = i;
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&ctx->lock);

	if (!found) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_LIMIT;
	}

	status = csLoadProgram(csx_handle, prog_index, image, size);
	if (status != CS_STATUS_SUCCESS) {
		atomic_store(&ctx->prog_slot_used[prog_index], false);
		cs_device_release(ctx);
		return status;
	}

	*csee_id = (uint32_t)prog_index;
	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csGroupComputeByIds(csCSxHandle csx_handle, const csCSFId *csf_ids,
		     uint32_t count, uint32_t *group_id)
{
	if (!csf_ids || count == 0 || count > CS_MAX_GROUP_CSFS || !group_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t dev_index = cs_handle_get_index(csx_handle);

	cs_group_ctx_t *g;
	status = cs_group_alloc_ctx(dev_index, &g, group_id);
	if (status != CS_STATUS_SUCCESS) {
		cs_device_release(ctx);
		return status;
	}

	g->csf_count = count;
	memcpy(g->csf_ids, csf_ids, count * sizeof(csCSFId));

	cs_group_release(g);
	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csUngroupComputeFromGroupId(csCSxHandle csx_handle, uint32_t group_id)
{
	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	status = cs_group_free_ctx(group_id);

	cs_device_release(ctx);
	return status;
}

csStatus
csQueryLibrarySupport(csLibrarySupport *support)
{
	if (!support) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	memset(support, 0, sizeof(*support));
	support->api_version = 0x0100;
	support->num_backends = g_state.backend_count;
	support->features = CS_FEATURE_ASYNC_COMPUTE | CS_FEATURE_BATCH |
			    CS_FEATURE_EVENTS;
	return CS_STATUS_SUCCESS;
}

csStatus
csRegisterPlugin(const struct cs_backend_ops *ops)
{
	return cs_backend_register(ops);
}

csStatus
csDeregisterPlugin(const char *name)
{
	if (!name) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&g_state.backends_lock);

	for (uint32_t i = 0; i < g_state.backend_count; i++) {
		if (strcmp(g_state.backends[i]->name, name) == 0) {
			for (uint32_t j = i; j < g_state.backend_count - 1; j++) {
				g_state.backends[j] = g_state.backends[j + 1];
			}
			g_state.backend_count--;
			g_state.backends[g_state.backend_count] = NULL;
			pthread_mutex_unlock(&g_state.backends_lock);
			CS_LOG_INFO("Deregistered backend: %s", name);
			return CS_STATUS_SUCCESS;
		}
	}

	pthread_mutex_unlock(&g_state.backends_lock);
	return CS_STATUS_ERROR_NOT_FOUND;
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

/**
 * Program Management
 */
csStatus
csGetCSFId(csCSxHandle csx_handle, uint16_t prog_index, csCSFId *csf_id)
{
	if (!csf_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	/*
	 * Bug 1 fix: this used to unconditionally return SUCCESS for any
	 * prog_index, which made the free-slot scan in csCSFDownload() /
	 * csCSEEDownload() never find a free slot (it breaks on the first
	 * *failure*), so every download silently collided on prog_index 0.
	 * Report whether a program is actually loaded/activated at this
	 * slot instead. Slot 0 is always considered present (device
	 * built-in/default program; see cs_device_alloc()).
	 */
	csStatus result;
	if (prog_index >= CS_MAX_PROGRAM_SLOTS ||
	    !atomic_load(&ctx->prog_slot_used[prog_index])) {
		result = CS_STATUS_ERROR_NOT_FOUND;
	} else {
		/* CSF ID is simply the program index */
		*csf_id = prog_index;
		result = CS_STATUS_SUCCESS;
	}

	cs_device_release(ctx);
	return result;
}

csStatus
csGetCSFInfo(csCSxHandle csx_handle, csCSFId csf_id, csCSFInfo *info)
{
	if (!info) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	(void)ctx;

	memset(info, 0, sizeof(*info));
	snprintf(info->name, sizeof(info->name), "prog_%u", (unsigned)csf_id);
	info->version = 0;
	info->flags = 0;

	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csGetCSFIdByName(csCSxHandle csx_handle, const char *name, csCSFId *csf_id)
{
	if (!name || !csf_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t tbl_count;
	const cs_csf_entry_t *tbl = cs_csf_table(&tbl_count);

	csStatus result = CS_STATUS_ERROR_NOT_FOUND;
	for (uint32_t i = 0; i < tbl_count; i++) {
		if (strcmp(name, tbl[i].name) == 0) {
			*csf_id = tbl[i].csf_id;
			result = CS_STATUS_SUCCESS;
			break;
		}
	}

	cs_device_release(ctx);
	return result;
}

csStatus
csQueryCSFList(csCSxHandle csx_handle, csCSFInfo *csfs, uint32_t *count)
{
	if (!count) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (ctx->backend && ctx->backend->query_csf_list) {
		status = ctx->backend->query_csf_list(ctx->backend_ctx, csfs, count);
		cs_device_release(ctx);
		return status;
	}

	uint32_t total;
	const cs_csf_entry_t *tbl = cs_csf_table(&total);

	if (!csfs) {
		*count = total;
		cs_device_release(ctx);
		return CS_STATUS_SUCCESS;
	}

	if (*count < total) {
		*count = total;
		cs_device_release(ctx);
		return CS_STATUS_ERROR_LIMIT;
	}

	for (uint32_t i = 0; i < total; i++) {
		memset(&csfs[i], 0, sizeof(csfs[i]));
		snprintf(csfs[i].name, sizeof(csfs[i].name), "%s", tbl[i].name);
		csfs[i].version = 1;
		csfs[i].flags = 0;
	}
	*count = total;

	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csLoadProgram(csCSxHandle csx_handle, uint16_t prog_index,
	      const void *program, size_t program_size)
{
	if (!program || program_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!ctx->backend || !ctx->backend->prog_load) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	status = ctx->backend->prog_load(ctx->backend_ctx, prog_index,
					 program, program_size);
	cs_device_release(ctx);
	return status;
}

csStatus
csActivateProgram(csCSxHandle csx_handle, uint16_t prog_index)
{
	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!ctx->backend || !ctx->backend->prog_activate) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	status = ctx->backend->prog_activate(ctx->backend_ctx, prog_index);
	cs_device_release(ctx);
	return status;
}

csStatus
csDeactivateProgram(csCSxHandle csx_handle, uint16_t prog_index)
{
	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!ctx->backend || !ctx->backend->prog_deactivate) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	status = ctx->backend->prog_deactivate(ctx->backend_ctx, prog_index);

	/*
	 * Bug 1 fix: the public API has no separate CSF/CSEE "unload" call,
	 * so csDeactivateProgram() is the point at which a program slot
	 * claimed by csCSFDownload()/csCSEEDownload() is released back to
	 * the free pool. Slot 0 is reserved and never toggled here.
	 */
	if (status == CS_STATUS_SUCCESS && prog_index != 0 &&
	    prog_index < CS_MAX_PROGRAM_SLOTS) {
		atomic_store(&ctx->prog_slot_used[prog_index], false);
	}

	cs_device_release(ctx);
	return status;
}

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
