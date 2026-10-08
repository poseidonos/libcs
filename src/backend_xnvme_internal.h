/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file backend_xnvme_internal.h
 * @brief Shared internals of the xNVMe backend (split from backend_xnvme.c)
 *
 * Internal to the xNVMe backend translation units (backend_xnvme.c,
 * backend_xnvme_slm.c, backend_xnvme_compute.c, backend_xnvme_misc.c).
 * Do not install; do not include outside src/.
 */

#ifndef BACKEND_XNVME_INTERNAL_H
#define BACKEND_XNVME_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(__APPLE__)
/* glibc-only header; provide equivalent little-endian conversions on
 * Darwin so the same source builds everywhere (wire format unchanged:
 * these are identity on little-endian, the only wire-relevant case). */
#include <libkern/OSByteOrder.h>
#define htole16(x) ((uint16_t)OSSwapHostToLittleInt16(x))
#define htole32(x) ((uint32_t)OSSwapHostToLittleInt32(x))
#define htole64(x) ((uint64_t)OSSwapHostToLittleInt64(x))
#define le16toh(x) ((uint16_t)OSSwapLittleToHostInt16(x))
#define le32toh(x) ((uint32_t)OSSwapLittleToHostInt32(x))
#define le64toh(x) ((uint64_t)OSSwapLittleToHostInt64(x))
#else
#include <endian.h>
#endif
#include "cs_internal.h"
#include "cs_programs.h"

/* xNVMe headers */
#include <libxnvme.h>
#include <libxnvme_nvm.h>
#include <libxnvme_slm.h>
#include <libxnvme_cs.h>

/* Full definition below; the backend context only holds a pointer. */
struct slm_rb_pool;

/*
 * Shared SLM readback worker types.  The pool is owned by the backend
 * context (ctx->rb_pool) and torn down from backend_xnvme.c, so its
 * layout must be visible across the backend's translation units.
 */
/**
 * xNVMe backend context
 */
struct cs_backend_ctx {
	struct xnvme_dev *dev;          /**< xNVMe device handle (NVM ns) */
	struct xnvme_dev *slm_dev;      /**< SLM device handle (NULL if unavailable) */
	struct xnvme_queue *queue;      /**< Command queue for async ops */
	struct xnvme_queue *slm_queue;  /**< Lazily created queue on slm_dev for
					  *  async SLM readback (NULL until used) */
	bool slm_rb_async_disabled;     /**< Set after an async readback failure;
					  *  later readbacks go straight to sync */
	uint32_t slm_rb_chunk;          /**< Async readback chunk that last
					  *  succeeded (0 = not yet probed) */
	char *uri;                      /**< dev_open URI, for extra readback
					  *  connections */
	struct xnvme_opts slm_opts;     /**< Options slm_dev was opened with */
	struct slm_rb_pool *rb_pool;    /**< Parallel readback workers
					  *  (LIBCS_SLM_RB_CONNS > 1; NULL until
					  *  first large readback) */
	bool rb_par_disabled;           /**< Set after a parallel readback
					  *  failure; later readbacks use one
					  *  connection */
	uint8_t *rb_dma;                /**< Cached SLM readback DMA buffer
					  *  (grows only). */
	size_t   rb_dma_len;            /**< Allocated size of @rb_dma, 0 if
					  *  none. */
	uint32_t nsid;                  /**< Namespace ID (NVM, for device ops) */
	uint32_t libcs_nsid;             	/**< Compute Namespace ID (for compute cmds) */
	uint32_t slm_nsid;              /**< SLM Namespace ID (for staging) */
	uint32_t lba_nbytes;            /**< NVM LBA size in bytes */
	uint16_t mrs_rsid;              /**< Current MRS resource ID (0=none) */
	uint64_t mrs_data_bytes;        /**< Size of data staged in current MRS */
	struct xnvme_opts opts;         /**< Device options */
	bool cp_output_returns;         /**< CP Execute inline output half is
					  *  visible to the host on completion
					  *  (local/PCIe direct-attach). False
					  *  for NVMe-oF fabrics, where the
					  *  inline SGL is host->controller
					  *  only and the output half is never
					  *  written back into host memory.
					  *  Decided once from the URI at
					  *  dev_open() time -- this is a
					  *  static property of the transport,
					  *  not something derivable from a
					  *  command's completion status or by
					  *  sniffing buffer contents.
					  *
					  *  Documented gap (D1): the ioctl/libnvme/io_uring
					  *  backends record the equivalent flag but never
					  *  branch on it -- see docs/coverage-gaps.md
					  *  section 3. */
};
/**
 * Decide whether the CP Execute inline output buffer round-trips back to
 * the host for a given device URI.
 *
 * Local / direct-attach URIs ("/dev/nvmeXnY", "pci:...", "pcie:...") map
 * onto transports where the controller's write into the output half of an
 * inline command buffer is visible to the host once the command completes.
 * NVMe-oF fabrics URIs (bare "host:port", used for TCP/RDMA target
 * connections including SPDK loopback) are host->controller only for
 * inline data -- the device never DMAs the output half back to the host's
 * copy of the buffer, regardless of whether the command succeeded.
 *
 * This is intentionally a static, connection-time decision instead of a
 * per-call heuristic: an NVMe completion's status code only reports
 * whether the device executed the command, not whether a specific inline
 * SGL is bidirectional on this transport, and a legitimately all-zero
 * result is indistinguishable from "buffer never arrived" if that is
 * inferred from data content on every call.
 */

