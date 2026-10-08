/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_helpers.c
 * @brief Convenience helpers for CS API
 */

#include <math.h>
#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "cs_helpers.h"
#include "cs_internal.h"
#include "cs_programs.h"

bool
xnvme_uri_is_local_direct(const char *uri)
{
	if (!uri || uri[0] == '\0') {
		return false;
	}
	if (uri[0] == '/') {
		return true;
	}
	if (!strncmp(uri, "pci:", 4) || !strncmp(uri, "pcie:", 5)) {
		return true;
	}
	return false;
}

#if __has_include(<libxnvme.h>)
#include <libxnvme.h>
#include <libxnvme_nvm.h>
#define CS_HELPERS_HAS_XNVME 1
#else
#define CS_HELPERS_HAS_XNVME 0
#endif

static uint64_t
cs_now_ns(void)
{
	struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static _Thread_local csLatencyBreakdown tl_last_latency;

csLatencyBreakdown
csGetLastLatencyBreakdown(void)
{
	return tl_last_latency;
}

static uint32_t
cs_element_size_for_csf(csCSFId csf_id)
{
	switch (csf_id) {
	case CS_CSF_SUM64:
	case CS_CSF_MAX64:
	case CS_CSF_MIN64:
		return sizeof(uint64_t);
	case CS_CSF_DOT_PRODUCT:
	case CS_CSF_FILTER_GT:
	case CS_CSF_L2_DISTANCE_SQ:
	case CS_CSF_COSINE_SIMILARITY:
		return sizeof(float);
	case CS_CSF_MULTI_AGG64:
		return sizeof(csMultiAgg64Result);
	default:
		return 1;
	}
}

size_t
cs_infer_bytes_produced(csCSFId csf_id,
			 const void *input, size_t input_size,
			 const void *output, size_t output_size)
{
	(void)input;

	switch (csf_id) {
	case CS_CSF_SUM64:
	case CS_CSF_MAX64:
	case CS_CSF_MIN64:
		return output_size >= sizeof(uint64_t) ? sizeof(uint64_t) : 0;
	case CS_CSF_DOT_PRODUCT:
	case CS_CSF_L2_DISTANCE_SQ:
	case CS_CSF_COSINE_SIMILARITY:
		return output_size >= sizeof(float) ? sizeof(float) : 0;
	case CS_CSF_MULTI_AGG64:
		return output_size >= sizeof(csMultiAgg64Result) ? sizeof(csMultiAgg64Result) : 0;
	case CS_CSF_MEMCPY:
		return input_size < output_size ? input_size : output_size;
	case CS_CSF_FILTER_GT: {
		if (!output || (output_size % sizeof(float)) != 0) {
			return 0;
		}
		const float *vals = (const float *)output;
		size_t max = output_size / sizeof(float);
		size_t count = 0;
		while (count < max && !isnan(vals[count])) {
			count++;
		}
		return count * sizeof(float);
	}
	case CS_CSF_RLE_COMPRESS: {
		uint64_t actual = cs_get_last_device_result();
		if (actual > 0 && actual <= output_size) {
			return (size_t)actual;
		}
		if (!output || (output_size % 2) != 0) {
			return 0;
		}
		const uint8_t *buf = (const uint8_t *)output;
		size_t len = 0;
		for (size_t i = 0; i + 1 < output_size; i += 2) {
			if (buf[i] == 0) {
				break;
			}
			len = i + 2;
		}
		return len;
	}
	default:
		return output_size;
	}
}

static void
cs_prepare_variable_output(csCSFId csf_id, void *output, size_t output_size)
{
	if (!output || output_size == 0) {
		return;
	}

	if (csf_id == CS_CSF_FILTER_GT && (output_size % sizeof(float)) == 0) {
		float *vals = (float *)output;
		size_t count = output_size / sizeof(float);
		for (size_t i = 0; i < count; i++) {
			vals[i] = NAN;
		}
		return;
	}

	if (csf_id == CS_CSF_RLE_COMPRESS) {
		memset(output, 0, output_size);
	}
}

csStatus
csRunCompute(const char *uri, csCSFId csf_id,
	     const void *input, size_t input_size,
	     void *output, size_t output_size,
	     const void *args, size_t args_size)
{
	return csRunComputeEx(uri, csf_id, input, input_size,
			      output, output_size, args, args_size, NULL);
}

csStatus
csRunComputeEx(const char *uri, csCSFId csf_id,
	       const void *input, size_t input_size,
	       void *output, size_t output_size,
	       const void *args, size_t args_size,
	       csComputeResult *result)
{
	csStatus status = CS_STATUS_SUCCESS;
	csStatus first_error = CS_STATUS_SUCCESS;
	csCSxHandle dev = CS_INVALID_HANDLE;
	csMemHandle in_mem = CS_INVALID_HANDLE;
	csMemHandle out_mem = CS_INVALID_HANDLE;
	void *in_ptr = NULL;
	void *out_ptr = NULL;
	csComputeDesc desc;

	if (result) {
		result->status = CS_STATUS_ERROR_INVALID_PARAM;
		result->bytes_produced = 0;
		result->elements_produced = 0;
	}

	if (!uri || !input || input_size == 0 || !output || output_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	status = csInit();
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	status = csOpenCSx(uri, &dev);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	csMemDesc in_desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = input_size,
		.alignment = 0,
		.flags = 0,
	};
	status = csAllocMem(dev, &in_desc, &in_mem, &in_ptr);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	csMemDesc out_desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = output_size,
		.alignment = 0,
		.flags = 0,
	};
	status = csAllocMem(dev, &out_desc, &out_mem, &out_ptr);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	status = csQueueCopyMemRequest(dev, NULL, CS_INVALID_HANDLE,
				       (void *)input, (void *)(uintptr_t)in_mem,
				       input_size, CS_COPY_HOST_TO_DEVICE);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	cs_prepare_variable_output(csf_id, out_ptr, output_size);

	desc = csMakeComputeDescArgs(csf_id, in_ptr, input_size,
				     out_ptr, output_size,
				     args, args_size);
	status = csQueueComputeRequest(dev, &desc, CS_INVALID_HANDLE, NULL, NULL);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	status = csQueueCopyMemRequest(dev, NULL, CS_INVALID_HANDLE,
				       (void *)(uintptr_t)out_mem, output,
				       output_size, CS_COPY_DEVICE_TO_HOST);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	if (result) {
		uint32_t element_size = cs_element_size_for_csf(csf_id);
		result->bytes_produced = cs_infer_bytes_produced(csf_id, input, input_size,
							output, output_size);
		result->elements_produced = element_size > 0 ?
			(uint32_t)(result->bytes_produced / element_size) : 0;
	}

done:
	if (out_mem != CS_INVALID_HANDLE) {
		status = csFreeMem(dev, out_mem);
		if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
			first_error = status;
		}
	}
	if (in_mem != CS_INVALID_HANDLE) {
		status = csFreeMem(dev, in_mem);
		if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
			first_error = status;
		}
	}
	if (dev != CS_INVALID_HANDLE) {
		status = csCloseCSx(dev);
		if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
			first_error = status;
		}
	}
	status = csShutdown();
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
	}

	if (result) {
		result->status = first_error;
	}
	return first_error;
}

