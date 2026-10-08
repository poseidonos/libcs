/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_internal.h
 * @brief CS API Internal Types and Handle Management
 */

#ifndef CS_INTERNAL_H
#define CS_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#include "cs_api.h"
#include "cs_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup cs_shared_helpers Shared Backend Helpers
 * @{
 */

/**
 * Parse a uint32_t from an environment variable (base auto-detected).
 *
 * Replaces the per-backend copies of this exact function that existed
 * in backend_ioctl.c / backend_io_uring.c / backend_libnvme.c.
 *
 * @param name Environment variable name
 * @param out  Parsed value (left untouched on failure)
 * @return true if the variable is set to a value in [0, UINT32_MAX]
 */
static inline bool
cs_parse_u32_env(const char *name, uint32_t *out)
{
	const char *val = getenv(name);
	if (!val || !val[0]) {
		return false;
	}
	char *end;
	unsigned long v = strtoul(val, &end, 0);
	if (end == val || *end != '\0' || v > UINT32_MAX) {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

/**
 * DMA buffer alignment used by the kernel-passthrough backends
 * (ioctl, io_uring, libnvme) when allocating command payloads.
 */
#define CS_DMA_ALIGNMENT 4096u

/**
 * Define the full non-Linux vtable fallback set for a Linux-only
 * backend (single definition point; previously duplicated verbatim in
 * backend_ioctl.c, backend_io_uring.c and backend_libnvme.c).
 *
 * @param p Backend function-name prefix (e.g. ioctl_backend); expands
 *          to `p##_init`, `p##_dev_open`, ... exactly matching the
 *          cs_backend_ops_t slot signatures in cs_backend.h.
 */
#define CS_DEFINE_BACKEND_STUBS(p)                                              \
	static csStatus p##_init(void)                                          \
	{ return CS_STATUS_ERROR_NOT_SUPPORTED; }                               \
	static csStatus p##_shutdown(void) { return CS_STATUS_SUCCESS; }        \
	static csStatus p##_dev_query(csDeviceInfo *d, uint32_t *c)             \
	{                                                                       \
		(void)d; if (c) *c = 0; return CS_STATUS_ERROR_NOT_SUPPORTED;   \
	}                                                                       \
	static csStatus p##_dev_open(const char *u, struct cs_backend_ctx **c)  \
	{                                                                       \
		(void)u; (void)c; return CS_STATUS_ERROR_NOT_SUPPORTED;         \
	}                                                                       \
	static csStatus p##_dev_close(struct cs_backend_ctx *c)                 \
	{ (void)c; return CS_STATUS_ERROR_NOT_SUPPORTED; }                      \
	static csStatus p##_dev_get_info(struct cs_backend_ctx *c,              \
					 csDeviceInfo *i)                         \
	{                                                                       \
		(void)c; (void)i; return CS_STATUS_ERROR_NOT_SUPPORTED;         \
	}                                                                       \
	static csStatus p##_mem_alloc(struct cs_backend_ctx *c, size_t s,       \
				      uint32_t a, void **p_)                    \
	{                                                                       \
		(void)c; (void)s; (void)a; (void)p_;                            \
		return CS_STATUS_ERROR_NOT_SUPPORTED;                           \
	}                                                                       \
	static csStatus p##_mem_free(struct cs_backend_ctx *c, void *p_)        \
	{                                                                       \
		(void)c; (void)p_; return CS_STATUS_ERROR_NOT_SUPPORTED;         \
	}                                                                       \
	static csStatus p##_prog_load(struct cs_backend_ctx *c, uint16_t i,     \
				      const void *p_, size_t s)                 \
	{                                                                       \
		(void)c; (void)i; (void)p_; (void)s;                            \
		return CS_STATUS_ERROR_NOT_SUPPORTED;                           \
	}                                                                       \
	static csStatus p##_prog_activate(struct cs_backend_ctx *c,             \
					  uint16_t i)                          \
	{ (void)c; (void)i; return CS_STATUS_ERROR_NOT_SUPPORTED; }             \
	static csStatus p##_prog_deactivate(struct cs_backend_ctx *c,           \
					    uint16_t i)                      \
	{ (void)c; (void)i; return CS_STATUS_ERROR_NOT_SUPPORTED; }             \
	static csStatus p##_compute_execute(struct cs_backend_ctx *c,           \
					    uint16_t i, const void *in,       \
					    size_t is, void *o, size_t os,    \
					    const void *a, size_t as,        \
					    volatile int *cp)                \
	{                                                                       \
		(void)c; (void)i; (void)in; (void)is; (void)o; (void)os;        \
		(void)a; (void)as; (void)cp;                                    \
		return CS_STATUS_ERROR_NOT_SUPPORTED;                           \
	}                                                                       \
	static csStatus p##_storage_io(struct cs_backend_ctx *c,                \
				       csStorageOp op, uint64_t l, uint32_t b,  \
				       void *buf, size_t bs,                   \
				       volatile int *cp)                       \
	{                                                                       \
		(void)c; (void)op; (void)l; (void)b; (void)buf; (void)bs;       \
		(void)cp; return CS_STATUS_ERROR_NOT_SUPPORTED;                 \
	}                                                                       \
	static int p##_poll(struct cs_backend_ctx *c, uint32_t m)               \
	{ (void)c; (void)m; return 0; }                                         \
	static csStatus p##_mem_init(struct cs_backend_ctx *c, void *p_,        \
				     uint8_t f, size_t s)                       \
	{                                                                       \
		(void)c; (void)p_; (void)f; (void)s;                            \
		return CS_STATUS_ERROR_NOT_SUPPORTED;                           \
	}                                                                       \
	static csStatus p##_prog_get_name(struct cs_backend_ctx *c,             \
					  uint16_t i, char *n, size_t ns)     \
	{                                                                       \
		(void)c; (void)i; (void)n; (void)ns;                            \
		return CS_STATUS_ERROR_NOT_SUPPORTED;                           \
	}                                                                       \
	static csStatus p##_query_csf_list(struct cs_backend_ctx *c,            \
					   csCSFInfo *csfs, uint32_t *cnt)   \
	{                                                                       \
		(void)c; (void)csfs; if (cnt) *cnt = 0;                         \
		return CS_STATUS_ERROR_NOT_SUPPORTED;                           \
	}                                                                       \
	static csStatus p##_compute_submit(struct cs_backend_ctx *c,            \
					   uint16_t i, const void *in,        \
					   size_t is, void *o, size_t os,     \
					   const void *a, size_t as,          \
					   volatile int *cp)                  \
	{                                                                       \
		(void)c; (void)i; (void)in; (void)is; (void)o; (void)os;        \
		(void)a; (void)as; (void)cp;                                    \
		return CS_STATUS_ERROR_NOT_SUPPORTED;                           \
	}                                                                       \
	static csStatus p##_compute_reap(struct cs_backend_ctx *c,              \
					 volatile int *cp)                     \
	{ (void)c; (void)cp; return CS_STATUS_ERROR_NOT_SUPPORTED; }            \
	static csStatus p##_abort_request(struct cs_backend_ctx *c,             \
					  volatile int *cp)                    \
	{ (void)c; (void)cp; return CS_STATUS_ERROR_NOT_SUPPORTED; }

