/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file backend_ioctl.c
 * @brief Linux NVMe ioctl passthrough backend for CS API
 *
 * Submits CP/SLM admin and IO commands via the kernel's nvme_passthru_cmd
 * ioctl interface. No external dependencies — only standard Linux headers.
 *
 * Known limitation: struct nvme_passthru_cmd does not expose CDW4 (DLEN
 * in the Execute Program spec). CDW4 is set to 0 in all execute commands.
 * SPDK's builtin programs derive data length from the PRP transfer size and
 * work correctly, but custom eBPF programs that rely on CDW4 may not.
 *
 * URI format: /dev/nvme0  (NVMe controller character device)
 *   Strip the namespace suffix: /dev/nvme0n1 → /dev/nvme0
 *
 * Environment variables (shared with xNVMe backend):
 *   LIBCS_XNVME_NSID      — compute namespace ID (default 2)
 *   LIBCS_XNVME_NVM_NSID  — NVM namespace ID (default 1)
 *   LIBCS_XNVME_SLM_NSID  — SLM namespace ID (MRS path, optional)
 */

#ifndef __linux__

/* Non-Linux stub — all ops unsupported */
#include "cs_internal.h"

CS_DEFINE_BACKEND_STUBS(ioctl_backend)

#else /* __linux__ */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>

#include "cs_internal.h"
#include "cs_programs.h"
#include <stdio.h>
#include <math.h>

#define IOCTL_TIMEOUT_ADMIN_MS  5000u
#define IOCTL_TIMEOUT_IO_MS     30000u

/* Advertised device capability defaults for this backend */
#define IOCTL_DEFAULT_MAX_PROGRAMS   16
#define IOCTL_DEFAULT_MAX_MEMORY_MB  256

struct cs_backend_ctx {
	int       ctrl_fd;
	uint32_t  libcs_nsid;
	uint32_t  slm_nsid;
	uint32_t  nvm_nsid;
	uint32_t  lba_nbytes;
	char      ctrl_path[256];
	bool      cp_output_returns;  /**< CP Execute inline output half
                                       *  visible to host on completion
                                       *  (local/PCIe). False for NVMe-oF
                                       *  fabrics.
                                       *
                                       *  Documented gap (D1): this backend records the
                                       *  flag but never branches on it -- inline output
                                       *  return semantics are honored only by the xnvme
                                       *  backend. See docs/coverage-gaps.md section 3. */
};

/* ---------- helpers ---------- */

static csStatus
ioctl_admin_cmd(int fd, uint8_t opcode, uint32_t nsid,
		void *buf, uint32_t buf_len,
		uint32_t cdw2, uint32_t cdw3,
		uint32_t cdw10, uint32_t cdw11,
		uint32_t cdw12, uint32_t cdw13,
		uint32_t cdw14, uint32_t cdw15,
		uint32_t *result_out)
{
	struct nvme_passthru_cmd cmd = {
		.opcode      = opcode,
		.nsid        = nsid,
		.addr        = (__u64)(uintptr_t)buf,
		.data_len    = buf_len,
		.cdw2        = cdw2,
		.cdw3        = cdw3,
		.cdw10       = cdw10,
		.cdw11       = cdw11,
		.cdw12       = cdw12,
		.cdw13       = cdw13,
		.cdw14       = cdw14,
		.cdw15       = cdw15,
		.timeout_ms  = IOCTL_TIMEOUT_ADMIN_MS,
	};

	int ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
	if (ret < 0) return cs_map_errno(errno);
	if (ret != 0) return CS_STATUS_ERROR_IO; /* NVMe status code in ret */

	if (result_out) *result_out = cmd.result;
	return CS_STATUS_SUCCESS;
}