csStatus
csRunComputeAsync(csCSxHandle device, const csComputeDesc *desc,
		  uint32_t timeout_ms, csComputeResult *result)
{
	if (result) {
		result->status = CS_STATUS_ERROR_INVALID_PARAM;
		result->bytes_produced = 0;
		result->elements_produced = 0;
	}

	if (device == CS_INVALID_HANDLE || !desc) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_prepare_variable_output(desc->csf_id, desc->output_buffer, desc->output_size);

	csEventHandle event = CS_INVALID_HANDLE;
	csStatus status = csCreateEvent(device, &event);
	if (status != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = status;
		}
		return status;
	}

	status = csQueueComputeRequest(device, desc, event, NULL, NULL);
	if (status != CS_STATUS_SUCCESS && status != CS_STATUS_PENDING) {
		csDestroyEvent(device, event);
		if (result) {
			result->status = status;
		}
		return status;
	}

	csStatus completed = CS_STATUS_ERROR_BACKEND;
	status = csWaitEvent(device, event, timeout_ms, &completed);
	csStatus destroy_status = csDestroyEvent(device, event);

	if (status != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = status;
		}
		return status;
	}
	if (completed != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = completed;
		}
		return completed;
	}
	if (destroy_status != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = destroy_status;
		}
		return destroy_status;
	}

	if (result) {
		uint32_t element_size = cs_element_size_for_csf(desc->csf_id);
		result->status = CS_STATUS_SUCCESS;
		result->bytes_produced = cs_infer_bytes_produced(desc->csf_id,
							 desc->input_buffer, desc->input_size,
							 desc->output_buffer, desc->output_size);
		result->elements_produced = element_size > 0 ?
			(uint32_t)(result->bytes_produced / element_size) : 0;
	}

	return CS_STATUS_SUCCESS;
}

