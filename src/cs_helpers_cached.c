/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_helpers_cached.c
 * @brief Cached-session convenience helpers for CS API
 */

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
/* WITH_XNVME_BACKEND is defined by the build system exactly when libxnvme
 * is linked; __has_include alone breaks builds where only the header is
 * installed (header visible, library absent). */
#if defined(WITH_XNVME_BACKEND) && __has_include(<libxnvme.h>)
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

/**
 * Per-thread cache for csRunComputeExCached.
 * Holds open device + allocated in/out memory handles so successive calls
 * with the same URI avoid the open/alloc/close overhead.
 */

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