/** @} */

/**
 * @defgroup cs_handle_encoding Handle Encoding
 *
 * Handle format (64 bits):
 *   [63:56] - Handle type (8 bits)
 *   [55:32] - Generation counter (24 bits)
 *   [31:0]  - Index into handle table (32 bits)
 * @{
 */

#define CS_HANDLE_TYPE_SHIFT    56
#define CS_HANDLE_TYPE_MASK     0xFF00000000000000ULL
#define CS_HANDLE_GEN_SHIFT     32
#define CS_HANDLE_GEN_MASK      0x00FFFFFF00000000ULL
#define CS_HANDLE_INDEX_MASK    0x00000000FFFFFFFFULL

/** Handle types */
typedef enum {
	CS_HANDLE_TYPE_DEVICE = 1,
	CS_HANDLE_TYPE_MEMORY = 2,
	CS_HANDLE_TYPE_EVENT  = 3,
	CS_HANDLE_TYPE_BATCH  = 4,
} cs_handle_type_t;

/** Encode a handle from components */
static inline uint64_t
cs_handle_encode(cs_handle_type_t type, uint32_t gen, uint32_t index)
{
	return ((uint64_t)type << CS_HANDLE_TYPE_SHIFT) |
	       ((uint64_t)(gen & 0xFFFFFF) << CS_HANDLE_GEN_SHIFT) |
	       (uint64_t)index;
}