csStatus
csScopeOpen(csScope *scope, const char *uri)
{
	if (!scope || !uri) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	memset(scope, 0, sizeof(*scope));
	scope->device = CS_INVALID_HANDLE;
	for (int i = 0; i < CS_SCOPE_MAX_MEMS; i++) {
		scope->mems[i] = CS_INVALID_HANDLE;
	}
	for (int i = 0; i < CS_SCOPE_MAX_EVENTS; i++) {
		scope->events[i] = CS_INVALID_HANDLE;
	}

	csStatus status = csInit();
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	status = csOpenCSx(uri, &scope->device);
	if (status != CS_STATUS_SUCCESS) {
		csShutdown();
		return status;
	}

	return CS_STATUS_SUCCESS;
}

void *
csScopeAllocMem(csScope *scope, size_t size, csMemHandle *out_handle)
{
	if (out_handle) {
		*out_handle = CS_INVALID_HANDLE;
	}
	if (!scope || scope->device == CS_INVALID_HANDLE || size == 0) {
		return NULL;
	}
	if (scope->mem_count >= CS_SCOPE_MAX_MEMS) {
		return NULL;
	}

	csMemDesc desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = size,
		.alignment = 0,
		.flags = 0,
	};

	csMemHandle mem = CS_INVALID_HANDLE;
	void *ptr = NULL;
	csStatus status = csAllocMem(scope->device, &desc, &mem, &ptr);
	if (status != CS_STATUS_SUCCESS) {
		return NULL;
	}

	scope->mems[scope->mem_count] = mem;
	scope->mem_ptrs[scope->mem_count] = ptr;
	scope->mem_count++;

	if (out_handle) {
		*out_handle = mem;
	}
	return ptr;
}

void
csScopeClose(csScope *scope)
{
	if (!scope) {
		return;
	}

	for (int i = scope->event_count - 1; i >= 0; i--) {
		if (scope->events[i] != CS_INVALID_HANDLE && scope->device != CS_INVALID_HANDLE) {
			csDestroyEvent(scope->device, scope->events[i]);
		}
		scope->events[i] = CS_INVALID_HANDLE;
	}
	scope->event_count = 0;

	for (int i = scope->mem_count - 1; i >= 0; i--) {
		if (scope->mems[i] != CS_INVALID_HANDLE && scope->device != CS_INVALID_HANDLE) {
			csFreeMem(scope->device, scope->mems[i]);
		}
		scope->mems[i] = CS_INVALID_HANDLE;
		scope->mem_ptrs[i] = NULL;
	}
	scope->mem_count = 0;

	if (scope->device != CS_INVALID_HANDLE) {
		csCloseCSx(scope->device);
		scope->device = CS_INVALID_HANDLE;
	}

	csShutdown();
}

csComputeDesc
csMakeComputeDesc(csCSFId csf_id,
		  const void *input, size_t input_size,
		  void *output, size_t output_size)
{
	return csMakeComputeDescArgs(csf_id, input, input_size,
				    output, output_size,
				    NULL, 0);
}

csComputeDesc
csMakeComputeDescArgs(csCSFId csf_id,
		      const void *input, size_t input_size,
		      void *output, size_t output_size,
		      const void *args, size_t args_size)
{
	csComputeDesc desc;
	memset(&desc, 0, sizeof(desc));
	desc.csf_id = csf_id;
	desc.input_buffer = input;
	desc.input_size = input_size;
	desc.output_buffer = output;
	desc.output_size = output_size;
	desc.args = args;
	desc.args_size = args_size;
	return desc;
}

