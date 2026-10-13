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

#include "cs_internal.h"
#include "cs_programs.h"

/* xNVMe headers */
#include <libxnvme.h>
#include <libxnvme_nvm.h>
#include <libxnvme_slm.h>
#include <libxnvme_cs.h>

#include "backend_xnvme_internal.h"

/**
 * xNVMe backend state
 */
static struct {
	bool initialized;  /* backend init flag: written by xnvme_backend_init/shutdown
	                   * (under the library init_lock), read as re-entry guard */
} xnvme_state = { .initialized = false };

bool
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

csStatus
xnvme_backend_init(void)
{
	if (xnvme_state.initialized) {
		return CS_STATUS_SUCCESS;
	}

	CS_LOG_INFO("xNVMe backend initializing");
	xnvme_state.initialized = true;
	return CS_STATUS_SUCCESS;
}

csStatus
xnvme_backend_shutdown(void)
{
	if (!xnvme_state.initialized) {
		return CS_STATUS_SUCCESS;
	}

	CS_LOG_INFO("xNVMe backend shutting down");
	xnvme_state.initialized = false;
	return CS_STATUS_SUCCESS;
}

csStatus
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

csStatus
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
	 * LIBCS_XNVME_NSID = compute namespace (for compute commands, e.g. 2).
	 * LIBCS_XNVME_NVM_NSID = NVM namespace to open the device with (default 1).
	 *
	 * xnvme_dev_open() must be called with a standard NVM namespace because
	 * Compute namespaces don't support Identify Namespace and cause ENXIO.
	 * The compute nsid is stored separately and used only in CP execute calls.
	 */
	uint32_t libcs_nsid = 2; /* default compute namespace */
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

csStatus
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

csStatus
xnvme_backend_dev_get_info(struct cs_backend_ctx *ctx, csDeviceInfo *info)
{
	if (!ctx || !info) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	memset(info, 0, sizeof(*info));

	const struct xnvme_ident *ident = xnvme_dev_get_ident(ctx->dev);
	if (ident) {
		snprintf(info->uri, sizeof(info->uri), "%s", ident->uri);
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

csStatus
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

csStatus
xnvme_backend_mem_free(struct cs_backend_ctx *ctx, void *ptr)
{
	if (!ctx || !ptr) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	xnvme_buf_free(ctx->dev, ptr);
	return CS_STATUS_SUCCESS;
}

csStatus
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

csStatus
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

csStatus
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