/** Extract type from handle */
static inline cs_handle_type_t
cs_handle_get_type(uint64_t handle)
{
	return (cs_handle_type_t)((handle & CS_HANDLE_TYPE_MASK) >> CS_HANDLE_TYPE_SHIFT);
}

/** Extract generation from handle */
static inline uint32_t
cs_handle_get_gen(uint64_t handle)
{
	return (uint32_t)((handle & CS_HANDLE_GEN_MASK) >> CS_HANDLE_GEN_SHIFT);
}

/** Extract index from handle */
static inline uint32_t
cs_handle_get_index(uint64_t handle)
{
	return (uint32_t)(handle & CS_HANDLE_INDEX_MASK);
}

/** @} */

/**
 * @defgroup cs_context Context Structures
 * @{
 */

/** Maximum number of concurrent handles per type */
#define CS_MAX_DEVICES      64
#define CS_MAX_ALLOCATIONS  1024
#define CS_MAX_EVENTS       256
#define CS_MAX_BATCHES      64
#define CS_MAX_BATCH_ENTRIES 256
#define CS_MAX_GROUPS       32
#define CS_MAX_GROUP_CSFS   16

/**
 * Maximum number of program slots tracked per device for CSF/CSEE
 * download bookkeeping (csGetCSFId / csCSFDownload / csCSEEDownload).
 * Real backends currently report max_programs of 16-32; this is sized
 * with headroom. Slot 0 is reserved for a device's built-in/default
 * program and is never handed out by the allocator below.
 */
#define CS_MAX_PROGRAM_SLOTS 64

/** Device context */
typedef struct cs_device_ctx {
	bool in_use;                        /**< Slot is allocated */
	uint32_t generation;                /**< Generation counter for validation */
	pthread_mutex_t lock;               /**< Per-device mutex (program-slot bookkeeping only; see prog_slot_used below) */
	atomic_int refcount;                /**< Active reference count (TOCTOU guard) */
	/**
	 * Set (under the global `devices_lock`, in the same critical section
	 * as the not-busy refcount check) by csCloseCSx() before it calls
	 * backend->dev_close(). cs_device_lookup() checks this flag under the
	 * same `devices_lock` atomically with its own refcount-increment, so
	 * a lookup can never succeed once a close has committed to tearing
	 * the backend session down -- closing the TOCTOU window where a
	 * fresh lookup used to be able to hand out a reference to a device
	 * whose backend_ctx was already (or was about to be) destroyed.
	 * Reset to false by cs_device_alloc() when a slot is (re)allocated.
	 */
	bool closing;

	const cs_backend_ops_t *backend;    /**< Backend operations */
	struct cs_backend_ctx *backend_ctx; /**< Backend-specific context */

	char uri[256];                      /**< Device URI */
	csDeviceInfo info;                  /**< Cached device info */

	/**
	 * Program slot in-use tracking (Bug 1 fix). Index 0 is always
	 * reserved/in-use (built-in/default program); indices are claimed by
	 * csCSFDownload()/csCSEEDownload() under `lock` and released by
	 * csDeactivateProgram() on success. Reset on device (re)open by
	 * cs_device_alloc().
	 */
	atomic_bool prog_slot_used[CS_MAX_PROGRAM_SLOTS];
} cs_device_ctx_t;