/**
 * Per-thread cache for csRunComputeExCached.
 * Holds open device + allocated in/out memory handles so successive calls
 * with the same URI avoid the open/alloc/close overhead.
 */
#define CS_CACHED_URI_MAX 256

typedef struct {
	char      uri[CS_CACHED_URI_MAX];
	csCSxHandle dev;
	csMemHandle in_mem;
	csMemHandle out_mem;
	void     *in_ptr;
	void     *out_ptr;
	size_t    in_size;
	size_t    out_size;
	bool      valid;
} cs_cached_session_t;

static _Thread_local cs_cached_session_t tl_cache;
static _Thread_local bool tl_cache_init = false;

/* NVM h2d cache: when LIBCS_MRS_STAGE_VIA_NVM=1, the first call writes
 * input to NVM via the backend.  Subsequent calls with the same size
 * skip the h2d copy — the DMA buffer contents are unused because the
 * backend reads from NVM, not from the DMA buffer. */
static _Thread_local bool   tl_nvm_h2d_done  = false;
static _Thread_local size_t tl_nvm_h2d_size  = 0;

static void
cs_cached_session_teardown(cs_cached_session_t *c)
{
	if (!c->valid) {
		return;
	}
	if (c->out_mem != CS_INVALID_HANDLE) {
		csFreeMem(c->dev, c->out_mem);
		c->out_mem = CS_INVALID_HANDLE;
	}
	if (c->in_mem != CS_INVALID_HANDLE) {
		csFreeMem(c->dev, c->in_mem);
		c->in_mem = CS_INVALID_HANDLE;
	}
	if (c->dev != CS_INVALID_HANDLE) {
		csCloseCSx(c->dev);
		csShutdown();
		c->dev = CS_INVALID_HANDLE;
	}
	c->in_ptr  = NULL;
	c->out_ptr = NULL;
	c->in_size = 0;
	c->out_size = 0;
	c->uri[0]  = '\0';
	c->valid   = false;
}

static void
cs_cached_session_init(cs_cached_session_t *c)
{
	c->dev     = CS_INVALID_HANDLE;
	c->in_mem  = CS_INVALID_HANDLE;
	c->out_mem = CS_INVALID_HANDLE;
	c->in_ptr  = NULL;
	c->out_ptr = NULL;
	c->in_size = 0;
	c->out_size = 0;
	c->uri[0]  = '\0';
	c->valid   = false;
}

/**
 * Ensure the cached session is open for the given URI with at least the
 * requested buffer sizes. Reopens (and reallocates) if URI changed or if
 * existing buffers are too small.
 */
static csStatus
cs_cached_session_ensure(cs_cached_session_t *c, const char *uri,
			 size_t in_size, size_t out_size)
{
	/* URI change → tear down existing session */
	if (c->valid && strncmp(c->uri, uri, CS_CACHED_URI_MAX - 1) != 0) {
		cs_cached_session_teardown(c);
	}

	if (!c->valid) {
		csStatus status = csInit();
		if (status != CS_STATUS_SUCCESS) {
			return status;
		}
		status = csOpenCSx(uri, &c->dev);
		if (status != CS_STATUS_SUCCESS) {
			csShutdown();
			return status;
		}
		strncpy(c->uri, uri, CS_CACHED_URI_MAX - 1);
		c->uri[CS_CACHED_URI_MAX - 1] = '\0';
		c->valid = true;
	}

	/* (Re-)allocate input buffer if needed */
	if (in_size > c->in_size) {
		if (c->in_mem != CS_INVALID_HANDLE) {
			csFreeMem(c->dev, c->in_mem);
			c->in_mem  = CS_INVALID_HANDLE;
			c->in_ptr  = NULL;
			c->in_size = 0;
		}
		csMemDesc d = { .type = CS_MEM_TYPE_AFDM, .size = in_size };
		csStatus status = csAllocMem(c->dev, &d, &c->in_mem, &c->in_ptr);
		if (status != CS_STATUS_SUCCESS) {
			cs_cached_session_teardown(c);
			return status;
		}
		c->in_size = in_size;
	}

	/* (Re-)allocate output buffer if needed */
	if (out_size > c->out_size) {
		if (c->out_mem != CS_INVALID_HANDLE) {
			csFreeMem(c->dev, c->out_mem);
			c->out_mem  = CS_INVALID_HANDLE;
			c->out_ptr  = NULL;
			c->out_size = 0;
		}
		csMemDesc d = { .type = CS_MEM_TYPE_AFDM, .size = out_size };
		csStatus status = csAllocMem(c->dev, &d, &c->out_mem, &c->out_ptr);
		if (status != CS_STATUS_SUCCESS) {
			cs_cached_session_teardown(c);
			return status;
		}
		c->out_size = out_size;
	}

	return CS_STATUS_SUCCESS;
}

