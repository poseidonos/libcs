/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_helpers.h
 * @brief Convenience helpers for CS API
 */

#ifndef CS_HELPERS_H
#define CS_HELPERS_H

#include <stddef.h>
#include <stdint.h>

#include "cs_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	csStatus status;
	size_t bytes_produced;
	uint32_t elements_produced;
} csComputeResult;

/**
 * Per-phase latency breakdown (milliseconds) from the most recent
 * csRunComputeExCached() call on the current thread.
 *
 * For the CSD path (csRunComputeExCached), the three phases are:
 *   h2d_ms      — Host-to-Device copy (input descriptor → AFDM)
 *   execute_ms  — Compute execution (eBPF on CSD / CP command)
 *   d2h_ms      — Device-to-Host copy (result → host buffer)
 *   total_ms    — Wall-clock sum of all three phases
 */
typedef struct {
	double h2d_ms;
	double execute_ms;
	double d2h_ms;
	double total_ms;
} csLatencyBreakdown;

/**
 * Retrieve the latency breakdown from the most recent csRunComputeExCached()
 * call on the current thread.  Returns zeroes if no call has been made yet.
 */
csLatencyBreakdown csGetLastLatencyBreakdown(void);

typedef struct {
	uint64_t count;
	double sum;
	double min;
	double max;
} csMultiAgg64Result;

#define CS_SCOPE_MAX_MEMS   16
#define CS_SCOPE_MAX_EVENTS 8

/**
 * Convenience scope that bundles a device handle with its associated
 * memory and event handles for automatic cleanup in csScopeClose().
 *
 * Thread-safety: a csScope is NOT thread-safe.  It must be created,
 * used, and destroyed on a single thread.  The underlying csCSxHandle
 * is thread-safe, but the scope's internal arrays are not protected.
 */
typedef struct {
	csCSxHandle device;
	csMemHandle mems[CS_SCOPE_MAX_MEMS];
	void *mem_ptrs[CS_SCOPE_MAX_MEMS];
	int mem_count;
	csEventHandle events[CS_SCOPE_MAX_EVENTS];
	int event_count;
} csScope;

/* CS_CSF_* constants are defined as macros in cs_api.h (included above).
 * Keep the typedef so call sites compile without changes. */
typedef uint32_t csBuiltinCSF;

csStatus csRunCompute(const char *uri, csCSFId csf_id,
			      const void *input, size_t input_size,
			      void *output, size_t output_size,
			      const void *args, size_t args_size);

csStatus csRunComputeEx(const char *uri, csCSFId csf_id,
			const void *input, size_t input_size,
			void *output, size_t output_size,
			const void *args, size_t args_size,
			csComputeResult *result);

csStatus csRunComputeAsync(csCSxHandle device, const csComputeDesc *desc,
			   uint32_t timeout_ms, csComputeResult *result);

csStatus csScopeOpen(csScope *scope, const char *uri);
void *csScopeAllocMem(csScope *scope, size_t size, csMemHandle *out_handle);
void csScopeClose(csScope *scope);

csComputeDesc csMakeComputeDesc(csCSFId csf_id,
				const void *input, size_t input_size,
				void *output, size_t output_size);

csComputeDesc csMakeComputeDescArgs(csCSFId csf_id,
				    const void *input, size_t input_size,
				    void *output, size_t output_size,
				    const void *args, size_t args_size);

/**
 * csRunComputeExCached - like csRunComputeEx but caches device/memory handles
 * per-thread for the lifetime of the thread (or until URI changes).
 * Avoids repeated open/alloc/close overhead on successive calls with the same URI.
 *
 * Call csRunComputeExCachedCleanup() to release cached resources explicitly
 * (e.g. at thread exit or teardown). Resources are also released automatically
 * when the URI changes.
 */
csStatus csRunComputeExCached(const char *uri, csCSFId csf_id,
			      const void *input, size_t input_size,
			      void *output, size_t output_size,
			      const void *args, size_t args_size,
			      csComputeResult *result);

