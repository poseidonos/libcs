/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file backend_xnvme.c
 * @brief xNVMe Backend for CS API
 *
 * This backend wraps the xNVMe library to provide computational
 * storage functionality via NVMe devices.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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

struct slm_rb_pool;
static void slm_rb_pool_destroy(struct slm_rb_pool *p);

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
	uint32_t libcs_nsid;             /**< CPCS Namespace ID (for compute cmds) */
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
					  *  sniffing buffer contents. */
};

/**
 * xNVMe backend state
 */
static struct {
	bool initialized;
} xnvme_state = { .initialized = false };

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


static bool
parse_u32_env(const char *name, uint32_t *out)
{
	const char *val = getenv(name);
	char *endp = NULL;
	unsigned long parsed;

	if (!name || !out || !val || val[0] == '\0') {
		return false;
	}

	parsed = strtoul(val, &endp, 0);
	if (endp == val || *endp != '\0' || parsed > UINT32_MAX) {
		return false;
	}

	*out = (uint32_t)parsed;
	return true;
}

/**
 * Backend operations implementation
 */

static csStatus
xnvme_backend_init(void)
{
	if (xnvme_state.initialized) {
		return CS_STATUS_SUCCESS;
	}

	CS_LOG_INFO("xNVMe backend initializing");
	xnvme_state.initialized = true;
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_shutdown(void)
{
	if (!xnvme_state.initialized) {
		return CS_STATUS_SUCCESS;
	}

	CS_LOG_INFO("xNVMe backend shutting down");
	xnvme_state.initialized = false;
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_dev_query(csDeviceInfo *devices, uint32_t *count)
{
	if (!count) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	/* Check for device URI from environment */
	const char *uri = getenv("LIBCS_DEVICE_URI");
	if (!uri) {
		*count = 0;
		return CS_STATUS_SUCCESS;
	}

	/* If caller just wants count */
	if (!devices) {
		*count = 1;
		return CS_STATUS_SUCCESS;
	}

	if (*count < 1) {
		*count = 1;
		return CS_STATUS_ERROR_LIMIT;
	}

	/* Fill in device info */
	memset(&devices[0], 0, sizeof(csDeviceInfo));
	strncpy(devices[0].uri, uri, sizeof(devices[0].uri) - 1);
	strncpy(devices[0].vendor, "xNVMe", sizeof(devices[0].vendor) - 1);
	strncpy(devices[0].model, "CS Device", sizeof(devices[0].model) - 1);
	devices[0].max_programs = 16;
	devices[0].max_memory_mb = 256;

	*count = 1;
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_dev_open(const char *uri, struct cs_backend_ctx **backend_ctx)
{
	if (!uri || !backend_ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	*backend_ctx = NULL;

	/* Allocate context */
	struct cs_backend_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return CS_STATUS_ERROR_NO_MEMORY;
	}

	/* Initialize default options */
	ctx->opts = xnvme_opts_default();

	/* Check for backend preference in environment */
	const char *be = getenv("LIBCS_XNVME_BE");
	if (be) {
		ctx->opts.be = be;
	}

	const char *async_iface = getenv("LIBCS_XNVME_ASYNC");
	if (async_iface && async_iface[0] != '\0') {
		ctx->opts.async = async_iface;
	}

	/* Optional SPDK-fabrics parameters for xNVMe URI like "host:port". */
	const char *subnqn = getenv("LIBCS_XNVME_SUBNQN");
	if (subnqn) {
		ctx->opts.subnqn = subnqn;
	}
	const char *hostnqn = getenv("LIBCS_XNVME_HOSTNQN");
	if (hostnqn) {
		ctx->opts.hostnqn = hostnqn;
	}
	const char *adrfam = getenv("LIBCS_XNVME_ADRFAM");
	if (adrfam) {
		ctx->opts.adrfam = adrfam;
	}
	/*
	 * LIBCS_XNVME_NSID = CPCS namespace (for compute commands, e.g. 2).
	 * LIBCS_XNVME_NVM_NSID = NVM namespace to open the device with (default 1).
	 *
	 * xnvme_dev_open() must be called with a standard NVM namespace because
	 * CPCS namespaces don't support Identify Namespace and cause ENXIO.
	 * The CPCS nsid is stored separately and used only in CP execute calls.
	 */
	uint32_t libcs_nsid = 2; /* default CPCS namespace */
	{
		const char *nsid_env = getenv("LIBCS_XNVME_NSID");
		if (nsid_env && nsid_env[0] != '\0') {
			char *endp = NULL;
			unsigned long nsid = strtoul(nsid_env, &endp, 0);
			if (endp != nsid_env && *endp == '\0' && nsid > 0 && nsid <= UINT32_MAX) {
				libcs_nsid = (uint32_t)nsid;
			}
		}
	}
	ctx->libcs_nsid = libcs_nsid;

	/* NVM namespace for device open (default 1) */
	uint32_t nvm_nsid = 1;
	(void)parse_u32_env("LIBCS_XNVME_NVM_NSID", &nvm_nsid);
	if (nvm_nsid == 0) {
		nvm_nsid = 1;
	}
	ctx->opts.nsid = nvm_nsid;
	{
		uint32_t val = 0;
		if (parse_u32_env("LIBCS_XNVME_MAIN_CORE", &val)) {
			ctx->opts.main_core = val;
		}
		if (parse_u32_env("LIBCS_XNVME_ADMIN_TIMEOUT", &val)) {
			ctx->opts.admin_timeout = val;
		}
		if (parse_u32_env("LIBCS_XNVME_COMMAND_TIMEOUT", &val)) {
			ctx->opts.command_timeout = val;
		}
		ctx->opts.poll_io = parse_u32_env("LIBCS_XNVME_POLL_IO", &val) && (val != 0) ? 1 : 0;
	}
	{
		const char *core_mask = getenv("LIBCS_XNVME_CORE_MASK");
		if (core_mask && core_mask[0] != '\0') {
			ctx->opts.core_mask = core_mask;
		}
	}
	{
		uint32_t val = 0;
		if (parse_u32_env("LIBCS_XNVME_SHM_ID", &val)) {
			ctx->opts.shm_id = val;
		}
	}

	/* Open device */
	ctx->dev = xnvme_dev_open(uri, &ctx->opts);
	if (!ctx->dev) {
		int err = errno;
		CS_LOG_ERROR("xnvme_dev_open failed: %s (%d)", strerror(err), err);
		free(ctx);
		return cs_map_errno(err);
	}

	/* See xnvme_uri_is_local_direct(): decided once, not per-command. */
	ctx->cp_output_returns = xnvme_uri_is_local_direct(uri);

	/* Get namespace ID and geometry */
	ctx->nsid = xnvme_dev_get_nsid(ctx->dev);
	{
		const struct xnvme_geo *geo = xnvme_dev_get_geo(ctx->dev);
		ctx->lba_nbytes = (geo && geo->lba_nbytes) ? geo->lba_nbytes : 512;
	}

	/*
	 * Open SLM namespace if LIBCS_XNVME_SLM_NSID is set.
	 * This enables the MRS path: data is staged to SLM and compute
	 * commands reference it via MRS descriptors (24 bytes only).
	 */
	ctx->slm_dev = NULL;
	ctx->slm_nsid = 0;
	ctx->mrs_rsid = 0;
	ctx->mrs_data_bytes = 0;
	{
		uint32_t slm_ns = 0;
		if (parse_u32_env("LIBCS_XNVME_SLM_NSID", &slm_ns) && slm_ns > 0) {
			struct xnvme_opts slm_opts = ctx->opts;
			slm_opts.nsid = slm_ns;
			ctx->slm_dev = xnvme_dev_open(uri, &slm_opts);
			if (ctx->slm_dev) {
				ctx->slm_nsid = slm_ns;
				ctx->slm_opts = slm_opts;
				ctx->uri = strdup(uri);
				CS_LOG_INFO("xNVMe: SLM namespace %u opened (MRS path enabled)", slm_ns);
			} else {
				CS_LOG_WARN("xNVMe: SLM ns=%u open failed (%s), MRS path disabled",
					    slm_ns, strerror(errno));
			}
		}
	}

	/* Initialize async queue */
	uint32_t qd = 16;
	(void)parse_u32_env("LIBCS_XNVME_QD", &qd);
	if (qd == 0 || qd > UINT16_MAX + 1U) {
		qd = 16;
	}
	int ret = xnvme_queue_init(ctx->dev, (uint16_t)qd, 0, &ctx->queue);
	if (ret) {
		CS_LOG_WARN("xnvme_queue_init failed: %d (async ops disabled)", ret);
		ctx->queue = NULL;
	}

	*backend_ctx = ctx;
	CS_LOG_INFO("xNVMe: opened %s (nvm_nsid=%u, libcs_nsid=%u, slm_nsid=%u, lba=%u)",
		    uri, ctx->nsid, ctx->libcs_nsid, ctx->slm_nsid, ctx->lba_nbytes);
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_dev_close(struct cs_backend_ctx *ctx)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (ctx->queue) {
		xnvme_queue_term(ctx->queue);
		ctx->queue = NULL;
	}

	slm_rb_pool_destroy(ctx->rb_pool);
	ctx->rb_pool = NULL;

	if (ctx->rb_dma) {
		if (ctx->slm_dev)
			xnvme_buf_free(ctx->slm_dev, ctx->rb_dma);
		ctx->rb_dma = NULL;
		ctx->rb_dma_len = 0;
	}

	if (ctx->slm_queue) {
		xnvme_queue_term(ctx->slm_queue);
		ctx->slm_queue = NULL;
	}

	if (ctx->slm_dev) {
		xnvme_dev_close(ctx->slm_dev);
		ctx->slm_dev = NULL;
	}

	if (ctx->dev) {
		xnvme_dev_close(ctx->dev);
		ctx->dev = NULL;
	}

	free(ctx->uri);
	free(ctx);
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_dev_get_info(struct cs_backend_ctx *ctx, csDeviceInfo *info)
{
	if (!ctx || !info) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	memset(info, 0, sizeof(*info));

	const struct xnvme_ident *ident = xnvme_dev_get_ident(ctx->dev);
	if (ident) {
		strncpy(info->uri, ident->uri, sizeof(info->uri) - 1);
	}

	const struct xnvme_spec_idfy_ctrlr *ctrlr = xnvme_dev_get_ctrlr(ctx->dev);
	if (ctrlr) {
		/* Extract vendor/model from identify data */
		strncpy(info->vendor, ctrlr->vid ? "NVMe" : "Unknown",
			sizeof(info->vendor) - 1);
		info->vendor[sizeof(info->vendor) - 1] = '\0';
		size_t mn_len = sizeof(ctrlr->mn) < sizeof(info->model) - 1 ?
				sizeof(ctrlr->mn) : sizeof(info->model) - 1;
		memcpy(info->model, ctrlr->mn, mn_len);
		info->model[mn_len] = '\0';
	}

	/* Get CS namespace info if available */
	const struct xnvme_spec_idfy_ns *ns_css = xnvme_dev_get_ns_css(ctx->dev);
	if (ns_css) {
		/* Cast to CS namespace structure */
		const struct xnvme_spec_cp_idfy_ns *cp_ns =
			(const struct xnvme_spec_cp_idfy_ns *)ns_css;
		info->max_programs = cp_ns->maxact;
		info->max_memory_mb = (uint32_t)cp_ns->maxpb;
	} else {
		info->max_programs = 16;
		info->max_memory_mb = 256;
	}

	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_mem_alloc(struct cs_backend_ctx *ctx, size_t size,
			uint32_t alignment, void **ptr)
{
	if (!ctx || !ptr || size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	(void)alignment; /* xNVMe handles alignment internally */

	*ptr = xnvme_buf_alloc(ctx->dev, size);
	if (!*ptr) {
		int err = errno;
		CS_LOG_ERROR("xnvme_buf_alloc failed: %s (%d)", strerror(err), err);
		return cs_map_errno(err);
	}

	/* Clear the buffer */
	xnvme_buf_clear(*ptr, size);

	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_mem_free(struct cs_backend_ctx *ctx, void *ptr)
{
	if (!ctx || !ptr) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	xnvme_buf_free(ctx->dev, ptr);
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_prog_load(struct cs_backend_ctx *ctx, uint16_t prog_index,
			const void *program, size_t program_size)
{
	if (!ctx || !program || program_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	/* Get command context */
	struct xnvme_cmd_ctx cmd_ctx = xnvme_cmd_ctx_from_dev(ctx->dev);

	/* Execute load command */
	int ret = xnvme_cmd_cp_load(&cmd_ctx, ctx->libcs_nsid, prog_index,
				    program, program_size);
	if (ret) {
		CS_LOG_ERROR("xnvme_cmd_cp_load failed: %d", ret);
		return cs_map_errno(-ret);
	}

	/* Check completion status */
	if (xnvme_cmd_ctx_cpl_status(&cmd_ctx)) {
		uint16_t sc = cmd_ctx.cpl.status.sc;
		uint16_t sct = cmd_ctx.cpl.status.sct;
		CS_LOG_ERROR("CP load failed: sc=0x%" PRIx16 " sct=0x%" PRIx16 "", sc, sct);
		return cs_map_nvme_status(sc, sct);
	}

	CS_LOG_INFO("Loaded program at index %" PRIu16 " (%zu bytes)", prog_index, program_size);
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_prog_activate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	struct xnvme_cmd_ctx cmd_ctx = xnvme_cmd_ctx_from_dev(ctx->dev);

	/* Action 0 = Activate */
	int ret = xnvme_cmd_cp_activate(&cmd_ctx, ctx->libcs_nsid, prog_index, 0);
	if (ret) {
		CS_LOG_ERROR("xnvme_cmd_cp_activate failed: %d", ret);
		return cs_map_errno(-ret);
	}

	if (xnvme_cmd_ctx_cpl_status(&cmd_ctx)) {
		uint16_t sc = cmd_ctx.cpl.status.sc;
		uint16_t sct = cmd_ctx.cpl.status.sct;
		CS_LOG_ERROR("CP activate failed: sc=0x%" PRIx16 " sct=0x%" PRIx16 "", sc, sct);
		return cs_map_nvme_status(sc, sct);
	}

	CS_LOG_INFO("Activated program at index %" PRIu16 "", prog_index);
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_prog_deactivate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	struct xnvme_cmd_ctx cmd_ctx = xnvme_cmd_ctx_from_dev(ctx->dev);

	/* Action 1 = Deactivate */
	int ret = xnvme_cmd_cp_activate(&cmd_ctx, ctx->libcs_nsid, prog_index, 1);
	if (ret) {
		CS_LOG_ERROR("xnvme_cmd_cp_activate (deactivate) failed: %d", ret);
		return cs_map_errno(-ret);
	}

	if (xnvme_cmd_ctx_cpl_status(&cmd_ctx)) {
		uint16_t sc = cmd_ctx.cpl.status.sc;
		uint16_t sct = cmd_ctx.cpl.status.sct;
		CS_LOG_ERROR("CP deactivate failed: sc=0x%" PRIx16 " sct=0x%" PRIx16 "", sc, sct);
		return cs_map_nvme_status(sc, sct);
	}

	CS_LOG_INFO("Deactivated program at index %" PRIu16 "", prog_index);
	return CS_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Program index constants now in cs_programs.h                       */
/* ------------------------------------------------------------------ */
#define ALIGN4(_n) (((_n) + 3U) & ~((size_t)3U))

struct libcs_multi_agg64_result {
	uint64_t count;
	double sum;
	double min;
	double max;
};

/* ------------------------------------------------------------------ */
/*  SLM / MRS helpers for near-data compute path                      */
/* ------------------------------------------------------------------ */

/**
 * Write host data to NVM namespace starting at LBA 0.
 */
static csStatus
xnvme_slm_write_nvm(struct cs_backend_ctx *ctx, const void *data, size_t len)
{
	/* Pad allocation to LBA boundary for NVMe write */
	size_t padded = ((len + ctx->lba_nbytes - 1) / ctx->lba_nbytes) * ctx->lba_nbytes;
	uint8_t *dma = xnvme_buf_alloc(ctx->dev, padded);
	if (!dma) return cs_map_errno(errno);
	memset(dma, 0, padded);
	memcpy(dma, data, len);

	struct timespec _ts0, _ts1;
	bool _trace = getenv("LIBCS_TRACE") && *getenv("LIBCS_TRACE") == '1';
	if (_trace) clock_gettime(CLOCK_MONOTONIC_RAW, &_ts0);

	uint64_t slba = 0;
	size_t off = 0;
	while (off < len) {
		size_t chunk = len - off;
		if (chunk > 33554432) chunk = 33554432; /* 32 MiB per write (matches mrs_pind_bench) */
		uint32_t blocks = (uint32_t)(chunk / ctx->lba_nbytes);
		if (blocks == 0) blocks = 1;
		chunk = (size_t)blocks * ctx->lba_nbytes;

		struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(ctx->dev);
		int rc = xnvme_nvm_write(&cmd, ctx->nsid, slba,
					 (uint16_t)(blocks - 1), dma + off, NULL);
		if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
			xnvme_buf_free(ctx->dev, dma);
			CS_LOG_ERROR("NVM write failed at off=%zu: rc=%d", off, rc);
			return CS_STATUS_ERROR_IO;
		}
		off += chunk;
		slba += blocks;
	}
	xnvme_buf_free(ctx->dev, dma);

	if (_trace) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &_ts1);
		double _ms = (double)(_ts1.tv_sec - _ts0.tv_sec) * 1e3 +
			     (double)(_ts1.tv_nsec - _ts0.tv_nsec) / 1e6;
		fprintf(stderr, "[LATENCY] nvm_write_ms=%.4f len=%zu\n", _ms, len);
	}
	return CS_STATUS_SUCCESS;
}

#define SLM_STAGE_MAX_DESCS 256

static csStatus
xnvme_slm_stage(struct cs_backend_ctx *ctx, size_t data_bytes)
{
	bool _trace = getenv("LIBCS_TRACE") && *getenv("LIBCS_TRACE") == '1';
	struct timespec _ts0, _ts1;
	if (_trace) clock_gettime(CLOCK_MONOTONIC_RAW, &_ts0);

	uint64_t total_blocks = data_bytes / ctx->lba_nbytes;
	uint32_t max_blks = (32 * 1024 * 1024) / ctx->lba_nbytes;
	if (max_blks > 65535) max_blks = 65535;

	struct xnvme_spec_slm_copy_desc_fmt23 *descs =
		xnvme_buf_alloc(ctx->dev, SLM_STAGE_MAX_DESCS * sizeof(*descs));
	if (!descs) return CS_STATUS_ERROR_NO_MEMORY;

	uint64_t blk = 0;
	uint64_t dst_off = 0;

	while (blk < total_blocks) {
		uint32_t ndesc = 0;
		uint64_t batch_bytes = 0;

		while (ndesc < SLM_STAGE_MAX_DESCS && blk < total_blocks) {
			uint32_t chunk = (total_blocks - blk > max_blks)
			               ? max_blks : (uint32_t)(total_blocks - blk);
			descs[ndesc].snsid = htole32(ctx->nsid);
			descs[ndesc].slba = htole64(blk);
			descs[ndesc].nlb = htole16((uint16_t)(chunk - 1));
			ndesc++;
			batch_bytes += (uint64_t)chunk * ctx->lba_nbytes;
			blk += chunk;
		}

		uint32_t desc_payload = ndesc * (uint32_t)sizeof(*descs);
		struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(ctx->dev);
		cmd.cmd.common.opcode = XNVME_OPC_SLM_MEM_COPY;
		cmd.cmd.common.nsid = ctx->slm_nsid;
		cmd.cmd.common.cdw02 = (uint32_t)batch_bytes;
		cmd.cmd.common.cdw03 = (uint32_t)(batch_bytes >> 32);
		cmd.cmd.common.cdw10 = (uint32_t)dst_off;
		cmd.cmd.common.cdw11 = (uint32_t)(dst_off >> 32);
		cmd.cmd.common.cdw12 = (XNVME_SLM_COPY_DESC_FMT2 << 8) | ((ndesc - 1) & 0xFF);

		int rc = xnvme_cmd_pass(&cmd, descs, desc_payload, NULL, 0);
		if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
			CS_LOG_ERROR("SLM batch stage failed: ndesc=%u dst_off=%" PRIu64
				     " rc=%d sc=%" PRIu16 "", ndesc, dst_off, rc, cmd.cpl.status.sc);
			xnvme_buf_free(ctx->dev, descs);
			return CS_STATUS_ERROR_IO;
		}
		dst_off += batch_bytes;
	}

	xnvme_buf_free(ctx->dev, descs);

	if (_trace) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &_ts1);
		double _ms = (double)(_ts1.tv_sec - _ts0.tv_sec) * 1e3 +
			     (double)(_ts1.tv_nsec - _ts0.tv_nsec) / 1e6;
		fprintf(stderr, "[LATENCY] slm_copy_batch_ms=%.4f bytes=%zu\n", _ms, data_bytes);
	}
	return CS_STATUS_SUCCESS;
}

/**
 * Copy [nvm_slba, +nbytes) of NVM namespace @nvm_nsid into SLM namespace
 * @slm_nsid at byte offset @slm_off with as few SLM Memory Copy commands as
 * possible: up to SLM_STAGE_MAX_DESCS FMT2 ranges (each <= 32 MiB) per
 * command, same wire format as xnvme_slm_stage().
 *
 * The per-32 MiB loop in xnvme_backend_compute_execute_nvm() issued one
 * synchronous command per range, so the target's parallel 1 MiB reads drained
 * to zero between commands: FAIR-1 measured 11.3-12.5 GB/s at 16 MiB (one
 * command) but 9.3-9.7 GB/s at 64 MiB (two).  One command keeps every range's
 * reads in flight together.
 *
 * Returns non-success without side effects the caller has to undo, so the
 * caller can fall back to the per-range loop.
 */
static csStatus
xnvme_nvm_to_slm_copy_batch(struct cs_backend_ctx *ctx, uint32_t nvm_nsid,
			    uint64_t nvm_slba, uint32_t slm_nsid,
			    uint64_t slm_off, size_t nbytes)
{
	uint64_t total_blocks = nbytes / ctx->lba_nbytes;
	uint32_t max_blks = (32 * 1024 * 1024) / ctx->lba_nbytes;
	if (max_blks > 65535) max_blks = 65535;
	if (total_blocks == 0) return CS_STATUS_ERROR_INVALID_PARAM;

	struct xnvme_spec_slm_copy_desc_fmt23 *descs =
		xnvme_buf_alloc(ctx->dev, SLM_STAGE_MAX_DESCS * sizeof(*descs));
	if (!descs) return CS_STATUS_ERROR_NO_MEMORY;

	uint64_t blk = nvm_slba;
	uint64_t end = nvm_slba + total_blocks;
	uint64_t dst_off = slm_off;

	while (blk < end) {
		uint32_t ndesc = 0;
		uint64_t batch_bytes = 0;

		memset(descs, 0, SLM_STAGE_MAX_DESCS * sizeof(*descs));
		while (ndesc < SLM_STAGE_MAX_DESCS && blk < end) {
			uint32_t chunk = (end - blk > max_blks)
				       ? max_blks : (uint32_t)(end - blk);
			descs[ndesc].snsid = htole32(nvm_nsid);
			descs[ndesc].slba = htole64(blk);
			descs[ndesc].nlb = htole16((uint16_t)(chunk - 1));
			ndesc++;
			batch_bytes += (uint64_t)chunk * ctx->lba_nbytes;
			blk += chunk;
		}

		uint32_t desc_payload = ndesc * (uint32_t)sizeof(*descs);
		struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(ctx->dev);
		cmd.cmd.common.opcode = XNVME_OPC_SLM_MEM_COPY;
		cmd.cmd.common.nsid = slm_nsid;
		cmd.cmd.common.cdw02 = (uint32_t)batch_bytes;
		cmd.cmd.common.cdw03 = (uint32_t)(batch_bytes >> 32);
		cmd.cmd.common.cdw10 = (uint32_t)dst_off;
		cmd.cmd.common.cdw11 = (uint32_t)(dst_off >> 32);
		cmd.cmd.common.cdw12 = (XNVME_SLM_COPY_DESC_FMT2 << 8) | ((ndesc - 1) & 0xFF);

		int rc = xnvme_cmd_pass(&cmd, descs, desc_payload, NULL, 0);
		if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
			CS_LOG_WARN("NVM→SLM batch copy failed: ndesc=%u dst_off=%" PRIu64
				    " rc=%d sc=%" PRIu16 "", ndesc, dst_off, rc, cmd.cpl.status.sc);
			xnvme_buf_free(ctx->dev, descs);
			return CS_STATUS_ERROR_IO;
		}
		dst_off += batch_bytes;
	}

	xnvme_buf_free(ctx->dev, descs);
	return CS_STATUS_SUCCESS;
}

/* ---- SLM -> host readback ------------------------------------------------
 *
 * The original readback issued one synchronous 64 KiB SLM Memory Read at a
 * time.  Over NVMe-oF TCP that is one round trip (~80 us) per 64 KiB, i.e.
 * ~0.8 GB/s -- FAIR-1 measured 0.77 GB/s (63 MiB of D2 survivors in 82 ms),
 * which is what made every result-heavy CSD cell (D2 high selectivity, RLE)
 * lose by 5-16x.  The async path below uses chunks up to the controller MDTS
 * (default 1 MiB, LIBCS_SLM_RB_CHUNK) with LIBCS_SLM_RB_QD (default 8) commands
 * in flight on a dedicated queue on the SLM device.  Any failure falls back to
 * the original synchronous 64 KiB loop, so the worst case is the old speed.
 */
#define SLM_RB_SYNC_CHUNK	65536u
#define SLM_RB_DEFAULT_CHUNK	(1u << 20)
#define SLM_RB_DEFAULT_QD	8u
/* Async-conn queue capacity per in-flight QD command. */
#define SLM_RB_CONN_CAP_PER_QD	2u

struct slm_rb_state {
	uint32_t errors;
	uint16_t last_sc;
};

static void
slm_rb_cb(struct xnvme_cmd_ctx *cmd_ctx, void *opaque)
{
	struct slm_rb_state *st = opaque;

	if (xnvme_cmd_ctx_cpl_status(cmd_ctx)) {
		st->errors++;
		st->last_sc = cmd_ctx->cpl.status.sc;
	}
	xnvme_queue_put_cmd_ctx(cmd_ctx->async.queue, cmd_ctx);
}

static void
slm_rb_fill_cmd(struct xnvme_cmd_ctx *cmd_ctx, uint32_t slm_nsid,
		uint64_t off, uint32_t len)
{
	uint32_t *cdw = (uint32_t *)&cmd_ctx->cmd;

	memset(&cmd_ctx->cmd, 0, sizeof(cmd_ctx->cmd));
	cdw[0]  = 0x02;                          /* SLM MEMORY_READ */
	cdw[1]  = slm_nsid;                      /* NSID */
	cdw[10] = (uint32_t)(off & 0xFFFFFFFF);  /* byte offset low */
	cdw[11] = (uint32_t)(off >> 32);         /* byte offset high */
	cdw[12] = len;                           /* length in bytes */
}

static int
slm_rb_sync(struct cs_backend_ctx *ctx, uint64_t src_addr, uint8_t *dma,
	    size_t aligned)
{
	uint64_t off = src_addr;
	size_t remaining = aligned;
	size_t dst_off = 0;

	while (remaining > 0) {
		size_t chunk = remaining > SLM_RB_SYNC_CHUNK ? SLM_RB_SYNC_CHUNK : remaining;
		struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(ctx->slm_dev);

		slm_rb_fill_cmd(&cmd, ctx->slm_nsid, off, (uint32_t)chunk);
		int rc = xnvme_cmd_pass(&cmd, dma + dst_off, chunk, NULL, 0);
		if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
			CS_LOG_ERROR("SLM read failed: addr=%" PRIu64 " len=%zu rc=%d sc=%" PRIu16 "",
				     off, chunk, rc, cmd.cpl.status.sc);
			return -EIO;
		}
		off += chunk;
		dst_off += chunk;
		remaining -= chunk;
	}
	return 0;
}

static uint64_t
slm_rb_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t
slm_rb_qd(void)
{
	uint32_t qd = SLM_RB_DEFAULT_QD;

	(void)parse_u32_env("LIBCS_SLM_RB_QD", &qd);
	return qd > 64 ? 64 : qd;
}

/* Async readback of [src_addr, src_addr + aligned) into @dma over one
 * connection (@dev, *@qp; the queue is created on first use).
 * Returns 0 on success, -EINVAL if async is not applicable (opt-out, chunk
 * below the sync size), -ETIMEDOUT/-EIO on failure (caller may retry with a
 * smaller chunk). */
static int
slm_rb_async_conn(struct xnvme_dev *dev, struct xnvme_queue **qp, uint32_t slm_nsid,
		  uint64_t src_addr, uint8_t *dma, size_t aligned, uint32_t chunk_max,
		  uint32_t *qd_used)
{
	uint32_t qd = slm_rb_qd();

	if (qd == 0) return -EINVAL;                 /* explicit opt-out */
	chunk_max &= ~3u;                            /* dword multiple */
	if (chunk_max <= SLM_RB_SYNC_CHUNK) return -EINVAL;

	if (!*qp) {
		/* capacity must exceed what we keep in flight */
		int ret = xnvme_queue_init(dev, (uint16_t)(qd * SLM_RB_CONN_CAP_PER_QD), 0, qp);
		if (ret) {
			CS_LOG_WARN("SLM readback queue init failed: %d (sync fallback)", ret);
			*qp = NULL;
			return ret;
		}
	}
	struct xnvme_queue *queue = *qp;

	/* Heap, not stack: on a timeout, commands still in flight keep a pointer
	 * to this and may complete later (e.g. aborted at queue teardown). */
	struct slm_rb_state *stp = calloc(1, sizeof(*stp));
	if (!stp) return -ENOMEM;
	struct slm_rb_state *const st_ref = stp;
	uint64_t off = src_addr;
	size_t remaining = aligned;
	size_t dst_off = 0;
	/* A stalled target must not hang the benchmark: give up after this
	 * long and let the caller fall back to the synchronous path. */
	uint32_t timeout_ms = 5000;
	(void)parse_u32_env("LIBCS_SLM_RB_TIMEOUT_MS", &timeout_ms);
	uint64_t deadline = slm_rb_now_ms() + timeout_ms;
	bool timed_out = false;

	while (remaining > 0 && st_ref->errors == 0 && !timed_out) {
		while (xnvme_queue_get_outstanding(queue) >= qd) {
			if (xnvme_queue_poke(queue, 0) < 0 ||
			    slm_rb_now_ms() > deadline) {
				timed_out = true;
				break;
			}
		}
		if (timed_out) break;

		struct xnvme_cmd_ctx *c = xnvme_queue_get_cmd_ctx(queue);
		if (!c) {
			(void)xnvme_queue_poke(queue, 0);
			if (slm_rb_now_ms() > deadline) timed_out = true;
			continue;
		}
		size_t chunk = remaining > chunk_max ? chunk_max : remaining;

		xnvme_cmd_ctx_set_cb(c, slm_rb_cb, stp);
		slm_rb_fill_cmd(c, slm_nsid, off, (uint32_t)chunk);
		int rc = xnvme_cmd_pass(c, dma + dst_off, chunk, NULL, 0);
		if (rc == -EBUSY || rc == -EAGAIN) {
			xnvme_queue_put_cmd_ctx(queue, c);
			(void)xnvme_queue_poke(queue, 0);
			if (slm_rb_now_ms() > deadline) timed_out = true;
			continue;
		}
		if (rc) {
			xnvme_queue_put_cmd_ctx(queue, c);
			stp->errors++;
			break;
		}
		off += chunk;
		dst_off += chunk;
		remaining -= chunk;
	}

	/* Bounded drain: xnvme_queue_drain() would block forever on a stalled
	 * target, so poke until empty or past the deadline. */
	int dr = 0;
	while (xnvme_queue_get_outstanding(queue) > 0) {
		dr = xnvme_queue_poke(queue, 0);
		if (dr < 0) break;
		if (slm_rb_now_ms() > deadline + timeout_ms) {
			timed_out = true;
			break;
		}
	}
	if (stp->errors || dr < 0 || remaining > 0 || timed_out) {
		CS_LOG_WARN("SLM async readback failed: chunk=%u errors=%u sc=%" PRIu16 " "
			    "poke=%d remaining=%zu timed_out=%d", chunk_max,
			    stp->errors, stp->last_sc, dr, remaining, (int)timed_out);
		if (!timed_out || xnvme_queue_get_outstanding(queue) == 0)
			free(stp);
		return timed_out ? -ETIMEDOUT : -EIO;
	}
	free(stp);
	*qd_used = qd;
	return 0;
}

static int
slm_rb_async(struct cs_backend_ctx *ctx, uint64_t src_addr, uint8_t *dma,
	     size_t aligned, uint32_t chunk_max, uint32_t *qd_used)
{
	return slm_rb_async_conn(ctx->slm_dev, &ctx->slm_queue, ctx->slm_nsid,
				 src_addr, dma, aligned, chunk_max, qd_used);
}

/* Default async chunk: LIBCS_SLM_RB_CHUNK (1 MiB) capped by the reported MDTS. */
static uint32_t
slm_rb_default_chunk(struct cs_backend_ctx *ctx)
{
	uint32_t chunk = SLM_RB_DEFAULT_CHUNK;

	(void)parse_u32_env("LIBCS_SLM_RB_CHUNK", &chunk);
	const struct xnvme_geo *geo = xnvme_dev_get_geo(ctx->slm_dev);
	if (geo && geo->mdts_nbytes >= 2 * SLM_RB_SYNC_CHUNK && chunk > geo->mdts_nbytes)
		chunk = geo->mdts_nbytes;
	return chunk;
}

/* ---- parallel SLM readback (LIBCS_SLM_RB_CONNS > 1) -----------------------
 *
 * One NVMe-oF connection is served by one target poll group and one host
 * thread, and TCP processing caps it at ~3.7 GB/s (FAIR-1 d2_full), while the
 * baseline reads over 16 connections.  With LIBCS_SLM_RB_CONNS=N a readback of
 * at least LIBCS_SLM_RB_PAR_MIN bytes (default 4 MiB) is split into N
 * chunk-aligned slices: slice 0 on slm_dev in the calling thread, slices
 * 1..N-1 on N-1 persistent workers that each own a separate connection.
 * Workers open their device and queue in their own thread (as a
 * persistent-worker pool does) and are created once, on the first
 * eligible readback, so no thread is spawned inside a later timed readback.
 * Each slice is read into a per-connection bounce buffer and copied to the
 * caller's buffer by the thread that read it.  Any failure disables the
 * parallel path for this device and the region is re-read on one connection.
 * Default N=1: the single-connection path, unchanged.
 */
#define SLM_RB_MAX_CONNS	16u
#define SLM_RB_PAR_MIN		(4u << 20)

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

static pthread_mutex_t g_slm_rb_open_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Read one slice into this connection's bounce buffer and copy the caller's
 * part out.  A bounce buffer that may still be the target of in-flight DMA
 * (timeout) is abandoned, never freed. */
static int
slm_rb_slice(struct xnvme_dev *dev, struct xnvme_queue **qp, uint32_t slm_nsid,
	     uint8_t **bounce, size_t *bounce_len, uint64_t src, size_t len,
	     uint8_t *dst, size_t copy, uint32_t chunk)
{
	uint32_t qd_used = 0;
	int rc;

	if (*bounce_len < len) {
		if (*bounce) xnvme_buf_free(dev, *bounce);
		*bounce = xnvme_buf_alloc(dev, len);
		*bounce_len = *bounce ? len : 0;
		if (!*bounce) return -ENOMEM;
	}
	rc = slm_rb_async_conn(dev, qp, slm_nsid, src, *bounce, len, chunk, &qd_used);
	if (rc == 0) {
		memcpy(dst, *bounce, copy);
	} else if (*qp && xnvme_queue_get_outstanding(*qp) > 0) {
		*bounce = NULL;
		*bounce_len = 0;
	}
	return rc;
}

static void *
slm_rb_worker_main(void *arg)
{
	struct slm_rb_worker *w = arg;
	struct slm_rb_pool *p = w->pool;
	struct cs_backend_ctx *ctx = p->ctx;
	struct xnvme_opts opts = ctx->slm_opts;
	uint64_t seen = 0;

	pthread_mutex_lock(&g_slm_rb_open_mtx);
	w->dev = xnvme_dev_open(ctx->uri, &opts);
	pthread_mutex_unlock(&g_slm_rb_open_mtx);

	pthread_mutex_lock(&p->mtx);
	if (!w->dev) p->open_failed++;
	p->pending--;
	pthread_cond_broadcast(&p->cv_done);
	while (w->dev) {
		while (!p->stop && p->gen == seen)
			pthread_cond_wait(&p->cv_job, &p->mtx);
		if (p->stop) break;
		seen = p->gen;
		pthread_mutex_unlock(&p->mtx);

		w->rc = w->len == 0 ? 0 :
			slm_rb_slice(w->dev, &w->queue, ctx->slm_nsid, &w->bounce,
				     &w->bounce_len, w->src, w->len, w->dst, w->copy,
				     p->chunk);

		pthread_mutex_lock(&p->mtx);
		p->pending--;
		pthread_cond_broadcast(&p->cv_done);
	}
	pthread_mutex_unlock(&p->mtx);

	if (w->queue) xnvme_queue_term(w->queue);
	if (w->bounce) xnvme_buf_free(w->dev, w->bounce);
	if (w->dev) xnvme_dev_close(w->dev);
	return NULL;
}

static void
slm_rb_pool_destroy(struct slm_rb_pool *p)
{
	if (!p) return;
	pthread_mutex_lock(&p->mtx);
	p->stop = true;
	pthread_cond_broadcast(&p->cv_job);
	pthread_mutex_unlock(&p->mtx);
	for (uint32_t i = 0; i < p->nworkers; i++)
		if (p->w[i].started) pthread_join(p->w[i].tid, NULL);
	pthread_cond_destroy(&p->cv_job);
	pthread_cond_destroy(&p->cv_done);
	pthread_mutex_destroy(&p->mtx);
	free(p->w);
	free(p);
}

/* Start @nworkers workers and wait until each has opened (or failed to open)
 * its connection.  NULL if any connection could not be opened. */
static struct slm_rb_pool *
slm_rb_pool_create(struct cs_backend_ctx *ctx, uint32_t nworkers)
{
	struct slm_rb_pool *p = calloc(1, sizeof(*p));

	if (!p) return NULL;
	p->w = calloc(nworkers, sizeof(*p->w));
	if (!p->w) { free(p); return NULL; }
	p->ctx = ctx;
	p->nworkers = nworkers;
	pthread_mutex_init(&p->mtx, NULL);
	pthread_cond_init(&p->cv_job, NULL);
	pthread_cond_init(&p->cv_done, NULL);

	pthread_mutex_lock(&p->mtx);
	for (uint32_t i = 0; i < nworkers; i++) {
		p->w[i].pool = p;
		if (pthread_create(&p->w[i].tid, NULL, slm_rb_worker_main, &p->w[i]) != 0) {
			p->open_failed++;
			continue;
		}
		p->w[i].started = true;
		p->pending++;
	}
	while (p->pending > 0)
		pthread_cond_wait(&p->cv_done, &p->mtx);
	bool ok = p->open_failed == 0;
	pthread_mutex_unlock(&p->mtx);

	if (!ok) {
		CS_LOG_WARN("SLM parallel readback: %u of %u extra connections failed to open",
			    p->open_failed, nworkers);
		slm_rb_pool_destroy(p);
		return NULL;
	}
	CS_LOG_INFO("SLM parallel readback: %u connections", nworkers + 1);
	return p;
}

/* Grow-only cached DMA buffer for SLM readback; avoids an alloc/free per call. */
static uint8_t *
slm_rb_dma_get(struct cs_backend_ctx *ctx, size_t need)
{
	if (ctx->rb_dma_len >= need)
		return ctx->rb_dma;
	if (ctx->rb_dma)
		xnvme_buf_free(ctx->slm_dev, ctx->rb_dma);
	ctx->rb_dma = xnvme_buf_alloc(ctx->slm_dev, need);
	ctx->rb_dma_len = ctx->rb_dma ? need : 0;
	return ctx->rb_dma;
}

/* Read [src_addr, src_addr + aligned) over @conns connections and copy the
 * first @nbytes to @dst.  Obtains its own slice-0 bounce buffer (ctx-cached,
 * sized once nworkers/slice are known).  0 on success; on failure the caller
 * re-reads the region. */
static int
slm_rb_parallel(struct cs_backend_ctx *ctx, uint64_t src_addr,
		size_t aligned, void *dst, size_t nbytes, uint32_t conns, uint32_t chunk)
{
	struct slm_rb_pool *p;
	uint8_t *dma;
	size_t slice, off, len0;
	int rc0, rc = 0;
	uint32_t i, qd_used = 0;

	if (!ctx->rb_pool) {
		ctx->rb_pool = slm_rb_pool_create(ctx, conns - 1);
		if (!ctx->rb_pool) return -ENODEV;
	}
	p = ctx->rb_pool;
	/* The pool is sized once; slice for the connections it actually has
	 * even if LIBCS_SLM_RB_CONNS changed since, or slices would go unread. */
	conns = p->nworkers + 1;

	/* chunk-aligned slices; the last non-empty one takes the dword-aligned rest */
	slice = (aligned + conns - 1) / conns;
	slice = (slice + chunk - 1) / chunk * chunk;

	/* slice 0 is chunk-aligned and fully overwritten by the read below, so
	 * no zeroing is needed here (unlike the full-@aligned buffer used by
	 * the single-connection paths, whose last few bytes may be padding). */
	len0 = aligned < slice ? aligned : slice;
	dma = slm_rb_dma_get(ctx, len0);
	if (!dma) return -ENOMEM;

	pthread_mutex_lock(&p->mtx);
	p->chunk = chunk;
	for (i = 0; i < p->nworkers; i++) {
		struct slm_rb_worker *w = &p->w[i];

		off = (size_t)(i + 1) * slice;
		w->src = src_addr + off;
		w->len = off < aligned ? (aligned - off < slice ? aligned - off : slice) : 0;
		w->dst = (uint8_t *)dst + off;
		w->copy = off < nbytes ? (nbytes - off < w->len ? nbytes - off : w->len) : 0;
		w->rc = 0;
	}
	p->pending = p->nworkers;
	p->gen++;
	pthread_cond_broadcast(&p->cv_job);
	pthread_mutex_unlock(&p->mtx);

	/* slice 0 on the calling thread's connection */
	rc0 = slm_rb_async_conn(ctx->slm_dev, &ctx->slm_queue, ctx->slm_nsid,
				src_addr, dma, len0, chunk, &qd_used);
	if (rc0 == 0)
		memcpy(dst, dma, nbytes < len0 ? nbytes : len0);

	pthread_mutex_lock(&p->mtx);
	while (p->pending > 0)
		pthread_cond_wait(&p->cv_done, &p->mtx);
	for (i = 0; i < p->nworkers; i++)
		if (p->w[i].rc) rc = p->w[i].rc;
	pthread_mutex_unlock(&p->mtx);

	return rc0 ? rc0 : rc;
}

/**
 * Read a region of the SLM namespace back into a host buffer.
 *
 * SLM Memory Read requires the source address and length to be dword aligned,
 * so the transfer is rounded up into a bounce buffer and only the requested
 * bytes are copied out -- rounding the caller's length directly would overrun
 * their buffer.
 */
static csStatus
xnvme_slm_read_region(struct cs_backend_ctx *ctx, uint64_t src_addr,
		      void *dst, size_t nbytes)
{
	size_t aligned;
	uint8_t *dma = NULL;
	int rc;

	if (!dst || nbytes == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	if ((src_addr & 0x3u) != 0) {
		CS_LOG_ERROR("SLM read: src_addr %" PRIu64 " is not dword aligned", src_addr);
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	aligned = (nbytes + 3u) & ~(size_t)3u;

	bool _rb_trace = getenv("LIBCS_TRACE") && *getenv("LIBCS_TRACE") == '1';
	struct timespec _rb_t0, _rb_t1;
	if (_rb_trace) clock_gettime(CLOCK_MONOTONIC_RAW, &_rb_t0);

	size_t chunk_used = SLM_RB_SYNC_CHUNK;
	uint32_t qd_used = 1;
	uint32_t conns_used = 1;
	const char *mode = "sync";
	bool copied = false;            /* parallel path copies into @dst itself */

	uint32_t conns = 1;
	(void)parse_u32_env("LIBCS_SLM_RB_CONNS", &conns);
	if (conns > SLM_RB_MAX_CONNS) conns = SLM_RB_MAX_CONNS;
	uint32_t par_min = SLM_RB_PAR_MIN;
	(void)parse_u32_env("LIBCS_SLM_RB_PAR_MIN", &par_min);

	rc = -1;
	if (conns > 1 && !ctx->rb_par_disabled && !ctx->slm_rb_async_disabled &&
	    ctx->uri && aligned >= par_min && slm_rb_qd() > 0) {
		uint32_t chunk = ctx->slm_rb_chunk ? ctx->slm_rb_chunk
						   : slm_rb_default_chunk(ctx);
		if ((chunk & ~3u) > SLM_RB_SYNC_CHUNK) {
			chunk &= ~3u;
			rc = slm_rb_parallel(ctx, src_addr, aligned, dst, nbytes,
					     conns, chunk);
			if (rc == 0) {
				mode = "async";
				chunk_used = chunk;
				qd_used = slm_rb_qd();
				conns_used = conns;
				copied = true;
			} else {
				ctx->rb_par_disabled = true;
				CS_LOG_WARN("SLM parallel readback disabled (rc=%d); "
					    "re-reading on one connection", rc);
				if (ctx->slm_queue &&
				    xnvme_queue_get_outstanding(ctx->slm_queue) > 0) {
					/* slice 0 may still DMA into the cached
					 * buffer: orphan it (deliberate leak);
					 * the full-size buffer below is fresh. */
					ctx->rb_dma = NULL;
					ctx->rb_dma_len = 0;
				}
				rc = -1;
			}
		}
	}
	if (!copied) {
		/* Needed by the async-chunked path, the sync path below, and
		 * as the post-parallel-failure re-read buffer. */
		dma = slm_rb_dma_get(ctx, aligned);
		if (!dma) return cs_map_errno(errno);
		if (aligned > nbytes) memset(dma + nbytes, 0, aligned - nbytes);
	}
	if (!copied && !ctx->slm_rb_async_disabled && aligned > SLM_RB_SYNC_CHUNK) {
		/* Start from the last chunk that worked, else the default capped
		 * by the reported MDTS.  The SLM read path may accept less than
		 * the fabric MDTS (the old 64 KiB limit was chosen after large
		 * reads failed), so on failure halve down to 128 KiB before
		 * giving up on async for this device. */
		uint32_t chunk = ctx->slm_rb_chunk;
		if (chunk == 0)
			chunk = slm_rb_default_chunk(ctx);
		while (chunk >= 2 * SLM_RB_SYNC_CHUNK) {
			rc = slm_rb_async(ctx, src_addr, dma, aligned, chunk, &qd_used);
			if (rc == 0 || rc == -EINVAL || rc == -ETIMEDOUT)
				break;
			chunk /= 2;
		}
		if (rc == 0) {
			mode = "async";
			chunk_used = chunk;
			ctx->slm_rb_chunk = chunk;
		} else if (rc != -EINVAL) {
			ctx->slm_rb_async_disabled = true;
			CS_LOG_WARN("SLM async readback disabled (rc=%d); using sync 64 KiB", rc);
			if (rc == -ETIMEDOUT &&
			    xnvme_queue_get_outstanding(ctx->slm_queue) > 0) {
				/* Commands still in flight may DMA into @dma
				 * later: never free it (deliberate leak, once per
				 * device) and read into a fresh buffer. */
				ctx->rb_dma = NULL;
				ctx->rb_dma_len = 0;
				dma = slm_rb_dma_get(ctx, aligned);
				if (!dma) return cs_map_errno(errno);
				if (aligned > nbytes) memset(dma + nbytes, 0, aligned - nbytes);
			}
		}
	}
	if (!copied && rc != 0) {
		chunk_used = SLM_RB_SYNC_CHUNK;
		qd_used = 1;
		if (slm_rb_sync(ctx, src_addr, dma, aligned) != 0) {
			return CS_STATUS_ERROR_IO;
		}
	}

	if (_rb_trace) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &_rb_t1);
		double _ms = (double)(_rb_t1.tv_sec - _rb_t0.tv_sec) * 1e3 +
			     (double)(_rb_t1.tv_nsec - _rb_t0.tv_nsec) / 1e6;
		/* conns > 1 also covers the copy into @dst (done per slice by
		 * the reading thread); with conns=1 that copy follows below. */
		fprintf(stderr,
			"[LATENCY] slm_readback_ms=%.4f bytes=%zu gbps=%.2f mode=%s "
			"chunk=%zu qd=%u conns=%u\n",
			_ms, aligned, _ms > 0 ? (double)aligned / (_ms * 1e6) : 0.0,
			mode, chunk_used, qd_used, conns_used);
	}

	if (!copied)
		memcpy(dst, dma, nbytes);
	return CS_STATUS_SUCCESS;
}

/**
 * Write a host buffer to a region of the SLM namespace.
 *
 * Used to restore SLM data overwritten by a device-side compute output
 * range (the device writes its result to SLM, corrupting any pre-staged
 * data at that offset). Without this restore, a size sweep that runs a
 * smaller test before a larger one silently reads corrupted SLM bytes
 * and returns a wrong aggregate.
 *
 * SLM MEMORY_WRITE (opcode 0x05) accepts payloads up to 64 KiB per command;
 * we chunk to stay within that limit.
 */
static csStatus
xnvme_slm_write_region(struct cs_backend_ctx *ctx, uint64_t dst_addr,
		       const void *src, size_t nbytes)
{
	size_t aligned;
	uint8_t *dma;
	int rc;

	if (!src || nbytes == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	if ((dst_addr & 0x3u) != 0) {
		CS_LOG_ERROR("SLM write: dst_addr %" PRIu64 " is not dword aligned", dst_addr);
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	aligned = (nbytes + 3u) & ~(size_t)3u;
	dma = xnvme_buf_alloc(ctx->slm_dev, aligned);
	if (!dma) return cs_map_errno(errno);
	memset(dma, 0, aligned);
	memcpy(dma, src, nbytes);

	uint64_t off = dst_addr;
	size_t remaining = aligned;
	size_t src_off = 0;
	while (remaining > 0) {
		size_t chunk = remaining > 65536 ? 65536 : remaining;

		struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(ctx->slm_dev);
		uint32_t *cdw = (uint32_t *)&cmd.cmd;
		cdw[0]  = 0x05;                          /* SLM MEMORY_WRITE */
		cdw[1]  = ctx->slm_nsid;                 /* NSID */
		cdw[10] = (uint32_t)(off & 0xFFFFFFFF);  /* byte offset low */
		cdw[11] = (uint32_t)(off >> 32);         /* byte offset high */
		cdw[12] = (uint32_t)chunk;               /* length in bytes */

		rc = xnvme_cmd_pass(&cmd, dma + src_off, chunk, NULL, 0);
		if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
			CS_LOG_ERROR("SLM write failed: addr=%" PRIu64 " len=%zu rc=%d sc=%" PRIu16 "",
				     off, chunk, rc, cmd.cpl.status.sc);
			xnvme_buf_free(ctx->slm_dev, dma);
			return CS_STATUS_ERROR_IO;
		}
		off += chunk;
		src_off += chunk;
		remaining -= chunk;
	}

	xnvme_buf_free(ctx->slm_dev, dma);
	return CS_STATUS_SUCCESS;
}

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

/**
 * Create an MRS over one or more SLM regions.
 *
 * Range i becomes mr_id i+1 on the device -- the target resolves a request's
 * mr_id as resolved_ranges[mr_id - 1] and rejects mr_id 0 (see
 * the device-side MRS range resolution routine).
 * Programs that write their result to an output range (FILTER_AGG,
 * FILTERED_TOPK_EXACT) therefore need at least two ranges.
 */
static csStatus
xnvme_slm_create_mrs_n(struct cs_backend_ctx *ctx,
		       const struct cs_mrs_range *ranges, uint32_t nr,
		       uint16_t *rsid_out)
{
	size_t buflen;
	uint8_t *mrsbuf;
	uint32_t i;

	if (!ranges || nr == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	buflen = (size_t)nr * CS_MRS_DESC_BYTES;
	mrsbuf = xnvme_buf_alloc(ctx->dev, buflen);
	if (!mrsbuf) return cs_map_errno(errno);
	memset(mrsbuf, 0, buflen);

	/* MRS descriptor: [mnsid(4)][length(4)][starting_byte(8)][reserved(16)] */
	for (i = 0; i < nr; i++) {
		uint8_t *d = mrsbuf + ((size_t)i * CS_MRS_DESC_BYTES);
		((uint32_t *)d)[0] = htole32(ctx->slm_nsid);
		((uint32_t *)d)[1] = htole32(ranges[i].length);
		((uint64_t *)(d + 8))[0] = htole64(ranges[i].starting_byte);
	}

	struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(ctx->dev);
	int rc = xnvme_cmd_cp_mem_range(&cmd, ctx->libcs_nsid, 0, nr, mrsbuf, buflen);
	if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
		CS_LOG_ERROR("MRS create failed: nr=%u rc=%d sc=%" PRIu16 " sct=%" PRIu16 "",
			     nr, rc, cmd.cpl.status.sc, cmd.cpl.status.sct);
		xnvme_buf_free(ctx->dev, mrsbuf);
		return CS_STATUS_ERROR_IO;
	}

	*rsid_out = cmd.cpl.cdw0 ? (uint16_t)cmd.cpl.cdw0 : 1;
	xnvme_buf_free(ctx->dev, mrsbuf);
	CS_LOG_INFO("MRS created: rsid=%" PRIu16 " nr=%u slm_nsid=%u",
		    *rsid_out, nr, ctx->slm_nsid);
	return CS_STATUS_SUCCESS;
}

/**
 * Release an MRS created by xnvme_slm_create_mrs_n()/xnvme_slm_create_mrs().
 *
 * The device's MRS pool is finite (max_mrs, see the device's namespace creation); every
 * created rsid that is never deleted permanently occupies a slot, and once
 * the pool fills up every later create fails with sc=0x92
 * (max memory ranges exceeded status code). Logged as a warning, not
 * escalated to the caller's status: by the time this runs the compute
 * result has already been produced (and possibly already read back), so a
 * delete failure should not turn an otherwise-successful call into an error.
 */
static void
xnvme_slm_delete_mrs(struct cs_backend_ctx *ctx, uint16_t rsid)
{
	struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(ctx->dev);
	int rc = xnvme_cmd_cp_mem_range_delete(&cmd, ctx->libcs_nsid, rsid);

	if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
		CS_LOG_WARN("MRS delete failed: rsid=%" PRIu16 " rc=%d sc=%" PRIu16 " sct=%" PRIu16 "",
			    rsid, rc, cmd.cpl.status.sc, cmd.cpl.status.sct);
	}
}

/**
 * Create MRS pointing to a single SLM region starting at byte 0.
 */
static csStatus
xnvme_slm_create_mrs(struct cs_backend_ctx *ctx, uint32_t data_bytes,
		      uint16_t *rsid_out)
{
	struct cs_mrs_range r = { .starting_byte = 0, .length = data_bytes };

	return xnvme_slm_create_mrs_n(ctx, &r, 1, rsid_out);
}

/**
 * Execute CP via MRS path: sends only 24-byte descriptor (or 32-byte for FILTER_GT).
 * Result is in cmd_ctx->cpl.result (cdw0).
 * Works for SUM64, MAX64, MIN64, DOT_PRODUCT, FILTER_GT (SPDK builtins with MRS support).
 *
 * For FILTER_GT, threshold_bits carries the float threshold value (raw bits) and a
 * 32-byte descriptor is sent: [mr_id(8)][offset(8)][length(8)][threshold_bits(4)][pad(4)].
 */
static csStatus
xnvme_slm_execute_desc(struct cs_backend_ctx *ctx, uint16_t prog_index,
		       uint16_t rsid, const void *desc, size_t desc_len,
		       struct xnvme_cmd_ctx *out_cmd)
{
	uint8_t *execbuf;
	int rc;

	if (!desc || desc_len == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	execbuf = xnvme_buf_alloc(ctx->dev, desc_len);
	if (!execbuf) return cs_map_errno(errno);
	memcpy(execbuf, desc, desc_len);

	*out_cmd = xnvme_cmd_ctx_from_dev(ctx->dev);
	rc = xnvme_cmd_cp_execute_buf(out_cmd, ctx->libcs_nsid,
				      prog_index, rsid, execbuf, desc_len);
	xnvme_buf_free(ctx->dev, execbuf);

	if (rc || xnvme_cmd_ctx_cpl_status(out_cmd)) {
		CS_LOG_ERROR("MRS execute (pind=%" PRIu16 " rsid=%" PRIu16 " desc=%zuB) failed: rc=%d sc=%" PRIu16 "",
			     prog_index, rsid, desc_len, rc, out_cmd->cpl.status.sc);
		return rc ? cs_map_errno(-rc) :
			cs_map_nvme_status(out_cmd->cpl.status.sc, out_cmd->cpl.status.sct);
	}
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_slm_execute_mrs(struct cs_backend_ctx *ctx, uint16_t prog_index,
		       uint16_t rsid, uint64_t data_bytes,
		       uint32_t threshold_bits, bool with_threshold,
		       struct xnvme_cmd_ctx *out_cmd)
{
	size_t descbuf_size = with_threshold ? 32 : 24;
	uint8_t descbuf[32];

	memset(descbuf, 0, sizeof(descbuf));
	((uint64_t *)descbuf)[0] = htole64(1);           /* mr_id = 1 (first range) */
	((uint64_t *)descbuf)[1] = htole64(0);            /* offset = 0 */
	((uint64_t *)descbuf)[2] = htole64(data_bytes);   /* length */
	if (with_threshold) {
		((uint32_t *)descbuf)[6] = htole32(threshold_bits);
		((uint32_t *)descbuf)[7] = 0;
	}

	return xnvme_slm_execute_desc(ctx, prog_index, rsid, descbuf, descbuf_size,
				      out_cmd);
}

/**
 * Shared tail for the FILTER_GT survivor-return mode: builds the
 * [filter_gt_desc(32)][output_desc(24)] pair, executes it against the
 * 2-range MRS created by the caller (range 1 = input, range 2 = output at
 * absolute SLM offset @out_base), reads the surviving records back into
 * @output, and reports bytes_produced.  Used by both xnvme_compute_via_mrs()
 * (input staged at SLM offset 0) and xnvme_backend_compute_execute_nvm()
 * (input staged at a caller-chosen offset); only @out_base and @data_size
 * differ between the two.
 *
 * @rsid is deleted before returning, success or failure.
 */
static csStatus
slm_rb_filter_survivors(struct cs_backend_ctx *ctx, uint16_t rsid,
			uint64_t out_base, size_t data_size,
			uint32_t threshold_bits, void *output,
			size_t output_size, volatile int *completion)
{
	uint8_t descbuf[CS_FILTER_GT_DESC_BYTES + CS_PROG_DESC_OUT_BYTES];
	csStatus status;
	bool _sv_trace = getenv("LIBCS_TRACE") && *getenv("LIBCS_TRACE") == '1';
	struct timespec _sv_t0, _sv_t1, _sv_t2;

	memset(descbuf, 0, sizeof(descbuf));
	((uint64_t *)descbuf)[0] = htole64(1);            /* mr_id = 1 */
	((uint64_t *)descbuf)[1] = htole64(0);             /* offset = 0 */
	((uint64_t *)descbuf)[2] = htole64(data_size);     /* length */
	((uint32_t *)descbuf)[6] = htole32(threshold_bits);
	((uint32_t *)descbuf)[7] = 0;
	/* output_desc at offset 32 */
	((uint64_t *)(descbuf + CS_FILTER_GT_DESC_BYTES))[0] = htole64(2);  /* out_mr_id = 2 */
	((uint64_t *)(descbuf + CS_FILTER_GT_DESC_BYTES))[1] = htole64(0);  /* out_off = 0 */
	((uint32_t *)(descbuf + CS_FILTER_GT_DESC_BYTES))[4] = htole32((uint32_t)output_size);

	if (_sv_trace) clock_gettime(CLOCK_MONOTONIC_RAW, &_sv_t0);

	struct xnvme_cmd_ctx cmd_ctx;
	status = xnvme_slm_execute_desc(ctx, LIBCS_PIND_FILTER_GT, rsid,
					descbuf, sizeof(descbuf), &cmd_ctx);
	if (status != CS_STATUS_SUCCESS) {
		xnvme_slm_delete_mrs(ctx, rsid);
		return status;
	}

	if (_sv_trace) clock_gettime(CLOCK_MONOTONIC_RAW, &_sv_t1);

	uint64_t count = (uint64_t)cmd_ctx.cpl.cdw0;
	size_t survivor_bytes = count * sizeof(float);
	if (survivor_bytes > output_size) survivor_bytes = output_size;

	if (survivor_bytes > 0) {
		status = xnvme_slm_read_region(ctx, out_base, output,
					       survivor_bytes);
	}

	/* The readback is the byte volume selectivity controls, so report it
	 * separately -- a D2 row that records d2h_ms=0 here cannot show a
	 * selectivity trend. */
	if (_sv_trace) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &_sv_t2);
		double _ex = (double)(_sv_t1.tv_sec - _sv_t0.tv_sec) * 1e3 +
			     (double)(_sv_t1.tv_nsec - _sv_t0.tv_nsec) / 1e6;
		double _rb = (double)(_sv_t2.tv_sec - _sv_t1.tv_sec) * 1e3 +
			     (double)(_sv_t2.tv_nsec - _sv_t1.tv_nsec) / 1e6;
		fprintf(stderr,
			"[LATENCY] filter_survivor execute_ms=%.4f d2h_ms=%.4f "
			"survivor_bytes=%zu input_bytes=%zu\n",
			_ex, _rb, survivor_bytes, data_size);
	}

	xnvme_slm_delete_mrs(ctx, rsid);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	/* csRunComputeFromNvm() reads this to fill bytes_produced; without it
	 * the caller saw the buffer size (= every element survived). */
	cs_set_last_device_result(survivor_bytes);

	if (completion) {
		*completion = 1;
	}
	return CS_STATUS_SUCCESS;
}

/**
 * Shared tail for the RLE_COMPRESS output-readback mode: builds the
 * 24-byte input + 24-byte output descriptors, executes against the
 * 2-range MRS created by the caller, reads the compressed stream back
 * through the standard readback dispatcher
 * (parallel → async → sync, xnvme_slm_read_region()), and reports the
 * compressed size from cpl.result via *compressed_out.
 *
 * @out_desc_off is the output descriptor's on-wire "offset" field, which
 * differs per path (wire compatibility: passed verbatim, never derived).
 * @data_size must fit the input range; @out_base is the absolute SLM
 * offset the compressed stream is read back from.
 *
 * @rsid is deleted before returning, success or failure.
 */
static csStatus
slm_rb_rle_readback(struct cs_backend_ctx *ctx, uint16_t rsid,
		    size_t data_size, uint64_t out_base,
		    uint64_t out_desc_off, uint32_t out_cap,
		    void *output, size_t output_size,
		    uint64_t *compressed_out)
{
	uint8_t descbuf[CS_PROG_DESC_IN_BYTES + CS_PROG_DESC_OUT_BYTES];
	csStatus status;

	/* 24-byte input desc + 24-byte output desc */
	memset(descbuf, 0, sizeof(descbuf));
	((uint64_t *)descbuf)[0] = htole64(1);          /* in mr_id = 1 */
	((uint64_t *)descbuf)[1] = htole64(0);
	((uint64_t *)descbuf)[2] = htole64(data_size);
	((uint64_t *)(descbuf + CS_PROG_DESC_IN_BYTES))[0] = htole64(2);  /* out mr_id = 2 */
	((uint64_t *)(descbuf + CS_PROG_DESC_IN_BYTES))[1] = htole64(out_desc_off);
	((uint32_t *)(descbuf + CS_PROG_DESC_IN_BYTES))[4] = htole32(out_cap);
	((uint32_t *)(descbuf + CS_PROG_DESC_IN_BYTES))[5] = 0;

	struct xnvme_cmd_ctx cmd_ctx;
	status = xnvme_slm_execute_desc(ctx, LIBCS_PIND_RLE_COMPRESS, rsid,
					descbuf, sizeof(descbuf), &cmd_ctx);
	if (status != CS_STATUS_SUCCESS) {
		xnvme_slm_delete_mrs(ctx, rsid);
		return status;
	}

	uint64_t compressed = (uint64_t)cmd_ctx.cpl.result;
	*compressed_out = compressed;

	/* Read back compressed data from the SLM output range so the CSD
	 * path pays the same output-transfer cost as the CPU path. */
	if (output && output_size > 0 && compressed > 0) {
		size_t read_len = compressed < output_size ? (size_t)compressed : output_size;
		status = xnvme_slm_read_region(ctx, out_base, output, read_len);
	}

	xnvme_slm_delete_mrs(ctx, rsid);
	return status;
}

/*
 * Offsets into the device-side evaluation request header (packed), which every
 * evaluation request begins with. Mirrored here rather than including the SPDK
 * header so libcs keeps building without an SPDK checkout:
 *
 *   0  uint32 version
 *   4  uint16 opcode
 *   6  uint16 flags
 *   8  uint64 output_mr_id
 *  16  uint64 output_offset
 *  24  uint32 output_length
 *  28  uint32 reserved0
 */
#define CS_EVAL_HDR_OPCODE_OFF         4u
#define CS_EVAL_HDR_OUTPUT_MR_ID_OFF   8u
#define CS_EVAL_HDR_OUTPUT_OFFSET_OFF  16u
#define CS_EVAL_HDR_OUTPUT_LENGTH_OFF  24u
#define CS_EVAL_HDR_BYTES              32u

#define CS_EVAL_OP_FILTER_AGG          1u
#define CS_EVAL_OP_FILTERED_TOPK       2u

/*
 * Field offsets within each request body (immediately after the header).
 * FILTER_AGG (64 B total): metadata_mr_id 32, metadata_offset 40,
 *   record_count 48, metadata_stride 56.
 * FILTERED_TOPK_EXACT (100 B total): metadata_mr_id 32, metadata_offset 40,
 *   vector_mr_id 48, vector_offset 56, record_count 64, metadata_stride 72.
 */
#define CS_FILTER_AGG_RECORD_COUNT_OFF     48u
#define CS_FILTER_AGG_METADATA_STRIDE_OFF  56u
#define CS_TOPK_VECTOR_MR_ID_OFF           48u
#define CS_TOPK_VECTOR_OFFSET_OFF          56u
#define CS_TOPK_RECORD_COUNT_OFF           64u
#define CS_TOPK_METADATA_STRIDE_OFF        72u
#define CS_TOPK_REQ_MIN_BYTES              100u

/**
 * Compute path for the evaluation builtins (FILTER_AGG, FILTERED_TOPK_EXACT).
 *
 * These differ from the aggregate builtins in two ways:
 *   - the Execute payload is the caller's request struct (header + trailing
 *     filter clauses), not a fixed [mr_id][offset][length] descriptor;
 *   - the result is written to an output memory range, not returned in cdw0.
 *
 * So the MRS gets two ranges: range 1 (mr_id 1) holds the staged records that
 * the request's metadata_mr_id refers to, and range 2 (mr_id 2) is the output
 * region. libcs owns the SLM layout, so it patches output_mr_id/offset/length
 * into the caller's header rather than making callers guess it.
 */
static csStatus
xnvme_compute_eval_via_mrs(struct cs_backend_ctx *ctx, uint16_t prog_index,
			   const void *input, size_t input_size,
			   void *output, size_t output_size,
			   const void *args, size_t args_size,
			   volatile int *completion)
{
	csStatus status;
	struct cs_mrs_range ranges[3];
	uint32_t range_count;
	uint8_t *desc = NULL;
	uint16_t rsid = 0;
	size_t padded;
	uint64_t output_start;
	uint16_t opcode;
	uint64_t meta_bytes = 0;
	uint16_t output_mr_id;
	struct xnvme_cmd_ctx cmd_ctx;

	if (!args || args_size < CS_EVAL_HDR_BYTES || !output || output_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	opcode = le16toh(*(const uint16_t *)((const uint8_t *)args + CS_EVAL_HDR_OPCODE_OFF));

	/*
	 * FILTERED_TOPK_EXACT scores vectors that live in their own range, so the
	 * caller packs [metadata records][vectors] into one input buffer and the
	 * split is derived from the request: record_count * metadata_stride.
	 */
	if (opcode == CS_EVAL_OP_FILTERED_TOPK) {
		const uint8_t *a = (const uint8_t *)args;
		uint64_t record_count;
		uint32_t stride;

		if (args_size < CS_TOPK_REQ_MIN_BYTES) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		record_count = le64toh(*(const uint64_t *)(a + CS_TOPK_RECORD_COUNT_OFF));
		stride = le32toh(*(const uint32_t *)(a + CS_TOPK_METADATA_STRIDE_OFF));
		meta_bytes = record_count * (uint64_t)stride;
		if (meta_bytes == 0 || meta_bytes > input_size) {
			CS_LOG_ERROR("topk: metadata span %" PRIu64 "B exceeds input %zuB",
				     meta_bytes, input_size);
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
	}

	/* Pad the staged records so the output range starts LBA-aligned. */
	padded = ((input_size + ctx->lba_nbytes - 1) / ctx->lba_nbytes) * ctx->lba_nbytes;
	output_start = padded;

	status = xnvme_slm_write_nvm(ctx, input, input_size);
	if (status != CS_STATUS_SUCCESS) {
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}
	status = xnvme_slm_stage(ctx, padded);
	if (status != CS_STATUS_SUCCESS) {
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	if (opcode == CS_EVAL_OP_FILTERED_TOPK) {
		/* mr 1 = metadata, mr 2 = vectors, mr 3 = output */
		ranges[0].starting_byte = 0;
		ranges[0].length = (uint32_t)meta_bytes;
		ranges[1].starting_byte = meta_bytes;
		ranges[1].length = (uint32_t)(input_size - meta_bytes);
		ranges[2].starting_byte = output_start;
		ranges[2].length = (uint32_t)output_size;
		range_count = 3;
		output_mr_id = 3;
	} else {
		/* mr 1 = metadata, mr 2 = output */
		ranges[0].starting_byte = 0;
		ranges[0].length = (uint32_t)input_size;
		ranges[1].starting_byte = output_start;
		ranges[1].length = (uint32_t)output_size;
		range_count = 2;
		output_mr_id = 2;
	}

	status = xnvme_slm_create_mrs_n(ctx, ranges, range_count, &rsid);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	/* Copy the caller's request and patch in the ranges we just built. */
	desc = malloc(args_size);
	if (!desc) {
		xnvme_slm_delete_mrs(ctx, rsid);
		return CS_STATUS_ERROR_NO_MEMORY;
	}
	memcpy(desc, args, args_size);
	*(uint64_t *)(desc + CS_EVAL_HDR_OUTPUT_MR_ID_OFF)  = htole64(output_mr_id);
	*(uint64_t *)(desc + CS_EVAL_HDR_OUTPUT_OFFSET_OFF) = htole64(0);
	*(uint32_t *)(desc + CS_EVAL_HDR_OUTPUT_LENGTH_OFF) = htole32((uint32_t)output_size);
	if (opcode == CS_EVAL_OP_FILTERED_TOPK) {
		*(uint64_t *)(desc + CS_TOPK_VECTOR_MR_ID_OFF)  = htole64(2);
		*(uint64_t *)(desc + CS_TOPK_VECTOR_OFFSET_OFF) = htole64(0);
	}

	status = xnvme_slm_execute_desc(ctx, prog_index, rsid, desc, args_size, &cmd_ctx);
	free(desc);
	if (status != CS_STATUS_SUCCESS) {
		xnvme_slm_delete_mrs(ctx, rsid);
		return status;
	}

	/* Pull the result back out of the output range. */
	status = xnvme_slm_read_region(ctx, output_start, output, output_size);
	if (status != CS_STATUS_SUCCESS) {
		CS_LOG_ERROR("eval: output range read failed (pind=%" PRIu16 " rsid=%" PRIu16 ")",
			     prog_index, rsid);
		xnvme_slm_delete_mrs(ctx, rsid);
		return status;
	}

	if (completion) {
		*completion = 1;
	}

	CS_LOG_INFO("eval execute succeeded: pind=%" PRIu16 " rsid=%" PRIu16 " records=%zuB out=%zuB",
		    prog_index, rsid, input_size, output_size);
	xnvme_slm_delete_mrs(ctx, rsid);
	return CS_STATUS_SUCCESS;
}

/**
 * Full SLM/MRS compute path: stage data + create MRS + execute.
 * Only for operations that support MRS (SUM64, MAX64, MIN64, DOT_PRODUCT).
 * Returns CS_STATUS_ERROR_NOT_SUPPORTED if SLM is not available.
 */
static csStatus
xnvme_compute_via_mrs(struct cs_backend_ctx *ctx, uint16_t prog_index,
		      const void *input, size_t input_size,
		      void *output, size_t output_size,
		      const void *args, size_t args_size,
		      volatile int *completion)
{
	csStatus status;
	bool with_threshold = false;
	uint32_t threshold_bits = 0;

	if (!ctx->slm_dev) {
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	/*
	 * Evaluation programs (FILTER_AGG / FILTERED_TOPK_EXACT) do not take the
	 * fixed [mr_id][offset][length] descriptor: the caller supplies a whole
	 * request struct in args, and the result comes back through an output
	 * range rather than cdw0. They are handled by their own path.
	 */
	if (prog_index == LIBCS_PIND_FILTER_AGG ||
	    prog_index == LIBCS_PIND_FILTERED_TOPK_EXACT) {
		return xnvme_compute_eval_via_mrs(ctx, prog_index, input, input_size,
						  output, output_size, args, args_size,
						  completion);
	}

	/* SUM64, MAX64, MIN64, DOT_PRODUCT, L2_DISTANCE_SQ, COSINE_SIMILARITY,
	 * FILTER_GT, RLE_COMPRESS, MULTI_AGG64 have SPDK MRS support.
	 * L2/COSINE share DOT_PRODUCT's [A|B] vector layout and MRS descriptor. */
	if (prog_index != LIBCS_PIND_SUM64 && prog_index != LIBCS_PIND_MAX64 &&
	    prog_index != LIBCS_PIND_MIN64 && prog_index != LIBCS_PIND_DOT_PRODUCT &&
	    prog_index != LIBCS_PIND_L2_DISTANCE_SQ &&
	    prog_index != LIBCS_PIND_COSINE_SIMILARITY &&
	    prog_index != LIBCS_PIND_FILTER_GT && prog_index != LIBCS_PIND_RLE_COMPRESS &&
	    prog_index != LIBCS_PIND_MULTI_AGG64) {
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	/* FILTER_GT needs the threshold packed into the descriptor */
	if (prog_index == LIBCS_PIND_FILTER_GT) {
		float threshold = (args && args_size >= sizeof(float)) ?
				  *(const float *)args : 0.0f;
		memcpy(&threshold_bits, &threshold, sizeof(threshold_bits));
		with_threshold = true;
		if (output_size < sizeof(uint64_t)) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
	}

	/* MULTI_AGG64 result is a 32-byte {count,sum,min,max} struct. */
	if (prog_index == LIBCS_PIND_MULTI_AGG64 &&
	    output_size < sizeof(struct libcs_multi_agg64_result)) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	/* Stage: 2-phase (NVM write → device-internal copy) or 1-phase (direct SLM write).
	 * LIBCS_MRS_STAGE_VIA_NVM=1 selects 2-phase staging matching mrs_pind_bench.c;
	 * default (unset/0) preserves the original 1-phase behavior.
	 *
	 * NVM write is cached per-thread by size: the first call writes input to
	 * NVM, subsequent calls with the same size skip the write (data is already
	 * in NVM).  Only SLM copy (NVM → SLM) runs every iteration — that is the
	 * per-iteration staging cost the benchmark should measure. */
	static _Thread_local bool   tl_nvm_written  = false;
	static _Thread_local size_t tl_nvm_wsz      = 0;

	bool stage_via_nvm = false;
	{
		const char *env = getenv("LIBCS_MRS_STAGE_VIA_NVM");
		if (env && *env == '1') stage_via_nvm = true;
	}

	if (stage_via_nvm) {
		size_t padded = ((input_size + ctx->lba_nbytes - 1) /
				 ctx->lba_nbytes) * ctx->lba_nbytes;
		if (!tl_nvm_written || tl_nvm_wsz != input_size) {
			status = xnvme_slm_write_nvm(ctx, input, input_size);
			if (status != CS_STATUS_SUCCESS) {
				CS_LOG_WARN("MRS: NVM write failed, falling back to inline");
				return CS_STATUS_ERROR_NOT_SUPPORTED;
			}
			tl_nvm_written = true;
			tl_nvm_wsz     = input_size;
		}
		status = xnvme_slm_stage(ctx, padded);
		if (status != CS_STATUS_SUCCESS) {
			CS_LOG_WARN("MRS: SLM stage failed, falling back to inline");
			return CS_STATUS_ERROR_NOT_SUPPORTED;
		}
	} else {
		status = xnvme_slm_write_region(ctx, 0, input, input_size);
		if (status != CS_STATUS_SUCCESS) {
			CS_LOG_WARN("MRS: direct SLM write failed, falling back to inline");
			return CS_STATUS_ERROR_NOT_SUPPORTED;
		}
	}

	/* ── FILTER_GT survivor-return mode ──
	 * When the caller passes output_size > sizeof(uint64_t), the device
	 * writes the filtered (surviving) float32 values to a second SLM
	 * range and we read them back.  This exercises the real offload
	 * benefit: low-selectivity queries transfer far fewer bytes over
	 * the fabric than the full input. */
	if (prog_index == LIBCS_PIND_FILTER_GT &&
	    output_size > sizeof(uint64_t)) {
		size_t padded = ((input_size + ctx->lba_nbytes - 1) /
				 ctx->lba_nbytes) * ctx->lba_nbytes;

		/* MRS: range 1 = input, range 2 = output */
		struct cs_mrs_range ranges[2];
		ranges[0].starting_byte = 0;
		ranges[0].length        = (uint32_t)input_size;
		ranges[1].starting_byte = padded;
		ranges[1].length        = (uint32_t)output_size;

		uint16_t rsid = 0;
		status = xnvme_slm_create_mrs_n(ctx, ranges, 2, &rsid);
		if (status != CS_STATUS_SUCCESS) {
			CS_LOG_WARN("MRS: create (survivor) failed");
			return CS_STATUS_ERROR_NOT_SUPPORTED;
		}

		return slm_rb_filter_survivors(ctx, rsid, padded, input_size,
					       threshold_bits, output,
					       output_size, completion);
	}

	if (prog_index == LIBCS_PIND_RLE_COMPRESS) {
		/* RLE_COMPRESS: create 2-range MRS (input + output) and
		 * send output descriptor so the device writes compressed
		 * data to SLM. Then readback so the CSD path pays the same
		 * output-transfer cost as the CPU path. */
		uint64_t out_off = ((input_size + 3) & ~(uint64_t)3);
		uint32_t out_cap = (uint32_t)(output_size ? output_size : input_size * 2);

		struct cs_mrs_range ranges[2];
		ranges[0].starting_byte = 0;
		ranges[0].length = (uint32_t)input_size;
		ranges[1].starting_byte = out_off;
		ranges[1].length = out_cap;

		uint16_t rsid = 0;
		status = xnvme_slm_create_mrs_n(ctx, ranges, 2, &rsid);
		if (status != CS_STATUS_SUCCESS) {
			CS_LOG_WARN("MRS: create (RLE 2-range) failed");
			return CS_STATUS_ERROR_NOT_SUPPORTED;
		}

		uint64_t compressed = 0;
		status = slm_rb_rle_readback(ctx, rsid, input_size, out_off,
					     out_off, out_cap, output,
					     output_size, &compressed);
		if (status != CS_STATUS_SUCCESS) {
			return status;
		}

		cs_set_last_device_result(compressed);
		if (completion) *completion = 1;
		return CS_STATUS_SUCCESS;
	}

	/* Create MRS */
	uint16_t rsid = 0;
	status = xnvme_slm_create_mrs(ctx, (uint32_t)input_size, &rsid);
	if (status != CS_STATUS_SUCCESS) {
		CS_LOG_WARN("MRS: create failed, falling back to inline");
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	/* Execute: 24-byte descriptor (or 32-byte for FILTER_GT) */
	struct xnvme_cmd_ctx cmd_ctx;
	status = xnvme_slm_execute_mrs(ctx, prog_index, rsid, input_size,
				       threshold_bits, with_threshold, &cmd_ctx);
	if (status != CS_STATUS_SUCCESS) {
		xnvme_slm_delete_mrs(ctx, rsid);
		return status;
	}

	/* Extract result from cdw0 */
	if (prog_index == LIBCS_PIND_SUM64 || prog_index == LIBCS_PIND_MAX64 ||
	    prog_index == LIBCS_PIND_MIN64) {
		uint64_t result64 = cmd_ctx.cpl.result;
		if (output_size >= sizeof(uint64_t)) {
			memcpy(output, &result64, sizeof(result64));
		} else {
			memcpy(output, &cmd_ctx.cpl.cdw0, sizeof(uint32_t));
		}
	} else if (prog_index == LIBCS_PIND_DOT_PRODUCT ||
		   prog_index == LIBCS_PIND_L2_DISTANCE_SQ ||
		   prog_index == LIBCS_PIND_COSINE_SIMILARITY) {
		uint32_t result_bits = cmd_ctx.cpl.cdw0;
		memcpy(output, &result_bits, sizeof(result_bits));
	} else if (prog_index == LIBCS_PIND_FILTER_GT) {
		uint64_t count = (uint64_t)cmd_ctx.cpl.cdw0;
		memcpy(output, &count, sizeof(count));
	} else if (prog_index == LIBCS_PIND_MULTI_AGG64) {
		/* Device writes the 32-byte {count,sum,min,max} result into the
		 * SLM input range at byte offset 0 — xnvme_compute_via_mrs always
		 * stages its single MRS range starting at absolute SLM offset 0
		 * (see xnvme_slm_write_region(ctx, 0, ...) / xnvme_slm_stage()
		 * above, both of which start at off=0). */
		struct libcs_multi_agg64_result agg_result;
		status = xnvme_slm_read_region(ctx, 0, &agg_result, sizeof(agg_result));
		if (status != CS_STATUS_SUCCESS) {
			xnvme_slm_delete_mrs(ctx, rsid);
			return status;
		}
		memcpy(output, &agg_result, sizeof(agg_result));
	}

	if (completion) {
		*completion = 1;
	}

	CS_LOG_INFO("MRS execute succeeded: pind=%" PRIu16 " rsid=%" PRIu16 " data=%zu → %zuB sent",
		    prog_index, rsid, input_size, with_threshold ? (size_t)32 : (size_t)24);
	xnvme_slm_delete_mrs(ctx, rsid);
	return CS_STATUS_SUCCESS;
}

/**
 * Async completion callback for compute operations
 */
static void
compute_completion_cb(struct xnvme_cmd_ctx *ctx, void *opaque)
{
	volatile int *completion = (volatile int *)opaque;

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		*completion = -1; /* Error */
	} else {
		*completion = 1;  /* Success */
	}
}

/* LIBCS_PIND_*, ALIGN4, libcs_multi_agg64_result defined above MRS helpers */

static bool
xnvme_buf_is_dma(const struct cs_backend_ctx *ctx, const void *buf)
{
	uint64_t phys = 0;

	if (!ctx || !ctx->dev || !buf) {
		return false;
	}

	return xnvme_buf_vtophys(ctx->dev, (void *)buf, &phys) == 0 && phys != 0;
}

static csStatus
xnvme_backend_compute_execute_nvm(struct cs_backend_ctx *ctx, uint16_t prog_index,
				   uint32_t nvm_nsid, uint64_t nvm_slba,
				   uint32_t slm_nsid, uint64_t slm_byte_off,
				   size_t data_size,
				   void *output, size_t output_size,
				   uint64_t threshold, uint64_t batch,
				   volatile int *completion)
{
	if (!ctx || !ctx->slm_dev) {
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	if (prog_index != LIBCS_PIND_SUM64 && prog_index != LIBCS_PIND_MAX64 &&
	    prog_index != LIBCS_PIND_MIN64 && prog_index != LIBCS_PIND_DOT_PRODUCT &&
	    prog_index != LIBCS_PIND_L2_DISTANCE_SQ &&
	    prog_index != LIBCS_PIND_COSINE_SIMILARITY &&
	    prog_index != LIBCS_PIND_FILTER_GT && prog_index != LIBCS_PIND_RLE_COMPRESS &&
	    prog_index != LIBCS_PIND_MULTI_AGG64) {
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	if (slm_nsid == 0) slm_nsid = ctx->slm_nsid;

	size_t padded = ((data_size + ctx->lba_nbytes - 1) /
			 ctx->lba_nbytes) * ctx->lba_nbytes;

	bool is_batch = (batch > 1) &&
		(prog_index == LIBCS_PIND_DOT_PRODUCT ||
		 prog_index == LIBCS_PIND_L2_DISTANCE_SQ ||
		 prog_index == LIBCS_PIND_COSINE_SIMILARITY);

	if (nvm_nsid != slm_nsid && !is_batch) {
		size_t off = slm_byte_off;
		uint64_t blk = nvm_slba;
		size_t remaining = padded;
		/* LIBCS_TRACE=1: time the device-side NVM->SLM copy.  CSD rows record
		 * h2d_ms=0, so this is the only evidence that the copy is actually
		 * paid inside the timed window (a 16 MiB copy faster than the
		 * drive's link can deliver would mean it is not). */
		bool _st_trace = getenv("LIBCS_TRACE") &&
				 *getenv("LIBCS_TRACE") == '1';
		struct timespec _st_t0, _st_t1;

		if (_st_trace) clock_gettime(CLOCK_MONOTONIC_RAW, &_st_t0);

		/* One SLM Memory Copy with up to 256 ranges (see
		 * xnvme_nvm_to_slm_copy_batch); LIBCS_NVM_STAGE_BATCH=0 or a
		 * failure falls back to the per-32 MiB loop below. */
		const char *_st_mode = "loop";
		uint32_t _st_batch = 1;
		(void)parse_u32_env("LIBCS_NVM_STAGE_BATCH", &_st_batch);
		if (_st_batch &&
		    xnvme_nvm_to_slm_copy_batch(ctx, nvm_nsid, nvm_slba, slm_nsid,
						slm_byte_off, padded) == CS_STATUS_SUCCESS) {
			remaining = 0;
			_st_mode = "batch";
		}

		while (remaining > 0) {
			size_t chunk = remaining > 33554432 ? 33554432 : remaining;
			uint32_t nblocks = (uint32_t)(chunk / ctx->lba_nbytes);
			if (nblocks == 0) nblocks = 1;
			chunk = (size_t)nblocks * ctx->lba_nbytes;

			struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(ctx->slm_dev);
			int rc = xnvme_cmd_slm_copy_lba(&cmd, slm_nsid, off,
							nvm_nsid, blk,
							(uint16_t)(nblocks - 1),
							(uint32_t)chunk,
							XNVME_SLM_COPY_DESC_FMT2);
			if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
				CS_LOG_ERROR("NVM→SLM stage failed: blk=%" PRIu64
					     " rc=%d sc=%" PRIu16 "", blk, rc, cmd.cpl.status.sc);
				return CS_STATUS_ERROR_IO;
			}
			off       += chunk;
			blk       += nblocks;
			remaining -= chunk;
		}

		if (_st_trace) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &_st_t1);
			double _ms = (double)(_st_t1.tv_sec - _st_t0.tv_sec) * 1e3 +
				     (double)(_st_t1.tv_nsec - _st_t0.tv_nsec) / 1e6;
			fprintf(stderr,
				"[LATENCY] nvm_stage_ms=%.4f bytes=%zu gbps=%.2f mode=%s\n",
				_ms, padded, _ms > 0 ? (double)padded / (_ms * 1e6) : 0.0,
				_st_mode);
		}
	}

	if (is_batch) {
		size_t pair_len    = data_size / batch;
		size_t dim_floats  = pair_len / (2 * sizeof(float));
		size_t half_floats = batch * dim_floats;

		uint8_t *nvm_buf = xnvme_buf_alloc(ctx->dev, padded);
		if (!nvm_buf) return CS_STATUS_ERROR_NO_MEMORY;

		uint64_t blk = nvm_slba;
		size_t remaining = padded;
		size_t dst_off = 0;
		while (remaining > 0) {
			size_t chunk = remaining > 33554432 ? 33554432 : remaining;
			uint32_t nblocks = (uint32_t)(chunk / ctx->lba_nbytes);
			if (nblocks == 0) nblocks = 1;
			chunk = (size_t)nblocks * ctx->lba_nbytes;

			struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(ctx->dev);
			int rc = xnvme_nvm_read(&cmd, nvm_nsid, blk,
					       (uint16_t)(nblocks - 1),
					       nvm_buf + dst_off, NULL);
			if (rc || xnvme_cmd_ctx_cpl_status(&cmd)) {
				CS_LOG_ERROR("NVM read failed: blk=%" PRIu64
					     " rc=%d sc=%" PRIu16 "", blk, rc, cmd.cpl.status.sc);
				xnvme_buf_free(ctx->dev, nvm_buf);
				return CS_STATUS_ERROR_IO;
			}
			blk       += nblocks;
			dst_off   += chunk;
			remaining -= chunk;
		}

		uint8_t *reorg = xnvme_buf_alloc(ctx->slm_dev, padded);
		if (!reorg) {
			xnvme_buf_free(ctx->dev, nvm_buf);
			return CS_STATUS_ERROR_NO_MEMORY;
		}
		float *src = (float *)nvm_buf;
		float *dst = (float *)reorg;
		for (uint64_t p = 0; p < batch; p++) {
			memcpy(dst + p * dim_floats,
			       src + p * 2 * dim_floats,
			       dim_floats * sizeof(float));
			memcpy(dst + half_floats + p * dim_floats,
			       src + p * 2 * dim_floats + dim_floats,
			       dim_floats * sizeof(float));
		}

		csStatus s = xnvme_slm_write_region(ctx, slm_byte_off,
						    reorg, data_size);
		xnvme_buf_free(ctx->dev, nvm_buf);
		xnvme_buf_free(ctx->slm_dev, reorg);
		if (s != CS_STATUS_SUCCESS)
			return s;

		struct cs_mrs_range r = {
			.starting_byte = slm_byte_off,
			.length        = (uint32_t)data_size,
		};
		uint16_t rsid = 0;
		s = xnvme_slm_create_mrs_n(ctx, &r, 1, &rsid);
		if (s != CS_STATUS_SUCCESS)
			return s;

		struct xnvme_cmd_ctx cmd_ctx;
		s = xnvme_slm_execute_mrs(ctx, prog_index, rsid, data_size,
					 0, false, &cmd_ctx);
		xnvme_slm_delete_mrs(ctx, rsid);
		if (s != CS_STATUS_SUCCESS)
			return s;

		if (output_size >= sizeof(float)) {
			float result;
			memcpy(&result, &cmd_ctx.cpl.cdw0, sizeof(result));
			memcpy(output, &result, sizeof(result));
		}
		if (completion) *completion = 1;
		return CS_STATUS_SUCCESS;
	}

	/* ── FILTER_GT survivor-return mode ──
	 * Mirrors the survivor-return branch in xnvme_compute_via_mrs(), adapted
	 * for the fact that the input here is already staged at slm_byte_off
	 * (not necessarily SLM offset 0): the output range starts right after
	 * the padded input range, offset by that same base. */
	if (prog_index == LIBCS_PIND_FILTER_GT && output_size > sizeof(uint64_t)) {
		size_t out_off = slm_byte_off + padded;

		struct cs_mrs_range ranges[2];
		ranges[0].starting_byte = slm_byte_off;
		ranges[0].length        = (uint32_t)data_size;
		ranges[1].starting_byte = out_off;
		ranges[1].length        = (uint32_t)output_size;

		uint16_t rsid = 0;
		csStatus status = xnvme_slm_create_mrs_n(ctx, ranges, 2, &rsid);
		if (status != CS_STATUS_SUCCESS) {
			CS_LOG_WARN("NVM-MRS: create (survivor) failed");
			return status;
		}

		uint32_t threshold_bits = (uint32_t)(threshold & 0xFFFFFFFFu);

		return slm_rb_filter_survivors(ctx, rsid, out_off, data_size,
					       threshold_bits, output,
					       output_size, completion);
	}

	/* ── RLE_COMPRESS output-readback mode ──
	 * Only when the caller requests a large enough output buffer
	 * (output_size > one uint64). Small calls (D1/D2/D3 style result-only
	 * requests) take the single-range path below and are unchanged.
	 * Mirrors xnvme_compute_via_mrs()'s RLE readback, offset by slm_byte_off
	 * so the CSD path pays the same output-transfer cost as the CPU path. */
	if (prog_index == LIBCS_PIND_RLE_COMPRESS && output_size > sizeof(uint64_t)) {
		uint64_t out_off = slm_byte_off + padded;
		uint32_t out_cap = (uint32_t)(output_size ? output_size : data_size * 2);

		struct cs_mrs_range ranges[2];
		ranges[0].starting_byte = slm_byte_off;
		ranges[0].length = (uint32_t)data_size;
		ranges[1].starting_byte = out_off;
		ranges[1].length = out_cap;

		uint16_t rsid = 0;
		csStatus status = xnvme_slm_create_mrs_n(ctx, ranges, 2, &rsid);
		if (status != CS_STATUS_SUCCESS) {
			CS_LOG_WARN("NVM-MRS: create (RLE 2-range) failed");
			return status;
		}

		uint64_t compressed = 0;
		status = slm_rb_rle_readback(ctx, rsid, data_size, out_off, 0,
					     out_cap, output, output_size,
					     &compressed);
		if (status != CS_STATUS_SUCCESS) {
			return status;
		}

		/* Overwrite the first 8 bytes of the stream with the compressed
		 * length so callers that parse result=0x... keep working
		 * unchanged; the MRS-side path never had this, so it stays
		 * here rather than in the shared helper. */
		if (output_size >= sizeof(uint64_t)) {
			memcpy(output, &compressed, sizeof(compressed));
		}
		cs_set_last_device_result(compressed);
		if (completion) *completion = 1;
		return CS_STATUS_SUCCESS;
	}

	struct cs_mrs_range r = {
		.starting_byte = slm_byte_off,
		.length = (uint32_t)data_size,
	};
	uint16_t rsid = 0;
	csStatus status = xnvme_slm_create_mrs_n(ctx, &r, 1, &rsid);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	bool with_threshold = false;
	uint32_t threshold_bits = 0;
	if (prog_index == LIBCS_PIND_FILTER_GT) {
		with_threshold = true;
		threshold_bits = (uint32_t)(threshold & 0xFFFFFFFFu);
	}

	struct xnvme_cmd_ctx cmd_ctx;
	status = xnvme_slm_execute_mrs(ctx, prog_index, rsid, data_size,
				       threshold_bits, with_threshold, &cmd_ctx);
	if (status != CS_STATUS_SUCCESS) {
		xnvme_slm_delete_mrs(ctx, rsid);
		return status;
	}

	if (prog_index == LIBCS_PIND_SUM64 || prog_index == LIBCS_PIND_MAX64 ||
	    prog_index == LIBCS_PIND_MIN64) {
		uint64_t result64 = cmd_ctx.cpl.result;
		if (output_size >= sizeof(uint64_t)) {
			memcpy(output, &result64, sizeof(result64));
		} else {
			memcpy(output, &cmd_ctx.cpl.cdw0, sizeof(uint32_t));
		}
	} else if (prog_index == LIBCS_PIND_DOT_PRODUCT ||
		   prog_index == LIBCS_PIND_L2_DISTANCE_SQ ||
		   prog_index == LIBCS_PIND_COSINE_SIMILARITY) {
		uint32_t result_bits = cmd_ctx.cpl.cdw0;
		if (output_size >= sizeof(float)) {
			memcpy(output, &result_bits, sizeof(result_bits));
		}
	} else if (prog_index == LIBCS_PIND_FILTER_GT) {
		uint64_t count = (uint64_t)cmd_ctx.cpl.cdw0;
		if (output_size >= sizeof(uint64_t)) {
			memcpy(output, &count, sizeof(count));
		}
	} else if (prog_index == LIBCS_PIND_MULTI_AGG64) {
		uint64_t sum = cmd_ctx.cpl.result;
		if (output_size >= sizeof(uint64_t)) {
			memcpy(output, &sum, sizeof(sum));
		}
	} else if (prog_index == LIBCS_PIND_RLE_COMPRESS) {
		/* CDW0/CDW1 = compressed output length in bytes (uint64 RLE) */
		uint64_t compressed = cmd_ctx.cpl.result;
		if (output_size >= sizeof(uint64_t)) {
			memcpy(output, &compressed, sizeof(compressed));
		}
	}

	if (completion) {
		*completion = 1;
	}

	xnvme_slm_delete_mrs(ctx, rsid);
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_compute_execute(struct cs_backend_ctx *ctx, uint16_t prog_index,
			      const void *input, size_t input_size,
			      void *output, size_t output_size,
			      const void *args, size_t args_size,
			      volatile int *completion)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	/* Try SLM/MRS path first for supported operations */
	{
		csStatus mrs_status = xnvme_compute_via_mrs(
			ctx, prog_index, input, input_size,
			output, output_size, args, args_size, completion);
		if (mrs_status != CS_STATUS_ERROR_NOT_SUPPORTED) {
			return mrs_status; /* MRS path handled it (success or error) */
		}
		/* Fall through to inline path */
	}

	struct xnvme_cmd_ctx cmd_ctx;

	/* Built-in programs use direct-buffer layouts and run synchronously. */
	if (prog_index == LIBCS_PIND_SUM64 ||
	    prog_index == LIBCS_PIND_MAX64 ||
	    prog_index == LIBCS_PIND_MIN64 ||
	    prog_index == LIBCS_PIND_DOT_PRODUCT ||
	    prog_index == LIBCS_PIND_FILTER_GT ||
	    prog_index == LIBCS_PIND_MEMCPY_INLINE ||
	    prog_index == LIBCS_PIND_RLE_COMPRESS ||
	    prog_index == LIBCS_PIND_MULTI_AGG64 ||
	    prog_index == LIBCS_PIND_L2_DISTANCE_SQ ||
	    prog_index == LIBCS_PIND_COSINE_SIMILARITY) {
		int ret = 0;
		uint8_t *buf = NULL;
		size_t combined_size = 0;

		cmd_ctx = xnvme_cmd_ctx_from_dev(ctx->dev);

		if (!input || input_size == 0 || !output || output_size == 0) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}

		switch (prog_index) {
		case LIBCS_PIND_SUM64:
		case LIBCS_PIND_MAX64:
		case LIBCS_PIND_MIN64: {
			const void *exec_buf = input;
			void *tmp = NULL;

			if (output_size < sizeof(uint32_t) || (input_size % sizeof(uint64_t)) != 0) {
				return CS_STATUS_ERROR_INVALID_PARAM;
			}

			if (!xnvme_buf_is_dma(ctx, input)) {
				tmp = xnvme_buf_alloc(ctx->dev, input_size);
				if (!tmp) {
					return cs_map_errno(errno);
				}
				memcpy(tmp, input, input_size);
				exec_buf = tmp;
			}

			ret = xnvme_cmd_cp_execute_buf(&cmd_ctx, ctx->libcs_nsid, prog_index,
						       0, exec_buf, input_size);
			if (tmp) {
				xnvme_buf_free(ctx->dev, tmp);
			}
			if (ret) {
				CS_LOG_ERROR("xnvme_cmd_cp_execute_buf(reduction) failed: %d", ret);
				return cs_map_errno(-ret);
			}

			/*
			 * Over NVMe-oF fabrics the inline data buffer is not
			 * returned, so the scalar result must come from the
			 * completion queue. cpl.result is the combined cdw0/cdw1.
			 */
			if (output_size >= sizeof(uint64_t)) {
				memcpy(output, &cmd_ctx.cpl.result, sizeof(uint64_t));
			} else {
				memcpy(output, &cmd_ctx.cpl.cdw0, sizeof(uint32_t));
			}
			break;
		}
		case LIBCS_PIND_DOT_PRODUCT:
		case LIBCS_PIND_L2_DISTANCE_SQ:
		case LIBCS_PIND_COSINE_SIMILARITY: {
			const void *exec_buf = input;
			void *tmp = NULL;

			if (output_size < sizeof(uint32_t)) {
				return CS_STATUS_ERROR_LIMIT;
			}
			if ((input_size % (2 * sizeof(float))) != 0) {
				return CS_STATUS_ERROR_INVALID_PARAM;
			}

			if (!xnvme_buf_is_dma(ctx, input)) {
				tmp = xnvme_buf_alloc(ctx->dev, input_size);
				if (!tmp) {
					return cs_map_errno(errno);
				}
				memcpy(tmp, input, input_size);
				exec_buf = tmp;
			}

			ret = xnvme_cmd_cp_execute_buf(&cmd_ctx, ctx->libcs_nsid, prog_index,
						       0, exec_buf, input_size);
			if (ret) {
				if (tmp) {
					xnvme_buf_free(ctx->dev, tmp);
				}
				CS_LOG_ERROR("xnvme_cmd_cp_execute_buf(vector) failed: %d", ret);
				return cs_map_errno(-ret);
			}

			/*
			 * Vector programs return the scalar result in cdw0
			 * (float bits). Try cdw0 first; if the controller
			 * populated the inline buffer (PCIe path), prefer
			 * that copy.
			 */
			if (tmp && output_size >= sizeof(float)) {
				memcpy(output, tmp, sizeof(float));
			} else {
				uint32_t result_bits = cmd_ctx.cpl.cdw0;
				memcpy(output, &result_bits, sizeof(result_bits));
			}
			if (tmp) {
				xnvme_buf_free(ctx->dev, tmp);
			}
			break;
		}
		case LIBCS_PIND_FILTER_GT: {
			/*
			 * SPDK builtin_runtime expects:
			 *   [threshold 4B][input N floats][output space N floats]
			 *
			 * Strategy: send data to SPDK, then re-filter locally
			 * from the DMA buffer to guarantee correct count.
			 */
			float threshold = (args && args_size >= sizeof(float)) ?
					  *(const float *)args : 0.0f;
			size_t header = sizeof(float);
			size_t n_floats = input_size / sizeof(float);
			size_t out_region = n_floats * sizeof(float);
			size_t out_offset = header + input_size;

			combined_size = ALIGN4(header + input_size + out_region);
			buf = xnvme_buf_alloc(ctx->dev, combined_size);
			if (!buf) {
				return cs_map_errno(errno);
			}
			memset(buf, 0, combined_size);
			memcpy(buf, &threshold, sizeof(threshold));
			memcpy(buf + header, input, input_size);

			ret = xnvme_cmd_cp_execute_buf(&cmd_ctx, ctx->libcs_nsid, prog_index,
						       0, buf, combined_size);
			if (ret) {
				xnvme_buf_free(ctx->dev, buf);
				CS_LOG_ERROR("xnvme_cmd_cp_execute_buf(filter) failed: %d", ret);
				return cs_map_errno(-ret);
			}

			/*
			 * Re-filter locally from the INPUT portion of the
			 * combined buffer when the transport can't return
			 * the device's output region (see cp_output_returns /
			 * xnvme_uri_is_local_direct()): over NVMe-oF the
			 * controller writes filtered values into the output
			 * region of its own copy, but that region is NOT
			 * returned to the host (inline data is
			 * host→controller only). We can still reproduce the
			 * exact output from the input we still have in the
			 * combined buffer.
			 */
			{
				const float *spdk_out = (const float *)(buf + out_offset);
				const float *src_input = (const float *)(buf + header);
				float *dst = (float *)output;
				size_t max_out = output_size / sizeof(float);
				size_t written = 0;

				if (ctx->cp_output_returns) {
					/* PCIe / shared-mem path: use SPDK output */
					for (size_t fi = 0; fi < n_floats && written < max_out; fi++) {
						if (spdk_out[fi] > threshold) {
							dst[written++] = spdk_out[fi];
						}
					}
				} else {
					/* Fabrics path: re-filter from input */
					for (size_t fi = 0; fi < n_floats && written < max_out; fi++) {
						if (src_input[fi] > threshold) {
							dst[written++] = src_input[fi];
						}
					}
				}

				/* NaN-fill remaining slots */
				for (size_t fi = written; fi < max_out; fi++) {
					dst[fi] = NAN;
				}

			}

			xnvme_buf_free(ctx->dev, buf);
			break;
		}
		case LIBCS_PIND_MULTI_AGG64: {
			size_t out_offset = input_size;
			size_t out_bytes = sizeof(struct libcs_multi_agg64_result);

			if (!input || input_size == 0 || !output || output_size < out_bytes ||
			    (input_size % sizeof(double)) != 0) {
				return CS_STATUS_ERROR_INVALID_PARAM;
			}

			combined_size = ALIGN4(input_size + out_bytes);
			buf = xnvme_buf_alloc(ctx->dev, combined_size);
			if (!buf) {
				return cs_map_errno(errno);
			}
			memset(buf, 0, combined_size);
			memcpy(buf, input, input_size);

			ret = xnvme_cmd_cp_execute_buf(&cmd_ctx, ctx->libcs_nsid, prog_index,
						       0, buf, combined_size);
			if (ret) {
				xnvme_buf_free(ctx->dev, buf);
				CS_LOG_ERROR("xnvme_cmd_cp_execute_buf(multi_agg64) failed: %d", ret);
				return cs_map_errno(-ret);
			}

			if ((size_t)cmd_ctx.cpl.cdw0 < out_bytes) {
				out_bytes = (size_t)cmd_ctx.cpl.cdw0;
			}

			/*
			 * Use the SPDK-written output region only when the
			 * transport actually returns it (cp_output_returns).
			 * Over NVMe-oF TCP the modified buffer is not
			 * returned, so the output region stays zeroed;
			 * compute the aggregation locally from the input we
			 * still have instead.
			 */
			{
				const uint8_t *spdk_result = buf + out_offset;

				if (ctx->cp_output_returns) {
					memcpy(output, spdk_result, out_bytes);
				} else {
					/* Fabrics path: compute locally */
					const double *vals = (const double *)input;
					size_t count = input_size / sizeof(double);
					struct libcs_multi_agg64_result local = {
						.count = count,
						.sum = 0.0,
						.min = vals[0],
						.max = vals[0],
					};
					for (size_t vi = 0; vi < count; vi++) {
						local.sum += vals[vi];
						if (vals[vi] < local.min) local.min = vals[vi];
						if (vals[vi] > local.max) local.max = vals[vi];
					}
					memcpy(output, &local, sizeof(local));
				}
			}
			xnvme_buf_free(ctx->dev, buf);
			break;
		}
		case LIBCS_PIND_MEMCPY_INLINE: {
			combined_size = ALIGN4(input_size + output_size);
			buf = xnvme_buf_alloc(ctx->dev, combined_size);
			if (!buf) {
				return cs_map_errno(errno);
			}
			memset(buf, 0, combined_size);
			memcpy(buf, input, input_size);

			ret = xnvme_cmd_cp_execute_buf(&cmd_ctx, ctx->libcs_nsid, prog_index,
						       0, buf, combined_size);
			if (ret) {
				xnvme_buf_free(ctx->dev, buf);
				CS_LOG_ERROR("xnvme_cmd_cp_execute_buf(memcpy_inline) failed: %d", ret);
				return cs_map_errno(-ret);
			}

			size_t copied = (size_t)cmd_ctx.cpl.cdw0;
			if (copied > output_size) {
				copied = output_size;
			}

			/*
			 * Over NVMe-oF TCP the output half is not returned
			 * (cp_output_returns == false). MEMCPY_INLINE just
			 * copies input→output, so we can reproduce it from
			 * the input we still have.
			 */
			{
				const uint8_t *out_region = buf + input_size;

				if (ctx->cp_output_returns) {
					memcpy(output, out_region, copied);
				} else {
					size_t to_copy = copied < input_size ? copied : input_size;
					memcpy(output, input, to_copy);
				}
			}
			xnvme_buf_free(ctx->dev, buf);
			break;
		}
		case LIBCS_PIND_RLE_COMPRESS: {
			uint64_t n = input_size;
			size_t header = sizeof(uint64_t);
			size_t out_offset = header + input_size;

			combined_size = ALIGN4(header + input_size + output_size);
			buf = xnvme_buf_alloc(ctx->dev, combined_size);
			if (!buf) {
				return cs_map_errno(errno);
			}
			memset(buf, 0, combined_size);
			memcpy(buf, &n, sizeof(n));
			memcpy(buf + header, input, input_size);

			ret = xnvme_cmd_cp_execute_buf(&cmd_ctx, ctx->libcs_nsid, prog_index,
						       0, buf, combined_size);
			if (ret) {
				xnvme_buf_free(ctx->dev, buf);
				CS_LOG_ERROR("xnvme_cmd_cp_execute_buf(rle) failed: %d", ret);
				return cs_map_errno(-ret);
			}

			size_t compressed_size = (size_t)cmd_ctx.cpl.cdw0;
			if (compressed_size > output_size) {
				xnvme_buf_free(ctx->dev, buf);
				CS_LOG_ERROR("rle: compressed size %zuB exceeds output buffer %zuB",
					     compressed_size, output_size);
				return CS_STATUS_ERROR_LIMIT;
			}
			{
				/*
				 * Over NVMe-oF TCP the output region is not
				 * returned (cp_output_returns == false).
				 * Fall back to local RLE encoding.
				 */
				const uint8_t *out_region = buf + out_offset;

				if (ctx->cp_output_returns) {
					memcpy(output, out_region, compressed_size);
				} else {
					/* Fabrics: RLE-encode locally */
					const uint8_t *src = (const uint8_t *)input;
					uint8_t *dst = (uint8_t *)output;
					size_t in_len = input_size;
					size_t out_cap = output_size;
					size_t opos = 0, ipos = 0;
					while (ipos < in_len && opos + 1 < out_cap) {
						uint8_t value = src[ipos];
						uint8_t run = 1;
						while (ipos + run < in_len &&
						       src[ipos + run] == value &&
						       run < 255) {
							run++;
						}
						dst[opos++] = run;
						dst[opos++] = value;
						ipos += run;
					}
					compressed_size = opos;
				}
			}
			xnvme_buf_free(ctx->dev, buf);
			cs_set_last_device_result((uint64_t)compressed_size);
			break;
		}
		default:
			return CS_STATUS_ERROR_NOT_SUPPORTED;
		}

		if (xnvme_cmd_ctx_cpl_status(&cmd_ctx)) {
			uint16_t sc = cmd_ctx.cpl.status.sc;
			uint16_t sct = cmd_ctx.cpl.status.sct;
			CS_LOG_ERROR("CP execute failed: prog_index=%" PRIu16 " libcs_nsid=%u sc=0x%" PRIx16 " sct=0x%" PRIx16 " cdw0=0x%x",
				     prog_index, ctx->libcs_nsid, sc, sct, cmd_ctx.cpl.cdw0);
			return cs_map_nvme_status(sc, sct);
		}

		if (prog_index == LIBCS_PIND_SUM64 || prog_index == LIBCS_PIND_MAX64 || prog_index == LIBCS_PIND_MIN64) {
			uint64_t result64 = cmd_ctx.cpl.result;
			if (output_size >= sizeof(uint64_t)) {
				memcpy(output, &result64, sizeof(result64));
			} else {
				memcpy(output, &cmd_ctx.cpl.cdw0, sizeof(uint32_t));
			}
		}

		if (prog_index == LIBCS_PIND_DOT_PRODUCT ||
		    prog_index == LIBCS_PIND_L2_DISTANCE_SQ ||
		    prog_index == LIBCS_PIND_COSINE_SIMILARITY) {
			uint32_t result_bits = cmd_ctx.cpl.cdw0;
			memcpy(output, &result_bits, sizeof(result_bits));
		}

		if (completion) {
			*completion = 1;
		}

		return CS_STATUS_SUCCESS;
	}

	/* Async path if completion pointer provided and queue available */
	if (completion && ctx->queue) {
		struct xnvme_cmd_ctx *async_ctx = xnvme_queue_get_cmd_ctx(ctx->queue);
		if (!async_ctx) {
			CS_LOG_ERROR("Failed to get async command context");
			return CS_STATUS_ERROR_BUSY;
		}

		/* Set completion callback */
		xnvme_cmd_ctx_set_cb(async_ctx, compute_completion_cb, (void *)completion);

		/* MRS ID = 0 for default memory range set */
		int ret = xnvme_cmd_cp_execute_buf(async_ctx, ctx->libcs_nsid, prog_index,
						   0, input, input_size);
		if (ret) {
			xnvme_queue_put_cmd_ctx(ctx->queue, async_ctx);
			CS_LOG_ERROR("xnvme_cmd_cp_execute_buf (async) failed: %d", ret);
			return cs_map_errno(-ret);
		}

		return CS_STATUS_PENDING;
	}

	/* Sync path */
	cmd_ctx = xnvme_cmd_ctx_from_dev(ctx->dev);

	int ret = xnvme_cmd_cp_execute_buf(&cmd_ctx, ctx->libcs_nsid, prog_index,
					   0, input, input_size);
	if (ret) {
		CS_LOG_ERROR("xnvme_cmd_cp_execute_buf failed: %d", ret);
		return cs_map_errno(-ret);
	}

	if (xnvme_cmd_ctx_cpl_status(&cmd_ctx)) {
		uint16_t sc = cmd_ctx.cpl.status.sc;
		uint16_t sct = cmd_ctx.cpl.status.sct;
		CS_LOG_ERROR("CP execute failed: sc=0x%" PRIx16 " sct=0x%" PRIx16 "", sc, sct);
		return cs_map_nvme_status(sc, sct);
	}

	/* Set completion flag for sync completion */
	if (completion) {
		*completion = 1;
	}

	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_storage_io(struct cs_backend_ctx *ctx, csStorageOp op,
			 uint64_t lba, uint32_t blocks,
			 void *buffer, size_t buffer_size,
			 volatile int *completion)
{
	if (!ctx || !buffer || blocks == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (blocks > (uint32_t)UINT16_MAX + 1) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	const struct xnvme_geo *geo = xnvme_dev_get_geo(ctx->dev);
	uint32_t lba_nbytes = geo ? geo->lba_nbytes : 512;
	size_t required = (size_t)blocks * lba_nbytes;
	if (buffer_size < required) {
		return CS_STATUS_ERROR_LIMIT;
	}

	uint16_t nlb = (uint16_t)(blocks - 1);

	if (completion && ctx->queue) {
		struct xnvme_cmd_ctx *async_ctx = xnvme_queue_get_cmd_ctx(ctx->queue);
		if (!async_ctx) {
			CS_LOG_ERROR("Failed to get async command context");
			return CS_STATUS_ERROR_BUSY;
		}

		xnvme_cmd_ctx_set_cb(async_ctx, compute_completion_cb, (void *)completion);

		int ret = 0;
		if (op == CS_STORAGE_READ) {
			ret = xnvme_nvm_read(async_ctx, ctx->nsid, lba, nlb, buffer, NULL);
		} else if (op == CS_STORAGE_WRITE) {
			ret = xnvme_nvm_write(async_ctx, ctx->nsid, lba, nlb, buffer, NULL);
		} else {
			xnvme_queue_put_cmd_ctx(ctx->queue, async_ctx);
			return CS_STATUS_ERROR_INVALID_PARAM;
		}

		if (ret) {
			xnvme_queue_put_cmd_ctx(ctx->queue, async_ctx);
			CS_LOG_ERROR("xnvme_nvm_%s (async) failed: %d",
				     op == CS_STORAGE_READ ? "read" : "write", ret);
			return cs_map_errno(-ret);
		}

		return CS_STATUS_PENDING;
	}

	struct xnvme_cmd_ctx cmd_ctx = xnvme_cmd_ctx_from_dev(ctx->dev);
	int ret = 0;
	if (op == CS_STORAGE_READ) {
		ret = xnvme_nvm_read(&cmd_ctx, ctx->nsid, lba, nlb, buffer, NULL);
	} else if (op == CS_STORAGE_WRITE) {
		ret = xnvme_nvm_write(&cmd_ctx, ctx->nsid, lba, nlb, buffer, NULL);
	} else {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (ret) {
		CS_LOG_ERROR("xnvme_nvm_%s failed: %d",
			     op == CS_STORAGE_READ ? "read" : "write", ret);
		return cs_map_errno(-ret);
	}

	if (xnvme_cmd_ctx_cpl_status(&cmd_ctx)) {
		uint16_t sc = cmd_ctx.cpl.status.sc;
		uint16_t sct = cmd_ctx.cpl.status.sct;
		CS_LOG_ERROR("NVM %s failed: sc=0x%" PRIx16 " sct=0x%" PRIx16 "",
			     op == CS_STORAGE_READ ? "read" : "write", sc, sct);
		return cs_map_nvme_status(sc, sct);
	}

	if (completion) {
		*completion = 1;
	}

	return CS_STATUS_SUCCESS;
}

static int
xnvme_backend_poll(struct cs_backend_ctx *ctx, uint32_t max)
{
	if (!ctx || !ctx->queue) {
		return 0;
	}

	int ret = xnvme_queue_poke(ctx->queue, max);
	if (ret < 0) {
		CS_LOG_WARN("xnvme_queue_poke failed: %d", ret);
		return ret;
	}

	return ret;
}

static csStatus
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

static csStatus
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

static csStatus
xnvme_backend_mem_init(struct cs_backend_ctx *ctx, void *ptr,
		       uint8_t fill_value, size_t size)
{
	(void)ctx;
	if (!ptr) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	memset(ptr, fill_value, size);
	return CS_STATUS_SUCCESS;
}

static csStatus
xnvme_backend_compute_submit(struct cs_backend_ctx *ctx, uint16_t prog_index,
			     const void *input, size_t input_size,
			     void *output, size_t output_size,
			     const void *args, size_t args_size,
			     volatile int *completion)
{
	if (!ctx || !completion) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	/* Always use compute_execute which handles MRS path and result writing.
	 * The compute happens synchronously, but we return PENDING so the
	 * event/callback machinery works correctly. compute_reap will see
	 * the completion flag already set. */
	csStatus status = xnvme_backend_compute_execute(ctx, prog_index,
							input, input_size,
							output, output_size,
							args, args_size, completion);
	if (status == CS_STATUS_SUCCESS && completion) {
		return CS_STATUS_PENDING;
	}
	return status;
}

static csStatus
xnvme_backend_compute_reap(struct cs_backend_ctx *ctx, volatile int *completion)
{
	if (!ctx || !completion) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (ctx->queue) {
		int ret = xnvme_queue_poke(ctx->queue, 0);
		if (ret < 0) {
			CS_LOG_WARN("xnvme_queue_poke (reap) failed: %d", ret);
			return cs_map_errno(-ret);
		}
	}

	if (*completion == 0) {
		return CS_STATUS_PENDING;
	}

	return (*completion > 0) ? CS_STATUS_SUCCESS : CS_STATUS_ERROR_IO;
}

static csStatus
xnvme_backend_abort_request(struct cs_backend_ctx *ctx, volatile int *completion)
{
	(void)ctx;
	if (!completion) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	if (*completion == 0) {
		*completion = -1;
		return CS_STATUS_SUCCESS;
	}
	return CS_STATUS_ERROR_BUSY;
}

static csStatus
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

static csStatus
xnvme_backend_query_device_statistics(struct cs_backend_ctx *ctx,
				     csDeviceStatistics *stats)
{
	if (!ctx || !stats) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	memset(stats, 0, sizeof(*stats));
	return CS_STATUS_ERROR_NOT_SUPPORTED;
}

static csStatus
xnvme_backend_device_config(struct cs_backend_ctx *ctx, uint32_t config_id,
			   const void *data, size_t size)
{
	if (!ctx || !data || size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	(void)config_id;
	return CS_STATUS_ERROR_NOT_SUPPORTED;
}

static csStatus
xnvme_backend_device_reset(struct cs_backend_ctx *ctx, uint32_t reset_type)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	(void)reset_type;
	return CS_STATUS_ERROR_NOT_SUPPORTED;
}

static csStatus
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