csStatus
csRunComputeExCached(const char *uri, csCSFId csf_id,
		     const void *input, size_t input_size,
		     void *output, size_t output_size,
		     const void *args, size_t args_size,
		     csComputeResult *result)
{
	if (result) {
		result->status = CS_STATUS_ERROR_INVALID_PARAM;
		result->bytes_produced = 0;
		result->elements_produced = 0;
	}

	if (!uri || !input || input_size == 0 || !output || output_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (!tl_cache_init) {
		cs_cached_session_init(&tl_cache);
		tl_cache_init = true;
	}

	cs_cached_session_t *c = &tl_cache;

	/* Instrumentation: cache-ensure (device open / buffer alloc) is
	 * NOT covered by the h2d/execute/d2h phase timers below.  Large
	 * wall-clock gaps between DuckDB's .timer and libcs [LATENCY]
	 * usually live here (per-thread cold csOpenCSx over NVMe-oF TCP
	 * can be 200-400ms).  Enable with LIBCS_TRACE=1. */
	bool was_valid_before_ensure = c->valid;
	uint64_t t_ensure0 = cs_now_ns();

	csStatus status = cs_cached_session_ensure(c, uri, input_size, output_size);
	if (status != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = status;
		}
		return status;
	}

	uint64_t t_ensure1 = cs_now_ns();
	{
		const char *trace = getenv("LIBCS_TRACE");
		if (trace && *trace == '1') {
			double ensure_ms = (double)(t_ensure1 - t_ensure0) / 1e6;
			fprintf(stderr,
				"[LATENCY] ensure_ms=%.4f cache_hit=%d thread=%lu\n",
				ensure_ms, was_valid_before_ensure ? 1 : 0,
				(unsigned long)pthread_self());
		}
	}

	/* ── Phase 1: Host → Device copy ── */
	uint64_t t_phase0 = cs_now_ns();

	bool skip_h2d = false;
	{
		const char *env = getenv("LIBCS_MRS_STAGE_VIA_NVM");
		if (env && *env == '1' && tl_nvm_h2d_done && tl_nvm_h2d_size == input_size)
			skip_h2d = true;
	}

	if (!skip_h2d) {
		status = csQueueCopyMemRequest(c->dev, NULL, CS_INVALID_HANDLE,
					       (void *)input,
					       (void *)(uintptr_t)c->in_mem,
					       input_size, CS_COPY_HOST_TO_DEVICE);
		if (status != CS_STATUS_SUCCESS) {
			if (result) {
				result->status = status;
			}
			return status;
		}
		tl_nvm_h2d_done = true;
		tl_nvm_h2d_size = input_size;
	}

	uint64_t t_phase1 = cs_now_ns();

	/* ── Phase 2: Compute execution ── */
	cs_prepare_variable_output(csf_id, c->out_ptr, output_size);

	csComputeDesc desc = csMakeComputeDescArgs(csf_id,
						   c->in_ptr, input_size,
						   c->out_ptr, output_size,
						   args, args_size);
	status = csQueueComputeRequest(c->dev, &desc, CS_INVALID_HANDLE, NULL, NULL);
	if (status != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = status;
		}
		return status;
	}

	uint64_t t_phase2 = cs_now_ns();

	/* ── Phase 3: Device → Host copy ── */
	size_t d2h_size = output_size;
	{
		uint64_t actual = cs_get_last_device_result();
		if (actual > 0 && actual < output_size)
			d2h_size = (size_t)actual;
	}
	status = csQueueCopyMemRequest(c->dev, NULL, CS_INVALID_HANDLE,
				       (void *)(uintptr_t)c->out_mem,
				       output,
				       d2h_size, CS_COPY_DEVICE_TO_HOST);
	if (status != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = status;
		}
		return status;
	}

	uint64_t t_phase3 = cs_now_ns();

	/* Record per-phase latency in thread-local storage */
	tl_last_latency.h2d_ms     = (double)(t_phase1 - t_phase0) / 1e6;
	tl_last_latency.execute_ms = (double)(t_phase2 - t_phase1) / 1e6;
	tl_last_latency.d2h_ms     = (double)(t_phase3 - t_phase2) / 1e6;
	tl_last_latency.total_ms   = (double)(t_phase3 - t_phase0) / 1e6;

	if (result) {
		uint32_t element_size = cs_element_size_for_csf(csf_id);
		result->bytes_produced = cs_infer_bytes_produced(csf_id, input, input_size,
								 output, output_size);
		result->elements_produced = element_size > 0 ?
			(uint32_t)(result->bytes_produced / element_size) : 0;
		result->status = CS_STATUS_SUCCESS;
	}

	return CS_STATUS_SUCCESS;
}