/* ------------------------------------------------------------------ */
/*  Program index constants now in cs_programs.h                       */
/* ------------------------------------------------------------------ */


/*
 * Offsets into the (packed) on-wire descriptor layouts shared by the
 * MRS execute helpers (backend_xnvme_compute.c) and the survivor / RLE
 * readback tails (backend_xnvme_slm.c).
 */
/** One MRS range. Ranges are addressed by the device as 1-based mr_id. */
struct cs_mrs_range {
	uint64_t starting_byte;   /**< Byte offset within the SLM namespace */
	uint32_t length;          /**< Range length in bytes */
};

#define CS_MRS_DESC_BYTES 32u

/* Program Execute descriptor sizes (on-wire, see xnvme_slm_execute_desc()):
 * input  [mr_id(8)][offset(8)][length(8)] = 24 bytes; output
 * [mr_id(8)][offset(8)][length(4)][reserved(4)] = 24 bytes; the FILTER_GT
 * input layout adds [threshold(4)][pad(4)] = 32 bytes. */
#define CS_PROG_DESC_IN_BYTES		24u
#define CS_PROG_DESC_OUT_BYTES		24u
#define CS_FILTER_GT_DESC_BYTES		(CS_PROG_DESC_IN_BYTES + 8u)

struct slm_rb_state {
	uint32_t errors;
	uint16_t last_sc;
};

struct slm_rb_worker {
	struct slm_rb_pool *pool;
	pthread_t tid;
	bool started;
	struct xnvme_dev *dev;
	struct xnvme_queue *queue;
	uint8_t *bounce;
	size_t bounce_len;
	/* current job */
	uint64_t src;
	size_t len;
	uint8_t *dst;
	size_t copy;
	int rc;
};

struct slm_rb_pool {
	struct cs_backend_ctx *ctx;
	uint32_t nworkers;
	struct slm_rb_worker *w;
	pthread_mutex_t mtx;
	pthread_cond_t cv_job;
	pthread_cond_t cv_done;
	uint64_t gen;
	uint32_t pending;
	uint32_t open_failed;
	uint32_t chunk;
	bool stop;
};

/* ---- shared helpers ------------------------------------------------- */

bool parse_u32_env(const char *name, uint32_t *out);

void slm_rb_pool_destroy(struct slm_rb_pool *p);

/* ---- SLM staging / readback (backend_xnvme_slm.c) ------------------- */

csStatus xnvme_slm_write_nvm(struct cs_backend_ctx *ctx, const void *data,
			     size_t len);
csStatus xnvme_slm_stage(struct cs_backend_ctx *ctx, size_t data_bytes);
csStatus xnvme_nvm_to_slm_copy_batch(struct cs_backend_ctx *ctx,
				     uint32_t nvm_nsid, uint64_t nvm_slba,
				     uint32_t slm_nsid, uint64_t slm_off,
				     size_t nbytes);
csStatus xnvme_slm_read_region(struct cs_backend_ctx *ctx, uint64_t src_addr,
			       void *dst, size_t nbytes);
csStatus xnvme_slm_write_region(struct cs_backend_ctx *ctx, uint64_t dst_addr,
				const void *src, size_t nbytes);
csStatus slm_rb_filter_survivors(struct cs_backend_ctx *ctx, uint16_t rsid,
				 uint64_t out_base, size_t data_size,
				 uint32_t threshold_bits, void *output,
				 size_t output_size,
				 volatile int *completion);
csStatus slm_rb_rle_readback(struct cs_backend_ctx *ctx, uint16_t rsid,
			     size_t data_size, uint64_t out_base,
			     uint64_t out_desc_off, uint32_t out_cap,
			     void *output, size_t output_size,
			     uint64_t *compressed_out);