/** Memory allocation context */
typedef struct cs_mem_ctx {
	bool in_use;                        /**< Slot is allocated */
	uint32_t generation;                /**< Generation counter */
	atomic_int refcount;                /**< Active reference count (TOCTOU guard) */
	/**
	 * Same purpose/locking discipline as cs_device_ctx_t.closing, but
	 * guarded by the global `allocations_lock` and checked by
	 * cs_mem_lookup(). Set by csFreeMem() before it calls
	 * backend->mem_free(); reset to false by cs_mem_alloc_ctx() when a
	 * slot is (re)allocated.
	 */
	bool closing;

	uint32_t device_index;              /**< Owning device index */
	void *ptr;                          /**< Host pointer */
	size_t size;                        /**< Allocation size */
	csMemType type;                     /**< Memory type */
} cs_mem_ctx_t;

/** Event context */
typedef struct cs_event_ctx {
	bool in_use;                        /**< Slot is allocated */
	uint32_t generation;                /**< Generation counter */
	atomic_int refcount;                /**< Active reference count (TOCTOU guard) */

	uint32_t device_index;              /**< Owning device index */
	atomic_int status;                  /**< Event status (csEventStatus) */
	atomic_int result;                  /**< Completion result (csStatus) */
	atomic_int completion_flag;         /**< Backend completion flag */

	/* Async callback (stored when csQueueComputeRequest returns PENDING) */
	csComputeCallback callback;         /**< Completion callback (NULL if none) */
	void *user_data;                    /**< User data for callback */
	bool callback_invoked;              /**< True after callback has been called */
} cs_event_ctx_t;

/** Batch context */
typedef struct cs_batch_ctx {
	bool in_use;
	uint32_t generation;
	atomic_int refcount;

	uint32_t device_index;
	csBatchMode mode;
	uint32_t max_entries;
	uint32_t entry_count;
	csRequest entries[CS_MAX_BATCH_ENTRIES];
} cs_batch_ctx_t;

/** Compute group context — scheduling hint for CSFs sharing a CSE */
typedef struct cs_group_ctx {
	bool in_use;
	uint32_t generation;
	uint32_t device_index;
	csCSFId csf_ids[CS_MAX_GROUP_CSFS];
	uint32_t csf_count;
} cs_group_ctx_t;

/** Global library state */
typedef struct cs_global_state {
	bool initialized;                   /**< Library is initialized */
	pthread_mutex_t init_lock;          /**< Initialization mutex */

	/* Handle tables */
	cs_device_ctx_t devices[CS_MAX_DEVICES];
	pthread_mutex_t devices_lock;

	cs_mem_ctx_t allocations[CS_MAX_ALLOCATIONS];
	pthread_mutex_t allocations_lock;

	cs_event_ctx_t events[CS_MAX_EVENTS];
	pthread_mutex_t events_lock;

	cs_batch_ctx_t batches[CS_MAX_BATCHES];
	pthread_mutex_t batches_lock;

	cs_group_ctx_t groups[CS_MAX_GROUPS];
	pthread_mutex_t groups_lock;

	/* Backend registry */
	const cs_backend_ops_t *backends[8];
	uint32_t backend_count;
	pthread_mutex_t backends_lock;
} cs_global_state_t;

/** @} */

/**
 * @defgroup cs_internal_funcs Internal Functions
 * @{
 */

/** Get global state (initializes if needed) */
cs_global_state_t *cs_get_state(void);

/** Ensure library is initialized */
csStatus cs_ensure_init(void);

/**
 * Device context management
 */
csStatus cs_device_alloc(cs_device_ctx_t **ctx, uint32_t *index);
csStatus cs_device_free(uint32_t index);
csStatus cs_device_lookup(csCSxHandle handle, cs_device_ctx_t **ctx);
void cs_device_release(cs_device_ctx_t *ctx);

/**
 * Memory context management
 */
csStatus cs_mem_alloc_ctx(uint32_t device_index, cs_mem_ctx_t **ctx,
			  csMemHandle *handle);
csStatus cs_mem_free_ctx(csMemHandle handle);
csStatus cs_mem_lookup(csMemHandle handle, cs_mem_ctx_t **ctx);
void cs_mem_release(cs_mem_ctx_t *ctx);