void
csRunComputeExCachedCleanup(void)
{
	if (tl_cache_init) {
		cs_cached_session_teardown(&tl_cache);
	}
}

#if CS_HELPERS_HAS_XNVME

typedef struct {
	struct xnvme_dev *nvm_dev;
	struct xnvme_dev *slm_dev;
	uint32_t          lba_nbytes;
	char              uri[256];
	uint32_t          nvm_nsid;
	uint32_t          slm_nsid;
} cs_slm_stage_cache_t;

static _Thread_local cs_slm_stage_cache_t tl_slm_cache;
static _Thread_local bool                 tl_slm_cache_init = false;

static void
cs_slm_stage_cache_teardown(cs_slm_stage_cache_t *c)
{
	if (c->slm_dev)  { xnvme_dev_close(c->slm_dev);  c->slm_dev  = NULL; }
	if (c->nvm_dev)  { xnvme_dev_close(c->nvm_dev);  c->nvm_dev  = NULL; }
	c->lba_nbytes = 0;
}

static csStatus
cs_slm_stage_cache_ensure(cs_slm_stage_cache_t *c, const char *uri,
			   uint32_t nvm_nsid, uint32_t slm_nsid)
{
	/* Reuse cached handles if target/nsids unchanged */
	if (c->nvm_dev && c->slm_dev && strcmp(c->uri, uri) == 0 &&
	    c->nvm_nsid == nvm_nsid && c->slm_nsid == slm_nsid)
		return CS_STATUS_SUCCESS;

	cs_slm_stage_cache_teardown(c);

	struct xnvme_opts opts = xnvme_opts_default();
	const char *be     = getenv("LIBCS_XNVME_BE");
	const char *subnqn = getenv("LIBCS_XNVME_SUBNQN");
	const char *adrfam = getenv("LIBCS_XNVME_ADRFAM");
	if (be)     opts.be     = be;
	if (subnqn) opts.subnqn = subnqn;
	opts.adrfam = adrfam ? adrfam : "IPv4";
	/* SPDK multi-process attach (mirrors backend_xnvme.c). */
	{
		const char *shm = getenv("LIBCS_XNVME_SHM_ID");
		if (shm && shm[0]) {
			char *end = NULL;
			unsigned long v = strtoul(shm, &end, 0);
			if (end != shm && *end == '\0' && v <= 0xFFFFFFFFUL) {
				opts.shm_id = (uint32_t)v;
			}
		}
	}

	opts.nsid  = nvm_nsid;
	c->nvm_dev = xnvme_dev_open(uri, &opts);
	if (!c->nvm_dev)
		return CS_STATUS_ERROR_IO;
	if (xnvme_dev_derive_geo(c->nvm_dev)) {
		xnvme_dev_close(c->nvm_dev); c->nvm_dev = NULL;
		return CS_STATUS_ERROR_IO;
	}
	c->lba_nbytes = xnvme_dev_get_geo(c->nvm_dev)->lba_nbytes;

	opts.nsid  = slm_nsid;
	c->slm_dev = xnvme_dev_open(uri, &opts);
	if (!c->slm_dev) {
		xnvme_dev_close(c->nvm_dev); c->nvm_dev = NULL;
		return CS_STATUS_ERROR_IO;
	}

	snprintf(c->uri, sizeof(c->uri), "%s", uri);
	c->nvm_nsid  = nvm_nsid;
	c->slm_nsid  = slm_nsid;
	return CS_STATUS_SUCCESS;
}