/* ---- MRS + compute (backend_xnvme_compute.c) ------------------------ */

void xnvme_slm_delete_mrs(struct cs_backend_ctx *ctx, uint16_t rsid);
csStatus xnvme_slm_execute_desc(struct cs_backend_ctx *ctx,
				uint16_t prog_index, uint16_t rsid,
				const void *desc, size_t desc_len,
				struct xnvme_cmd_ctx *out_cmd);
void compute_completion_cb(struct xnvme_cmd_ctx *ctx, void *opaque);

/* ---- backend vtable operations (backend_xnvme_misc.c ops table) ----- */

csStatus xnvme_backend_init(void);
csStatus xnvme_backend_shutdown(void);
csStatus xnvme_backend_dev_query(csDeviceInfo *devices, uint32_t *count);
csStatus xnvme_backend_dev_open(const char *uri,
				struct cs_backend_ctx **backend_ctx);
csStatus xnvme_backend_dev_close(struct cs_backend_ctx *ctx);
csStatus xnvme_backend_dev_get_info(struct cs_backend_ctx *ctx,
				     csDeviceInfo *info);
csStatus xnvme_backend_mem_alloc(struct cs_backend_ctx *ctx, size_t size,
				 uint32_t alignment, void **ptr);
csStatus xnvme_backend_mem_free(struct cs_backend_ctx *ctx, void *ptr);
csStatus xnvme_backend_prog_load(struct cs_backend_ctx *ctx,
				 uint16_t prog_index, const void *program,
				 size_t program_size);
csStatus xnvme_backend_prog_activate(struct cs_backend_ctx *ctx,
				      uint16_t prog_index);
csStatus xnvme_backend_prog_deactivate(struct cs_backend_ctx *ctx,
					uint16_t prog_index);
csStatus xnvme_backend_storage_io(struct cs_backend_ctx *ctx, csStorageOp op,
				  uint64_t lba, uint32_t blocks, void *buffer,
				  size_t buffer_size,
				  volatile int *completion);
int xnvme_backend_poll(struct cs_backend_ctx *ctx, uint32_t max);
csStatus xnvme_backend_prog_get_name(struct cs_backend_ctx *ctx,
				     uint16_t prog_index, char *name,
				     size_t name_size);
csStatus xnvme_backend_query_csf_list(struct cs_backend_ctx *ctx,
				      csCSFInfo *csfs, uint32_t *count);
csStatus xnvme_backend_mem_init(struct cs_backend_ctx *ctx, void *ptr,
				 uint8_t fill_value, size_t size);
csStatus xnvme_backend_compute_execute(struct cs_backend_ctx *ctx,
					uint16_t prog_index, const void *input,
					size_t input_size, void *output,
					size_t output_size, const void *args,
					size_t args_size,
					volatile int *completion);
csStatus xnvme_backend_compute_execute_nvm(struct cs_backend_ctx *ctx,
					    uint16_t prog_index,
					    uint32_t nvm_nsid,
					    uint64_t nvm_slba,
					    uint32_t slm_nsid,
					    uint64_t slm_byte_off,
					    size_t data_size, void *output,
					    size_t output_size,
					    uint64_t threshold, uint64_t batch,
					    volatile int *completion);
csStatus xnvme_backend_compute_submit(struct cs_backend_ctx *ctx,
				       uint16_t prog_index,
				       const void *input, size_t input_size,
				       void *output, size_t output_size,
				       const void *args, size_t args_size,
				       volatile int *completion);
csStatus xnvme_backend_compute_reap(struct cs_backend_ctx *ctx,
				     volatile int *completion);
csStatus xnvme_backend_abort_request(struct cs_backend_ctx *ctx,
				     volatile int *completion);
csStatus xnvme_backend_query_device_properties(struct cs_backend_ctx *ctx,
						csDeviceProperties *props);
csStatus xnvme_backend_query_device_statistics(struct cs_backend_ctx *ctx,
						csDeviceStatistics *stats);
csStatus xnvme_backend_device_config(struct cs_backend_ctx *ctx,
				     uint32_t config_id, const void *data,
				     size_t size);
csStatus xnvme_backend_device_reset(struct cs_backend_ctx *ctx,
				     uint32_t reset_type);
csStatus xnvme_backend_batch_submit(struct cs_backend_ctx *ctx,
				     csBatchHandle batch,
				     csComputeCallback callback,
				     csEventHandle event);

#endif /* BACKEND_XNVME_INTERNAL_H */