/**
 * Event context management
 */
csStatus cs_event_alloc_ctx(uint32_t device_index, cs_event_ctx_t **ctx,
			    csEventHandle *handle);
csStatus cs_event_free_ctx(csEventHandle handle);
csStatus cs_event_lookup(csEventHandle handle, cs_event_ctx_t **ctx);
void cs_event_release(cs_event_ctx_t *ctx);

csStatus cs_batch_alloc_ctx(uint32_t device_index, csBatchMode mode,
			    uint32_t max_entries, cs_batch_ctx_t **ctx,
			    csBatchHandle *handle);
csStatus cs_batch_free_ctx(csBatchHandle handle);
csStatus cs_batch_lookup(csBatchHandle handle, cs_batch_ctx_t **ctx);
void cs_batch_release(cs_batch_ctx_t *ctx);

csStatus cs_group_alloc_ctx(uint32_t device_index, cs_group_ctx_t **ctx,
			    uint32_t *group_id);
csStatus cs_group_free_ctx(uint32_t group_id);
csStatus cs_group_lookup(uint32_t group_id, cs_group_ctx_t **ctx);
void cs_group_release(cs_group_ctx_t *ctx);

/**
 * Error mapping utilities
 */

/** Map errno to CS status */
csStatus cs_map_errno(int err);

/** Map NVMe status code to CS status */
csStatus cs_map_nvme_status(uint16_t sc, uint16_t sct);

/**
 * Check whether a device URI refers to a directly-attached local device
 * (block path or PCIe URI) vs. a fabrics (host:port) remote target.
 *
 * @param uri Device URI string
 * @return true if local/direct, false if fabrics or unknown
 */
bool xnvme_uri_is_local_direct(const char *uri);

/**
 * Infer the number of output bytes produced by a CSF execution,
 * based on CSF type, input size, and output buffer contents.
 *
 * For fixed-output CSFs (SUM64, MAX64, etc.) returns the fixed size.
 * For variable-output CSFs (FILTER_GT, RLE_COMPRESS) parses the
 * output buffer to determine actual bytes produced.
 * For MEMCPY returns min(input_size, output_size).
 *
 * @param csf_id CSF identifier
 * @param input Input buffer (unused by most CSFs)
 * @param input_size Input buffer size
 * @param output Output buffer (read by variable-output CSFs)
 * @param output_size Output buffer capacity
 * @return Bytes actually produced, or 0 on error/empty
 */
size_t cs_infer_bytes_produced(csCSFId csf_id,
			       const void *input, size_t input_size,
			       const void *output, size_t output_size);

/** @} */

/**
 * @defgroup cs_debug Debug Utilities
 * @{
 */

/** Debug log level */
typedef enum {
	CS_LOG_ERROR = 0,
	CS_LOG_WARN  = 1,
	CS_LOG_INFO  = 2,
	CS_LOG_DEBUG = 3,
} cs_log_level_t;

/** Log a message (respects LIBCS_DEBUG environment variable) */
void cs_log(cs_log_level_t level, const char *fmt, ...);

#define CS_LOG_ERROR(fmt, ...) cs_log(CS_LOG_ERROR, fmt, ##__VA_ARGS__)
#define CS_LOG_WARN(fmt, ...)  cs_log(CS_LOG_WARN, fmt, ##__VA_ARGS__)
#define CS_LOG_INFO(fmt, ...)  cs_log(CS_LOG_INFO, fmt, ##__VA_ARGS__)
#define CS_LOG_DEBUG(fmt, ...) cs_log(CS_LOG_DEBUG, fmt, ##__VA_ARGS__)

/**
 * Last raw device completion value (cdw0/cdw1), per thread.
 *
 * Some programs return a scalar in the completion that cannot be recovered
 * from the output buffer -- RLE_COMPRESS writes the encoded bytes to the
 * output range and reports their length here, so the length is unrecoverable
 * once the call returns. Backends record it; helpers read it.
 */
void cs_set_last_device_result(uint64_t value);
uint64_t cs_get_last_device_result(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CS_INTERNAL_H */