csStatus
csSlmStageFromNvm(const char *device_uri,
		  uint32_t nvm_nsid, uint32_t slm_nsid,
		  uint64_t nvm_slba, uint64_t slm_byte_off,
		  uint32_t n_uint64)
{
	if (!device_uri)
		return CS_STATUS_ERROR_INVALID_PARAM;

	if (!tl_slm_cache_init) {
		memset(&tl_slm_cache, 0, sizeof(tl_slm_cache));
		tl_slm_cache_init = true;
	}

	csStatus s = cs_slm_stage_cache_ensure(&tl_slm_cache, device_uri,
					       nvm_nsid, slm_nsid);
	if (s != CS_STATUS_SUCCESS)
		return s;

	cs_slm_stage_cache_t *c = &tl_slm_cache;
	size_t data_bytes  = (size_t)n_uint64 * sizeof(uint64_t);
	size_t copy_bytes  = ((data_bytes + c->lba_nbytes - 1) /
			      c->lba_nbytes) * c->lba_nbytes;

	/* SLM_COPY descriptor only crosses the link; bulk data stays on target.
	 * Match the inline 09_fair_bench staging cap: 8 MiB, clamped so the
	 * descriptor's 16-bit nlb (value = nblocks - 1) cannot overflow. */
	uint32_t max_chunk_blocks = (8u * 1024u * 1024u) / c->lba_nbytes;
	if (max_chunk_blocks > 65535u) {
		max_chunk_blocks = 65535u;
	}
	const uint32_t max_chunk = max_chunk_blocks * c->lba_nbytes;
	uint64_t blk      = nvm_slba;
	uint64_t slm_off  = slm_byte_off;
	size_t   remaining = copy_bytes;

	while (remaining > 0) {
		size_t   chunk   = remaining < max_chunk ? remaining : max_chunk;
		uint32_t nblocks = (uint32_t)(chunk / c->lba_nbytes);
		if (nblocks == 0) nblocks = 1;
		chunk = (size_t)nblocks * c->lba_nbytes;

		struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(c->slm_dev);
		int rc = xnvme_cmd_slm_copy_lba(&cmd, slm_nsid,
						 slm_off,
						 nvm_nsid, blk,
						 (uint16_t)(nblocks - 1),
						 (uint32_t)chunk,
						 XNVME_SLM_COPY_DESC_FMT2);
		if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
			fprintf(stderr,
				"csSlmStageFromNvm: blk=%" PRIu64
				" rc=%d sc=%" PRIu16 " sct=%" PRIu16 "\n",
				blk, rc,
				cmd.cpl.status.sc, cmd.cpl.status.sct);
			return CS_STATUS_ERROR_IO;
		}
		blk       += nblocks;
		slm_off   += chunk;
		remaining -= chunk;
	}
	return CS_STATUS_SUCCESS;
}

void
csSlmStageFromNvmCachedCleanup(void)
{
	if (tl_slm_cache_init)
		cs_slm_stage_cache_teardown(&tl_slm_cache);
}