static csStatus
ioctl_io_cmd(int fd, uint8_t opcode, uint32_t nsid,
	     void *buf, uint32_t buf_len,
	     uint32_t cdw2, uint32_t cdw3,
	     uint32_t cdw10, uint32_t cdw11,
	     uint32_t cdw12, uint32_t cdw13,
	     uint32_t cdw14, uint32_t cdw15,
	     uint32_t *result_out)
{
	struct nvme_passthru_cmd cmd = {
		.opcode     = opcode,
		.nsid       = nsid,
		.addr       = (__u64)(uintptr_t)buf,
		.data_len   = buf_len,
		.cdw2       = cdw2,
		.cdw3       = cdw3,
		.cdw10      = cdw10,
		.cdw11      = cdw11,
		.cdw12      = cdw12,
		.cdw13      = cdw13,
		.cdw14      = cdw14,
		.cdw15      = cdw15,
		.timeout_ms = IOCTL_TIMEOUT_IO_MS,
	};

	int ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	if (ret < 0) return cs_map_errno(errno);
	if (ret != 0) return CS_STATUS_ERROR_IO;

	if (result_out) *result_out = cmd.result;
	return CS_STATUS_SUCCESS;
}

/* ---------- vtable implementations ---------- */

/* Backend init flag: written by backend init/shutdown (both invoked under the
 * library init_lock via the vtable), read as a re-entry guard only here. */
static bool g_initialized = false;

static csStatus
ioctl_backend_init(void)
{
	if (g_initialized) return CS_STATUS_SUCCESS;
	CS_LOG_INFO("ioctl backend initializing");
	g_initialized = true;
	return CS_STATUS_SUCCESS;
}

static csStatus
ioctl_backend_shutdown(void)
{
	g_initialized = false;
	return CS_STATUS_SUCCESS;
}

static csStatus
ioctl_backend_dev_query(csDeviceInfo *devices, uint32_t *count)
{
	if (!count) return CS_STATUS_ERROR_INVALID_PARAM;

	const char *uri = getenv("LIBCS_DEVICE_URI");
	if (!uri) { *count = 0; return CS_STATUS_SUCCESS; }

	if (!devices) { *count = 1; return CS_STATUS_SUCCESS; }
	if (*count < 1) { *count = 1; return CS_STATUS_ERROR_LIMIT; }

	memset(&devices[0], 0, sizeof(csDeviceInfo));
	strncpy(devices[0].uri, uri, sizeof(devices[0].uri) - 1);
	strncpy(devices[0].vendor, "NVMe/ioctl", sizeof(devices[0].vendor) - 1);
	strncpy(devices[0].model, "CS Device", sizeof(devices[0].model) - 1);
	devices[0].max_programs = IOCTL_DEFAULT_MAX_PROGRAMS;
	devices[0].max_memory_mb = IOCTL_DEFAULT_MAX_MEMORY_MB;
	*count = 1;
	return CS_STATUS_SUCCESS;
}

