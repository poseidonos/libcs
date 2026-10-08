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
 * Global state singleton.
 *
 * `initialized` is the library-lifecycle flag. It is written by csInit()
 * and csShutdown() under init_lock, and read locklessly as a fast-path
 * check by cs_ensure_init() (invoked at the top of every public API
 * entry point) — the flag only ever transitions false→true outside of
 * shutdown, so the unsynchronized read cannot observe a rollback.
 */
cs_global_state_t g_state = {
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

csStatus
cs_backend_unregister(const char *name)
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

	/*
	 * Force-close still-open devices. This is inside csShutdown() (the
	 * documented last API call of the process), so we take ownership of
	 * the handle slot directly: dev_close is invoked on the backend but
	 * per-handle cleanup events/callbacks are skipped. Not a normal close
	 * path — applications should csCloseCSx() before csShutdown().
	 */
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