csStatus
csRunComputeFromNvm(const char *device_uri,
		     uint32_t nvm_nsid, uint64_t nvm_slba,
		     uint32_t slm_nsid, uint64_t slm_byte_off,
		     uint32_t n_uint64,
		     csCSFId csf_id,
		     uint64_t threshold, uint64_t batch,
		     void *output, size_t output_size,
		     csComputeResult *result)
{
	if (result) {
		result->status = CS_STATUS_ERROR_INVALID_PARAM;
		result->bytes_produced = 0;
		result->elements_produced = 0;
	}

	if (!device_uri || n_uint64 == 0 || !output || output_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	uint16_t pind = cs_prog_index_from_csf(csf_id);

	if (pind != LIBCS_PIND_SUM64 && pind != LIBCS_PIND_MAX64 &&
	    pind != LIBCS_PIND_MIN64 && pind != LIBCS_PIND_DOT_PRODUCT &&
	    pind != LIBCS_PIND_L2_DISTANCE_SQ &&
	    pind != LIBCS_PIND_COSINE_SIMILARITY &&
	    pind != LIBCS_PIND_MULTI_AGG64 &&
	    pind != LIBCS_PIND_FILTER_GT &&
	    pind != LIBCS_PIND_RLE_COMPRESS) {
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	size_t data_bytes = (size_t)n_uint64 * sizeof(uint64_t);

	csComputeDesc desc;
	memset(&desc, 0, sizeof(desc));
	desc.csf_id       = csf_id;
	desc.input_size   = data_bytes;
	desc.output_buffer = output;
	desc.output_size  = output_size;
	desc.nvm_nsid     = nvm_nsid;
	desc.nvm_slba     = nvm_slba;
	desc.slm_nsid     = slm_nsid;
	desc.slm_byte_off = slm_byte_off;
	desc.threshold    = threshold;
	desc.batch        = batch;

	if (!tl_cache_init) {
		cs_cached_session_init(&tl_cache);
		tl_cache_init = true;
	}

	cs_cached_session_t *c = &tl_cache;

	csStatus status = cs_cached_session_ensure(c, device_uri,
						   data_bytes, output_size);
	if (status != CS_STATUS_SUCCESS) {
		if (result) result->status = status;
		return status;
	}

	status = csQueueComputeRequest(c->dev, &desc,
				      CS_INVALID_HANDLE, NULL, NULL);
	if (status != CS_STATUS_SUCCESS) {
		if (result) result->status = status;
		return status;
	}

	if (result) {
		uint32_t element_size = cs_element_size_for_csf(csf_id);
		/* For the two workloads whose output length is data-dependent
		 * (FILTER_GT in survivor-return mode and RLE_COMPRESS), the
		 * backend publishes the actual byte count it read back on the
		 * device-result channel.  Reporting output_size -- the BUFFER
		 * size -- instead made bytes_produced a constant, which hid the
		 * one quantity selectivity/entropy actually move.  Callers
		 * should not have to reach for the internal symbol to find it. */
		size_t produced = output_size;
		if ((pind == LIBCS_PIND_FILTER_GT && output_size > sizeof(uint64_t)) ||
		    pind == LIBCS_PIND_RLE_COMPRESS) {
			uint64_t actual = cs_get_last_device_result();
			if (actual > 0 && actual <= (uint64_t)output_size) {
				produced = (size_t)actual;
			}
		}
		result->bytes_produced = produced;
		result->elements_produced = element_size > 0 ?
			(uint32_t)(produced / element_size) : 0;
		result->status = CS_STATUS_SUCCESS;
	}

	return CS_STATUS_SUCCESS;
}

#else /* !CS_HELPERS_HAS_XNVME */

csStatus
csSlmStageFromNvm(const char *device_uri,
		  uint32_t nvm_nsid, uint32_t slm_nsid,
		  uint64_t nvm_slba, uint64_t slm_byte_off,
		  uint32_t n_uint64)
{
	(void)device_uri; (void)nvm_nsid; (void)slm_nsid;
	(void)nvm_slba; (void)slm_byte_off; (void)n_uint64;
	return CS_STATUS_ERROR_NOT_SUPPORTED;
}

void csSlmStageFromNvmCachedCleanup(void) {}

csStatus
csRunComputeFromNvm(const char *device_uri,
		     uint32_t nvm_nsid, uint64_t nvm_slba,
		     uint32_t slm_nsid, uint64_t slm_byte_off,
		     uint32_t n_uint64,
		     csCSFId csf_id,
		     uint64_t threshold, uint64_t batch,
		     void *output, size_t output_size,
		     csComputeResult *result)
{
	(void)device_uri; (void)nvm_nsid; (void)nvm_slba;
	(void)slm_nsid; (void)slm_byte_off; (void)n_uint64;
	(void)csf_id; (void)threshold; (void)batch;
	(void)output; (void)output_size; (void)result;
	return CS_STATUS_ERROR_NOT_SUPPORTED;
}

#endif /* CS_HELPERS_HAS_XNVME */