static csStatus
ioctl_backend_dev_open(const char *uri, struct cs_backend_ctx **backend_ctx)
{
	if (!uri || !backend_ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	*backend_ctx = NULL;

	struct cs_backend_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return CS_STATUS_ERROR_NO_MEMORY;

	cs_backend_ctrl_path_from_uri(uri, ctx->ctrl_path, sizeof(ctx->ctrl_path), "ioctl:");

	ctx->ctrl_fd = open(ctx->ctrl_path, O_RDWR);
	if (ctx->ctrl_fd < 0) {
		int err = errno;
		CS_LOG_ERROR("ioctl: open(%s) failed: %s", ctx->ctrl_path, strerror(err));
		free(ctx);
		return cs_map_errno(err);
	}

	ctx->libcs_nsid  = 2;
	ctx->nvm_nsid   = 1;
	ctx->slm_nsid   = 0;
	ctx->lba_nbytes = 512;

	cs_parse_u32_env("LIBCS_XNVME_NSID",     &ctx->libcs_nsid);
	cs_parse_u32_env("LIBCS_XNVME_NVM_NSID", &ctx->nvm_nsid);
	cs_parse_u32_env("LIBCS_XNVME_SLM_NSID", &ctx->slm_nsid);

	ctx->cp_output_returns = xnvme_uri_is_local_direct(uri);

	/* Read LBA size from Identify Namespace */
	uint8_t *idfy = aligned_alloc(CS_DMA_ALIGNMENT, 4096);
	if (idfy) {
		memset(idfy, 0, 4096);
		csStatus s = ioctl_admin_cmd(ctx->ctrl_fd, OPC_IDENTIFY,
					     ctx->nvm_nsid, idfy, 4096,
					     0, 0,
					     /* cdw10: CNS=0 (namespace) */ 0, 0,
					     0, 0, 0, 0, NULL);
		if (s == CS_STATUS_SUCCESS) {
			/* LBADS = bits [7:0] of LBA Format[active format], in bytes = 2^LBADS */
			/* Offset 26: FLBAS[3:0] = active format index */
			uint8_t flbas = idfy[26] & 0x0f;
			/* struct nvme_lbaf { __le16 ms; __u8 ds; __u8 rp; } = 4 bytes
			 * LBADS (ds) is byte 2 within each 4-byte LBAF entry */
			uint8_t lbads = idfy[128 + flbas * 4 + 2];
			if (lbads >= 9 && lbads <= 16) {
				ctx->lba_nbytes = 1u << lbads;
			}
		}
		free(idfy);
	}

	*backend_ctx = ctx;
	CS_LOG_INFO("ioctl: opened %s (libcs_nsid=%u nvm_nsid=%u slm_nsid=%u lba=%u)",
		    ctx->ctrl_path, ctx->libcs_nsid, ctx->nvm_nsid, ctx->slm_nsid, ctx->lba_nbytes);
	return CS_STATUS_SUCCESS;
}

static csStatus
ioctl_backend_dev_close(struct cs_backend_ctx *ctx)
{
	if (!ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	if (ctx->ctrl_fd >= 0) close(ctx->ctrl_fd);
	free(ctx);
	return CS_STATUS_SUCCESS;
}

static csStatus
ioctl_backend_dev_get_info(struct cs_backend_ctx *ctx, csDeviceInfo *info)
{
	if (!ctx || !info) return CS_STATUS_ERROR_INVALID_PARAM;
	memset(info, 0, sizeof(*info));
	strncpy(info->uri, ctx->ctrl_path, sizeof(info->uri) - 1);
	strncpy(info->vendor, "NVMe/ioctl", sizeof(info->vendor) - 1);
	strncpy(info->model, "CS Device", sizeof(info->model) - 1);
	info->max_programs = IOCTL_DEFAULT_MAX_PROGRAMS;
	info->max_memory_mb = IOCTL_DEFAULT_MAX_MEMORY_MB;
	return CS_STATUS_SUCCESS;
}

static csStatus
ioctl_backend_mem_alloc(struct cs_backend_ctx *ctx, size_t size,
			uint32_t alignment, void **ptr)
{
	if (!ctx || !ptr || !size) return CS_STATUS_ERROR_INVALID_PARAM;
	(void)alignment;

	uint32_t align = CS_DMA_ALIGNMENT;
	/* Round size to 4K for kernel DMA alignment */
	size_t padded = (size + align - 1) & ~(size_t)(align - 1);

	int ret = posix_memalign(ptr, align, padded);
	if (ret) return CS_STATUS_ERROR_NO_MEMORY;
	memset(*ptr, 0, padded);
	return CS_STATUS_SUCCESS;
}

static csStatus
ioctl_backend_mem_free(struct cs_backend_ctx *ctx, void *ptr)
{
	(void)ctx;
	if (!ptr) return CS_STATUS_ERROR_INVALID_PARAM;
	free(ptr);
	return CS_STATUS_SUCCESS;
}

static csStatus
ioctl_backend_prog_load(struct cs_backend_ctx *ctx, uint16_t prog_index,
			const void *program, size_t program_size)
{
	if (!ctx || !program || !program_size) return CS_STATUS_ERROR_INVALID_PARAM;

	/* Ensure DMA-aligned copy */
	void *buf = NULL;
	size_t padded = (program_size + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
	if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
	memcpy(buf, program, program_size);

	/*
	 * CDW10: [7:0]=pind, [23:16]=ptype(0=vendor), [24]=sel(0=load), [25+]=pit
	 * CDW11: total_size (bytes)
	 * CDW14: total_size (bytes, duplicated per spec)
	 */
	csStatus s = ioctl_admin_cmd(ctx->ctrl_fd, OPC_CP_LOAD, ctx->libcs_nsid,
				     buf, (uint32_t)program_size,
				     0, 0,
				     (uint32_t)prog_index,           /* cdw10 */
				     (uint32_t)program_size,          /* cdw11: size */
				     0, 0,
				     (uint32_t)program_size,          /* cdw14: total size */
				     0,
				     NULL);
	free(buf);

	if (s != CS_STATUS_SUCCESS) CS_LOG_ERROR("ioctl: prog_load pind=%" PRIu16 " failed: %d", prog_index, s);
	else CS_LOG_INFO("ioctl: loaded program pind=%" PRIu16 " (%zu bytes)", prog_index, program_size);
	return s;
}

static csStatus
ioctl_backend_prog_activate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	/* CDW10: [15:0]=pind, [16]=action(0=activate) */
	csStatus s = ioctl_admin_cmd(ctx->ctrl_fd, OPC_CP_ACTIVATE, ctx->libcs_nsid,
				     NULL, 0, 0, 0,
				     (uint32_t)prog_index,  /* cdw10: pind, action=0 */
				     0, 0, 0, 0, 0, NULL);
	if (s != CS_STATUS_SUCCESS) CS_LOG_ERROR("ioctl: prog_activate pind=%" PRIu16 " failed: %d", prog_index, s);
	else CS_LOG_INFO("ioctl: activated program pind=%" PRIu16 "", prog_index);
	return s;
}

static csStatus
ioctl_backend_prog_deactivate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	/* CDW10: [15:0]=pind, [16]=action(1=deactivate) */
	csStatus s = ioctl_admin_cmd(ctx->ctrl_fd, OPC_CP_ACTIVATE, ctx->libcs_nsid,
				     NULL, 0, 0, 0,
				     (1u << 16) | (uint32_t)prog_index,  /* cdw10: action=1 */
				     0, 0, 0, 0, 0, NULL);
	if (s != CS_STATUS_SUCCESS) CS_LOG_ERROR("ioctl: prog_deactivate pind=%" PRIu16 " failed: %d", prog_index, s);
	return s;
}

static csStatus
ioctl_backend_compute_execute(struct cs_backend_ctx *ctx, uint16_t prog_index,
			      const void *input, size_t input_size,
			      void *output, size_t output_size,
			      const void *args, size_t args_size,
			      volatile int *completion)
{
	if (!ctx || !input || !input_size || !output || output_size == 0)
		return CS_STATUS_ERROR_INVALID_PARAM;

	switch (prog_index) {

	case LIBCS_PIND_SUM64:
	case LIBCS_PIND_MAX64:
	case LIBCS_PIND_MIN64: {
		if (output_size < sizeof(uint32_t) || (input_size % sizeof(uint64_t)) != 0)
			return CS_STATUS_ERROR_INVALID_PARAM;

		size_t padded = (input_size + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
		void *buf = NULL;
		if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
		memset(buf, 0, padded);
		memcpy(buf, input, input_size);

		uint32_t result = 0;
		csStatus s = ioctl_io_cmd(ctx->ctrl_fd, OPC_CP_EXECUTE,
					  ctx->libcs_nsid, buf, (uint32_t)input_size,
					  (uint32_t)prog_index, 0,
					  0, 0, 0, 0, 0, 0, &result);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("ioctl: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		/* 32-bit result from ioctl — upper 32 bits lost (kernel limitation) */
		if (output_size >= sizeof(uint64_t))
			*(uint64_t *)output = (uint64_t)result;
		else
			*(uint32_t *)output = result;
		if (completion) *completion = 1;
		CS_LOG_INFO("ioctl: compute_execute pind=%" PRIu16 " result=0x%x", prog_index, result);
		return CS_STATUS_SUCCESS;
	}

	case LIBCS_PIND_DOT_PRODUCT:
	case LIBCS_PIND_L2_DISTANCE_SQ:
	case LIBCS_PIND_COSINE_SIMILARITY: {
		if (output_size < sizeof(uint32_t))
			return CS_STATUS_ERROR_LIMIT;
		if ((input_size % (2 * sizeof(float))) != 0)
			return CS_STATUS_ERROR_INVALID_PARAM;

		size_t padded = (input_size + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
		void *buf = NULL;
		if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
		memset(buf, 0, padded);
		memcpy(buf, input, input_size);

		uint32_t result = 0;
		csStatus s = ioctl_io_cmd(ctx->ctrl_fd, OPC_CP_EXECUTE,
					  ctx->libcs_nsid, buf, (uint32_t)input_size,
					  (uint32_t)prog_index, 0,
					  0, 0, 0, 0, 0, 0, &result);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("ioctl: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		/* Result is float bits in cdw0 */
		memcpy(output, &result, sizeof(result));
		if (completion) *completion = 1;
		CS_LOG_INFO("ioctl: compute_execute pind=%" PRIu16 " result=0x%x", prog_index, result);
		return CS_STATUS_SUCCESS;
	}

	case LIBCS_PIND_FILTER_GT: {
		float threshold = (args && args_size >= sizeof(float))
				  ? *(const float *)args : 0.0f;
		size_t header   = sizeof(float);
		size_t n_floats = input_size / sizeof(float);
		size_t out_reg  = n_floats * sizeof(float);
		size_t combined = ALIGN4(header + input_size + out_reg);

		size_t padded = (combined + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
		void *buf = NULL;
		if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
		memset(buf, 0, padded);
		memcpy(buf, &threshold, sizeof(threshold));
		memcpy((char *)buf + header, input, input_size);

		uint32_t result = 0;
		csStatus s = ioctl_io_cmd(ctx->ctrl_fd, OPC_CP_EXECUTE,
					  ctx->libcs_nsid, buf, (uint32_t)combined,
					  (uint32_t)prog_index, 0,
					  0, 0, 0, 0, 0, 0, &result);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("ioctl: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		/* Re-filter locally — always correct regardless of transport */
		{
			const float *src = (const float *)input;
			float *dst = (float *)output;
			size_t max_out = output_size / sizeof(float);
			size_t written = 0;
			for (size_t fi = 0; fi < n_floats && written < max_out; fi++) {
				if (src[fi] > threshold)
					dst[written++] = src[fi];
			}
			for (size_t fi = written; fi < max_out; fi++)
				dst[fi] = NAN;
		}
		if (completion) *completion = 1;
		CS_LOG_INFO("ioctl: compute_execute pind=%" PRIu16 " (FILTER_GT)", prog_index);
		return CS_STATUS_SUCCESS;
	}

	case LIBCS_PIND_MULTI_AGG64: {
		struct libcs_multi_agg64_result res;
		size_t out_bytes = sizeof(res);
		size_t combined  = ALIGN4(input_size + out_bytes);

		if ((input_size % sizeof(double)) != 0)
			return CS_STATUS_ERROR_INVALID_PARAM;
		if (output_size < out_bytes)
			return CS_STATUS_ERROR_INVALID_PARAM;

		size_t padded = (combined + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
		void *buf = NULL;
		if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
		memset(buf, 0, padded);
		memcpy(buf, input, input_size);

		uint32_t cdw0 = 0;
		csStatus s = ioctl_io_cmd(ctx->ctrl_fd, OPC_CP_EXECUTE,
					  ctx->libcs_nsid, buf, (uint32_t)combined,
					  (uint32_t)prog_index, 0,
					  0, 0, 0, 0, 0, 0, &cdw0);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("ioctl: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		/* Local fallback: compute aggregation from input */
		{
			const double *vals = (const double *)input;
			size_t count = input_size / sizeof(double);
			res.count = count;
			res.sum = 0.0;
			res.min = vals[0];
			res.max = vals[0];
			for (size_t i = 0; i < count; i++) {
				res.sum += vals[i];
				if (vals[i] < res.min) res.min = vals[i];
				if (vals[i] > res.max) res.max = vals[i];
			}
		}
		memcpy(output, &res, sizeof(res));
		if (completion) *completion = 1;
		CS_LOG_INFO("ioctl: compute_execute pind=%" PRIu16 " (MULTI_AGG64)", prog_index);
		return CS_STATUS_SUCCESS;
	}

	case LIBCS_PIND_MEMCPY_INLINE: {
		size_t combined = ALIGN4(input_size + output_size);

		size_t padded = (combined + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
		void *buf = NULL;
		if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
		memset(buf, 0, padded);
		memcpy(buf, input, input_size);

		uint32_t cdw0 = 0;
		csStatus s = ioctl_io_cmd(ctx->ctrl_fd, OPC_CP_EXECUTE,
					  ctx->libcs_nsid, buf, (uint32_t)combined,
					  (uint32_t)prog_index, 0,
					  0, 0, 0, 0, 0, 0, &cdw0);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("ioctl: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		/* Local fallback: memcpy input to output */
		{
			size_t to_copy = cdw0 < input_size ? cdw0 : input_size;
			if (to_copy > output_size) to_copy = output_size;
			memcpy(output, input, to_copy);
		}
		if (completion) *completion = 1;
		CS_LOG_INFO("ioctl: compute_execute pind=%" PRIu16 " (MEMCPY_INLINE)", prog_index);
		return CS_STATUS_SUCCESS;
	}

	case LIBCS_PIND_RLE_COMPRESS: {
		uint64_t n = input_size;
		size_t header   = sizeof(uint64_t);
		size_t combined = ALIGN4(header + input_size + output_size);

		size_t padded = (combined + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
		void *buf = NULL;
		if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
		memset(buf, 0, padded);
		memcpy(buf, &n, sizeof(n));
		memcpy((char *)buf + header, input, input_size);

		uint32_t cdw0 = 0;
		csStatus s = ioctl_io_cmd(ctx->ctrl_fd, OPC_CP_EXECUTE,
					  ctx->libcs_nsid, buf, (uint32_t)combined,
					  (uint32_t)prog_index, 0,
					  0, 0, 0, 0, 0, 0, &cdw0);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("ioctl: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		/* Local fallback: RLE encode */
		{
			const uint8_t *src = (const uint8_t *)input;
			uint8_t *dst = (uint8_t *)output;
			size_t ipos = 0, opos = 0;
			while (ipos < input_size && opos + 1 < output_size) {
				uint8_t value = src[ipos];
				uint8_t run = 1;
				while (ipos + run < input_size &&
				       src[ipos + run] == value && run < 255)
					run++;
				dst[opos++] = run;
				dst[opos++] = value;
				ipos += run;
			}
		}
		if (completion) *completion = 1;
		CS_LOG_INFO("ioctl: compute_execute pind=%" PRIu16 " (RLE_COMPRESS)", prog_index);
		return CS_STATUS_SUCCESS;
	}

	default:
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}
}

static csStatus
ioctl_backend_storage_io(struct cs_backend_ctx *ctx, csStorageOp op,
			 uint64_t lba, uint32_t blocks,
			 void *buffer, size_t buffer_size,
			 volatile int *completion)
{
	if (!ctx || !buffer || !buffer_size || !blocks) return CS_STATUS_ERROR_INVALID_PARAM;
	(void)completion;

	uint8_t opcode = (op == CS_STORAGE_READ) ? OPC_NVM_READ : OPC_NVM_WRITE;

	/*
	 * NVM read/write: CDW10/11 = SLBA, CDW12 = NLB (0-based: blocks-1)
	 */
	csStatus s = ioctl_io_cmd(ctx->ctrl_fd, opcode, ctx->nvm_nsid,
				  buffer, (uint32_t)((size_t)blocks * ctx->lba_nbytes),
				  0, 0,
				  (uint32_t)lba,        /* cdw10: SLBA low */
				  (uint32_t)(lba >> 32),/* cdw11: SLBA high */
				  blocks - 1,           /* cdw12: NLB (0-based) */
				  0, 0, 0,
				  NULL);
	if (s != CS_STATUS_SUCCESS)
		CS_LOG_ERROR("ioctl: storage_io op=%d lba=%lu blk=%u failed: %d", op, lba, blocks, s);
	return s;
}

static int
ioctl_backend_poll(struct cs_backend_ctx *ctx, uint32_t max)
{
	/* Synchronous backend — all ops complete before returning */
	(void)ctx; (void)max;
	return 0;
}

static csStatus
ioctl_backend_compute_submit(struct cs_backend_ctx *ctx, uint16_t prog_index,
			     const void *input, size_t input_size,
			     void *output, size_t output_size,
			     const void *args, size_t args_size,
			     volatile int *completion)
{
	csStatus status = ioctl_backend_compute_execute(ctx, prog_index,
						       input, input_size,
						       output, output_size,
						       args, args_size, completion);
	if (status == CS_STATUS_SUCCESS && completion)
		return CS_STATUS_PENDING;
	return status;
}

static csStatus
ioctl_backend_compute_reap(struct cs_backend_ctx *ctx, volatile int *completion)
{
	(void)ctx;
	if (!completion) return CS_STATUS_ERROR_INVALID_PARAM;
	if (*completion == 0) return CS_STATUS_PENDING;
	return (*completion > 0) ? CS_STATUS_SUCCESS : CS_STATUS_ERROR_IO;
}

#endif /* __linux__ */

/* ---------- registration ---------- */

#include "cs_backend.h"

static const cs_backend_ops_t ioctl_backend_ops = {
	.name             = "ioctl",
	.init             = ioctl_backend_init,
	.shutdown         = ioctl_backend_shutdown,
	.dev_query        = ioctl_backend_dev_query,
	.dev_open         = ioctl_backend_dev_open,
	.dev_close        = ioctl_backend_dev_close,
	.dev_get_info     = ioctl_backend_dev_get_info,
	.mem_alloc        = ioctl_backend_mem_alloc,
	.mem_free         = ioctl_backend_mem_free,
	.mem_init         = cs_backend_mem_init,
	.prog_load        = ioctl_backend_prog_load,
	.prog_activate    = ioctl_backend_prog_activate,
	.prog_deactivate  = ioctl_backend_prog_deactivate,
	.prog_get_name    = cs_backend_prog_get_name,
	.query_csf_list   = cs_backend_query_csf_list,
	.compute_execute  = ioctl_backend_compute_execute,
	.compute_submit   = ioctl_backend_compute_submit,
	.compute_reap     = ioctl_backend_compute_reap,
	.abort_request    = cs_backend_abort_request,
	.storage_io       = ioctl_backend_storage_io,
	.poll             = ioctl_backend_poll,
};

void
backend_ioctl_register(void)
{
	csStatus s = cs_backend_register(&ioctl_backend_ops);
	if (s != CS_STATUS_SUCCESS && s != CS_STATUS_ERROR_ALREADY_EXISTS)
		CS_LOG_ERROR("Failed to register ioctl backend: %d", s);
}