void csRunComputeExCachedCleanup(void);

/**
 * csSlmStageFromNvm - copy data from NVM namespace to SLM namespace.
 *
 * Issues a chunked slm_copy_lba sequence from nvm_nsid to slm_nsid over the
 * NVMe-oF transport.  Device handles are cached per-thread (opened on the
 * first call and reused on subsequent calls) so that per-iteration staging in
 * a benchmark loop does not pay repeated device-open costs.
 *
 * Reads env vars LIBCS_XNVME_BE, LIBCS_XNVME_SUBNQN, LIBCS_XNVME_ADRFAM for
 * connection parameters.
 *
 * @param device_uri   NVMe-oF target URI (e.g. "127.0.0.1:4420")
 * @param nvm_nsid     Source namespace ID (NVM storage, e.g. 1)
 * @param slm_nsid     Destination namespace ID (SLM, e.g. 100)
 * @param nvm_slba     Starting LBA in source namespace
 * @param slm_byte_off Starting byte offset in SLM
 * @param n_uint64     Number of uint64 elements to copy
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csSlmStageFromNvm(const char *device_uri,
			   uint32_t nvm_nsid, uint32_t slm_nsid,
			   uint64_t nvm_slba, uint64_t slm_byte_off,
			   uint32_t n_uint64);

/**
 * Release cached device handles opened by csSlmStageFromNvm on this thread.
 */
void csSlmStageFromNvmCachedCleanup(void);

/**
 * csRunComputeFromNvm - execute a CSF on data already in NVM, without
 * transferring it through the host.
 *
 * Stages the NVM region into SLM via a device-internal copy (slm_copy_lba),
 * creates an MRS pointing to the SLM region, then executes the requested
 * CSF via a 24-byte MRS descriptor.  No host-to-device data copy occurs;
 * the only bytes sent over the fabric are the MRS create (32 B) and the
 * MRS execute descriptor (24 B).
 *
 * Device handles and the MRS are cached per-thread (same as
 * csRunComputeExCached).  The MRS is reused
 * across calls with the same slm_byte_off and data size.
 *
 * Supports: SUM64, MAX64, MIN64, DOT_PRODUCT, L2_DISTANCE_SQ,
 *           COSINE_SIMILARITY, MULTI_AGG64, FILTER_GT, RLE_COMPRESS
 *
 * @param[in]  device_uri   NVMe-oF target URI
 * @param[in]  nvm_nsid     Source NVM namespace ID
 * @param[in]  nvm_slba     Starting LBA in NVM namespace
 * @param[in]  slm_nsid     Destination SLM namespace ID
 * @param[in]  slm_byte_off Byte offset in SLM for staged data
 * @param[in]  n_uint64     Number of uint64 elements (= total bytes / 8)
 * @param[in]  csf_id       CS function ID (CS_CSF_SUM64, etc.)
 * @param[in]  threshold    FILTER_GT comparison value (raw float bits in low 32);
 *                          0 for other workloads
 * @param[in]  batch        Number of vector pairs for batch workloads
 *                          (DOT_PRODUCT/L2/COSINE). 0 or 1 = single.
 * @param[out] output       Result buffer (uint64 for SUM/MAX/MIN, float for
 *                          vector, 32 bytes for MULTI_AGG64, uint64 for
 *                          FILTER_GT count / RLE_COMPRESS compressed size)
 * @param[in]  output_size  Size of result buffer
 * @param[out] result       Optional csComputeResult metadata (may be NULL)
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csRunComputeFromNvm(const char *device_uri,
			     uint32_t nvm_nsid, uint64_t nvm_slba,
			     uint32_t slm_nsid, uint64_t slm_byte_off,
			     uint32_t n_uint64,
			     csCSFId csf_id,
			     uint64_t threshold, uint64_t batch,
			     void *output, size_t output_size,
			     csComputeResult *result);

#ifdef __cplusplus
}
#endif

#endif /* CS_HELPERS_H */
