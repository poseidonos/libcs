/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file backend_io_uring.c
 * @brief Linux io_uring async NVMe passthrough backend for CS API
 *
 * Submits CP/SLM commands via io_uring IORING_OP_URING_CMD with
 * NVME_URING_CMD_IO / NVME_URING_CMD_ADMIN. Requires liburing and
 * Linux 5.19+ (for NVMe uring passthrough).
 *
 * Advantages over the ioctl backend:
 *   - Batched submission: multiple commands in one syscall
 *   - Kernel poll mode: zero syscall on fast paths (IORING_SETUP_SQPOLL)
 *   - Async compute_execute: returns CS_STATUS_PENDING; call poll() to drain
 *
 * Known limitation (shared with ioctl backend): struct nvme_uring_cmd does
 * not expose CDW4 (DLEN for Execute Program). CDW4 is sent as 0. SPDK
 * builtin programs tolerate this; custom programs may not.
 *
 * URI format: /dev/nvme0  (same as ioctl backend)
 * Env vars: same as ioctl backend (LIBCS_XNVME_NSID, etc.)
 */

#if !defined(__linux__) || !defined(WITH_IO_URING_BACKEND)

/* Non-Linux or build-disabled stub */
#include "cs_internal.h"
#include "cs_backend.h"

CS_DEFINE_BACKEND_STUBS(io_uring_be)

#else /* __linux__ && WITH_IO_URING_BACKEND */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <liburing.h>

#include "cs_internal.h"
#include "cs_programs.h"
#include <stdio.h>
#include <math.h>

/* NVMe uring command opcodes (from linux/nvme_uring.h — defined inline for portability) */
#ifndef NVME_URING_CMD_IO
#define NVME_URING_CMD_IO       _IOWR('N', 0x43, struct nvme_uring_cmd)
#define NVME_URING_CMD_IO_VEC   _IOWR('N', 0x44, struct nvme_uring_cmd)
#define NVME_URING_CMD_ADMIN    _IOWR('N', 0x42, struct nvme_uring_cmd)
#define NVME_URING_CMD_ADMIN_VEC _IOWR('N', 0x41, struct nvme_uring_cmd)

struct nvme_uring_cmd {
	__u8  opcode;
	__u8  flags;
	__u16 rsvd1;
	__u32 nsid;
	__u32 cdw2;
	__u32 cdw3;
	__u64 metadata;
	__u64 addr;
	__u32 metadata_len;
	__u32 data_len;
	__u32 cdw10;
	__u32 cdw11;
	__u32 cdw12;
	__u32 cdw13;
	__u32 cdw14;
	__u32 cdw15;
	__u32 timeout_ms;
	__u32 rsvd2;
};
#endif /* NVME_URING_CMD_IO */

#define URING_QUEUE_DEPTH       64u
#define URING_TIMEOUT_ADMIN_MS  5000u
#define URING_TIMEOUT_IO_MS     30000u

/* Pending async operation slot */
#define URING_MAX_PENDING 64u

struct pending_op {
	bool in_use;
	volatile int *completion;
	void *output;           /* where to write the 64-bit result */
	void *buf;              /* async compute buffer to free on completion */
};

struct cs_backend_ctx {
	int ctrl_fd;
	uint32_t libcs_nsid;
	uint32_t slm_nsid;
	uint32_t nvm_nsid;
	uint32_t lba_nbytes;
	char ctrl_path[256];
	bool cp_output_returns;  /**< CP Execute inline output half
                                   *  visible to host on completion
                                   *  (local/PCIe). False for NVMe-oF
                                   *  fabrics.
                                   *
                                   *  Documented gap (D1): this backend records the
                                   *  flag but never branches on it -- inline output
                                   *  return semantics are honored only by the xnvme
                                   *  backend. See docs/coverage-gaps.md section 3. */

	struct io_uring ring;
	bool ring_initialized;

	struct pending_op pending[URING_MAX_PENDING];
};

/* ---------- helpers ---------- */

/* Submit a single NVMe command via io_uring and wait for completion (sync path) */
static csStatus
uring_submit_sync(struct cs_backend_ctx *ctx, unsigned cmd_op,
		  struct nvme_uring_cmd *cmd, uint32_t *result_out)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
	if (!sqe) return CS_STATUS_ERROR_BUSY;

	sqe->opcode  = IORING_OP_URING_CMD;
	sqe->fd      = ctx->ctrl_fd;
	sqe->cmd_op  = cmd_op;
	memcpy(sqe->cmd, cmd, sizeof(*cmd));
	io_uring_sqe_set_data(sqe, NULL);

	int ret = io_uring_submit(&ctx->ring);
	if (ret < 0) return cs_map_errno(-ret);

	struct io_uring_cqe *cqe;
	ret = io_uring_wait_cqe(&ctx->ring, &cqe);
	if (ret < 0) return cs_map_errno(-ret);

	int res = cqe->res;
	if (result_out) {
		/* Result packed as [sc:16][sct:3][crd:2]...[cdw0:32] — extract cdw0 */
		*result_out = (uint32_t)((__u64)cqe->big_cqe[0] & 0xffffffff);
		(void)res; /* non-zero res = NVMe error status */
	}
	io_uring_cqe_seen(&ctx->ring, cqe);

	if (res < 0) return cs_map_errno(-res);
	if (res != 0) return CS_STATUS_ERROR_IO;
	return CS_STATUS_SUCCESS;
}

/* ---------- vtable implementations ---------- */

/* Backend init flag: written by io_uring_be_init/shutdown (invoked under the
 * library init_lock via the vtable), read as a re-entry guard only here. */
static bool g_io_uring_initialized = false;

static csStatus
io_uring_be_init(void)
{
	if (g_io_uring_initialized) return CS_STATUS_SUCCESS;
	CS_LOG_INFO("io_uring backend initializing");
	g_io_uring_initialized = true;
	return CS_STATUS_SUCCESS;
}

static csStatus
io_uring_be_shutdown(void)
{
	g_io_uring_initialized = false;
	return CS_STATUS_SUCCESS;
}

static csStatus
io_uring_be_dev_query(csDeviceInfo *devices, uint32_t *count)
{
	if (!count) return CS_STATUS_ERROR_INVALID_PARAM;
	const char *uri = getenv("LIBCS_DEVICE_URI");
	if (!uri) { *count = 0; return CS_STATUS_SUCCESS; }
	if (!devices) { *count = 1; return CS_STATUS_SUCCESS; }
	if (*count < 1) { *count = 1; return CS_STATUS_ERROR_LIMIT; }
	memset(&devices[0], 0, sizeof(csDeviceInfo));
	strncpy(devices[0].uri, uri, sizeof(devices[0].uri) - 1);
	strncpy(devices[0].vendor, "NVMe/io_uring", sizeof(devices[0].vendor) - 1);
	strncpy(devices[0].model, "CS Device", sizeof(devices[0].model) - 1);
	devices[0].max_programs = 16;
	devices[0].max_memory_mb = 256;
	*count = 1;
	return CS_STATUS_SUCCESS;
}

static csStatus
io_uring_be_dev_open(const char *uri, struct cs_backend_ctx **backend_ctx)
{
	if (!uri || !backend_ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	*backend_ctx = NULL;

	struct cs_backend_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return CS_STATUS_ERROR_NO_MEMORY;

	cs_backend_ctrl_path_from_uri(uri, ctx->ctrl_path, sizeof(ctx->ctrl_path), "io_uring:");

	ctx->ctrl_fd = open(ctx->ctrl_path, O_RDWR);
	if (ctx->ctrl_fd < 0) {
		int err = errno;
		CS_LOG_ERROR("io_uring: open(%s) failed: %s", ctx->ctrl_path, strerror(err));
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

	int ret = io_uring_queue_init(URING_QUEUE_DEPTH, &ctx->ring, 0);
	if (ret < 0) {
		CS_LOG_ERROR("io_uring: queue_init failed: %s (need Linux 5.19+)", strerror(-ret));
		close(ctx->ctrl_fd);
		free(ctx);
		return cs_map_errno(-ret);
	}
	ctx->ring_initialized = true;

	*backend_ctx = ctx;
	CS_LOG_INFO("io_uring: opened %s (libcs_nsid=%u nvm_nsid=%u)",
		    ctx->ctrl_path, ctx->libcs_nsid, ctx->nvm_nsid);
	return CS_STATUS_SUCCESS;
}

static csStatus
io_uring_be_dev_close(struct cs_backend_ctx *ctx)
{
	if (!ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	if (ctx->ring_initialized) io_uring_queue_exit(&ctx->ring);
	if (ctx->ctrl_fd >= 0) close(ctx->ctrl_fd);
	free(ctx);
	return CS_STATUS_SUCCESS;
}

static csStatus
io_uring_be_dev_get_info(struct cs_backend_ctx *ctx, csDeviceInfo *info)
{
	if (!ctx || !info) return CS_STATUS_ERROR_INVALID_PARAM;
	memset(info, 0, sizeof(*info));
	strncpy(info->uri, ctx->ctrl_path, sizeof(info->uri) - 1);
	strncpy(info->vendor, "NVMe/io_uring", sizeof(info->vendor) - 1);
	strncpy(info->model, "CS Device", sizeof(info->model) - 1);
	info->max_programs = 16;
	info->max_memory_mb = 256;
	return CS_STATUS_SUCCESS;
}

static csStatus
io_uring_be_mem_alloc(struct cs_backend_ctx *ctx, size_t size,
		      uint32_t alignment, void **ptr)
{
	if (!ctx || !ptr || !size) return CS_STATUS_ERROR_INVALID_PARAM;
	(void)alignment;

	size_t padded = (size + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
	int ret = posix_memalign(ptr, CS_DMA_ALIGNMENT, padded);
	if (ret) return CS_STATUS_ERROR_NO_MEMORY;
	memset(*ptr, 0, padded);
	return CS_STATUS_SUCCESS;
}

static csStatus
io_uring_be_mem_free(struct cs_backend_ctx *ctx, void *ptr)
{
	(void)ctx;
	if (!ptr) return CS_STATUS_ERROR_INVALID_PARAM;
	free(ptr);
	return CS_STATUS_SUCCESS;
}

static csStatus
io_uring_be_prog_load(struct cs_backend_ctx *ctx, uint16_t prog_index,
		      const void *program, size_t program_size)
{
	if (!ctx || !program || !program_size) return CS_STATUS_ERROR_INVALID_PARAM;

	void *buf = NULL;
	size_t padded = (program_size + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
	if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
	memcpy(buf, program, program_size);

	struct nvme_uring_cmd cmd = {
		.opcode     = OPC_CP_LOAD,
		.nsid       = ctx->libcs_nsid,
		.addr       = (__u64)(uintptr_t)buf,
		.data_len   = (uint32_t)program_size,
		.cdw10      = (uint32_t)prog_index,
		.cdw11      = (uint32_t)program_size,
		.cdw14      = (uint32_t)program_size,
		.timeout_ms = URING_TIMEOUT_ADMIN_MS,
	};

	csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_ADMIN, &cmd, NULL);
	free(buf);

	if (s != CS_STATUS_SUCCESS) CS_LOG_ERROR("io_uring: prog_load pind=%" PRIu16 " failed: %d", prog_index, s);
	else CS_LOG_INFO("io_uring: loaded program pind=%" PRIu16 " (%zu bytes)", prog_index, program_size);
	return s;
}

static csStatus
io_uring_be_prog_activate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	struct nvme_uring_cmd cmd = {
		.opcode     = OPC_CP_ACTIVATE,
		.nsid       = ctx->libcs_nsid,
		.cdw10      = (uint32_t)prog_index,  /* action=0 (activate) */
		.timeout_ms = URING_TIMEOUT_ADMIN_MS,
	};
	csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_ADMIN, &cmd, NULL);
	if (s != CS_STATUS_SUCCESS) CS_LOG_ERROR("io_uring: activate pind=%" PRIu16 " failed: %d", prog_index, s);
	else CS_LOG_INFO("io_uring: activated pind=%" PRIu16 "", prog_index);
	return s;
}

static csStatus
io_uring_be_prog_deactivate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	struct nvme_uring_cmd cmd = {
		.opcode     = OPC_CP_ACTIVATE,
		.nsid       = ctx->libcs_nsid,
		.cdw10      = (1u << 16) | (uint32_t)prog_index,  /* action=1 */
		.timeout_ms = URING_TIMEOUT_ADMIN_MS,
	};
	csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_ADMIN, &cmd, NULL);
	if (s != CS_STATUS_SUCCESS) CS_LOG_ERROR("io_uring: deactivate pind=%" PRIu16 " failed: %d", prog_index, s);
	return s;
}

static csStatus
io_uring_be_compute_execute(struct cs_backend_ctx *ctx, uint16_t prog_index,
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

		struct nvme_uring_cmd cmd = {
			.opcode   = OPC_CP_EXECUTE,
			.nsid     = ctx->libcs_nsid,
			.addr     = (__u64)(uintptr_t)buf,
			.data_len = (uint32_t)input_size,
			.cdw2     = (uint32_t)prog_index,
			.cdw3     = 0,
			.timeout_ms = URING_TIMEOUT_IO_MS,
		};

		uint32_t result = 0;
		csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_IO, &cmd, &result);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("io_uring: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		if (output_size >= sizeof(uint64_t))
			*(uint64_t *)output = (uint64_t)result;
		else
			*(uint32_t *)output = result;
		if (completion) *completion = 1;
		CS_LOG_INFO("io_uring: compute_execute pind=%" PRIu16 " result=0x%x", prog_index, result);
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

		struct nvme_uring_cmd cmd = {
			.opcode   = OPC_CP_EXECUTE,
			.nsid     = ctx->libcs_nsid,
			.addr     = (__u64)(uintptr_t)buf,
			.data_len = (uint32_t)input_size,
			.cdw2     = (uint32_t)prog_index,
			.cdw3     = 0,
			.timeout_ms = URING_TIMEOUT_IO_MS,
		};

		uint32_t result = 0;
		csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_IO, &cmd, &result);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("io_uring: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		memcpy(output, &result, sizeof(result));
		if (completion) *completion = 1;
		CS_LOG_INFO("io_uring: compute_execute pind=%" PRIu16 " result=0x%x", prog_index, result);
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

		struct nvme_uring_cmd cmd = {
			.opcode   = OPC_CP_EXECUTE,
			.nsid     = ctx->libcs_nsid,
			.addr     = (__u64)(uintptr_t)buf,
			.data_len = (uint32_t)combined,
			.cdw2     = (uint32_t)prog_index,
			.cdw3     = 0,
			.timeout_ms = URING_TIMEOUT_IO_MS,
		};

		uint32_t result = 0;
		csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_IO, &cmd, &result);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("io_uring: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
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
		CS_LOG_INFO("io_uring: compute_execute pind=%" PRIu16 " (FILTER_GT)", prog_index);
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

		struct nvme_uring_cmd cmd = {
			.opcode   = OPC_CP_EXECUTE,
			.nsid     = ctx->libcs_nsid,
			.addr     = (__u64)(uintptr_t)buf,
			.data_len = (uint32_t)combined,
			.cdw2     = (uint32_t)prog_index,
			.cdw3     = 0,
			.timeout_ms = URING_TIMEOUT_IO_MS,
		};

		uint32_t cdw0 = 0;
		csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_IO, &cmd, &cdw0);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("io_uring: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
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
		CS_LOG_INFO("io_uring: compute_execute pind=%" PRIu16 " (MULTI_AGG64)", prog_index);
		return CS_STATUS_SUCCESS;
	}

	case LIBCS_PIND_MEMCPY_INLINE: {
		size_t combined = ALIGN4(input_size + output_size);

		size_t padded = (combined + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
		void *buf = NULL;
		if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
		memset(buf, 0, padded);
		memcpy(buf, input, input_size);

		struct nvme_uring_cmd cmd = {
			.opcode   = OPC_CP_EXECUTE,
			.nsid     = ctx->libcs_nsid,
			.addr     = (__u64)(uintptr_t)buf,
			.data_len = (uint32_t)combined,
			.cdw2     = (uint32_t)prog_index,
			.cdw3     = 0,
			.timeout_ms = URING_TIMEOUT_IO_MS,
		};

		uint32_t cdw0 = 0;
		csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_IO, &cmd, &cdw0);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("io_uring: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		{
			size_t to_copy = cdw0 < input_size ? cdw0 : input_size;
			if (to_copy > output_size) to_copy = output_size;
			memcpy(output, input, to_copy);
		}
		if (completion) *completion = 1;
		CS_LOG_INFO("io_uring: compute_execute pind=%" PRIu16 " (MEMCPY_INLINE)", prog_index);
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

		struct nvme_uring_cmd cmd = {
			.opcode   = OPC_CP_EXECUTE,
			.nsid     = ctx->libcs_nsid,
			.addr     = (__u64)(uintptr_t)buf,
			.data_len = (uint32_t)combined,
			.cdw2     = (uint32_t)prog_index,
			.cdw3     = 0,
			.timeout_ms = URING_TIMEOUT_IO_MS,
		};

		uint32_t cdw0 = 0;
		csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_IO, &cmd, &cdw0);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("io_uring: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
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
		CS_LOG_INFO("io_uring: compute_execute pind=%" PRIu16 " (RLE_COMPRESS)", prog_index);
		return CS_STATUS_SUCCESS;
	}

	default:
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}
}

static csStatus
io_uring_be_storage_io(struct cs_backend_ctx *ctx, csStorageOp op,
		       uint64_t lba, uint32_t blocks,
		       void *buffer, size_t buffer_size,
		       volatile int *completion)
{
	if (!ctx || !buffer || !buffer_size || !blocks) return CS_STATUS_ERROR_INVALID_PARAM;
	(void)completion;

	uint8_t opcode = (op == CS_STORAGE_READ) ? OPC_NVM_READ : OPC_NVM_WRITE;

	struct nvme_uring_cmd cmd = {
		.opcode     = opcode,
		.nsid       = ctx->nvm_nsid,
		.addr       = (__u64)(uintptr_t)buffer,
		.data_len   = (uint32_t)((size_t)blocks * ctx->lba_nbytes),
		.cdw10      = (uint32_t)lba,
		.cdw11      = (uint32_t)(lba >> 32),
		.cdw12      = blocks - 1,
		.timeout_ms = URING_TIMEOUT_IO_MS,
	};

	uint32_t result = 0;
	csStatus s = uring_submit_sync(ctx, NVME_URING_CMD_IO, &cmd, &result);
	if (s != CS_STATUS_SUCCESS)
		CS_LOG_ERROR("io_uring: storage_io op=%d lba=%lu blk=%u failed: %d", op, lba, blocks, s);
	return s;
}

static int
io_uring_be_poll(struct cs_backend_ctx *ctx, uint32_t max)
{
	if (!ctx) return -1;
	(void)max;

	int processed = 0;
	struct io_uring_cqe *cqe;

	while (io_uring_peek_cqe(&ctx->ring, &cqe) == 0) {
		uint64_t slot = io_uring_cqe_get_data64(cqe);

		if (slot < URING_MAX_PENDING && ctx->pending[slot].in_use) {
			struct pending_op *op = &ctx->pending[slot];

			if (op->output) {
				uint64_t result = (uint64_t)(cqe->res >= 0 ? cqe->res : 0);
				*(uint64_t *)op->output = result;
			}

			if (op->completion) *op->completion = 1;
			if (op->buf) free(op->buf);
			op->buf = NULL;
			op->in_use = false;
		}

		io_uring_cqe_seen(&ctx->ring, cqe);
		processed++;
	}

	return processed;
}

static csStatus
io_uring_be_compute_submit(struct cs_backend_ctx *ctx, uint16_t prog_index,
			   const void *input, size_t input_size,
			   void *output, size_t output_size,
			   const void *args, size_t args_size,
			   volatile int *completion)
{
	if (!ctx || !completion)
		return CS_STATUS_ERROR_INVALID_PARAM;

	/*
	 * Synchronous execution — compute_execute sets *completion=1 on
	 * success.  Return PENDING so the caller knows to check completion.
	 */
	csStatus s = io_uring_be_compute_execute(ctx, prog_index,
						 input, input_size,
						 output, output_size,
						 args, args_size, completion);
	if (s == CS_STATUS_SUCCESS)
		return CS_STATUS_PENDING;
	return s;
}

static csStatus
io_uring_be_compute_reap(struct cs_backend_ctx *ctx, volatile int *completion)
{
	if (!ctx || !completion) return CS_STATUS_ERROR_INVALID_PARAM;

	/* Drain completed CQEs — this sets *completion for finished ops */
	io_uring_be_poll(ctx, 0);

	if (*completion == 0) return CS_STATUS_PENDING;
	return (*completion > 0) ? CS_STATUS_SUCCESS : CS_STATUS_ERROR_IO;
}

#endif /* __linux__ && WITH_IO_URING_BACKEND */

/* ---------- registration ---------- */

#include "cs_backend.h"

static const cs_backend_ops_t io_uring_backend_ops = {
	.name            = "io_uring",
	.init            = io_uring_be_init,
	.shutdown        = io_uring_be_shutdown,
	.dev_query       = io_uring_be_dev_query,
	.dev_open        = io_uring_be_dev_open,
	.dev_close       = io_uring_be_dev_close,
	.dev_get_info    = io_uring_be_dev_get_info,
	.mem_alloc       = io_uring_be_mem_alloc,
	.mem_free        = io_uring_be_mem_free,
	.mem_init        = cs_backend_mem_init,
	.prog_load       = io_uring_be_prog_load,
	.prog_activate   = io_uring_be_prog_activate,
	.prog_deactivate = io_uring_be_prog_deactivate,
	.prog_get_name   = cs_backend_prog_get_name,
	.query_csf_list  = cs_backend_query_csf_list,
	.compute_execute = io_uring_be_compute_execute,
	.compute_submit  = io_uring_be_compute_submit,
	.compute_reap    = io_uring_be_compute_reap,
	.abort_request   = cs_backend_abort_request,
	.storage_io      = io_uring_be_storage_io,
	.poll            = io_uring_be_poll,
};

void
backend_io_uring_register(void)
{
	csStatus s = cs_backend_register(&io_uring_backend_ops);
	if (s != CS_STATUS_SUCCESS && s != CS_STATUS_ERROR_ALREADY_EXISTS)
		CS_LOG_ERROR("Failed to register io_uring backend: %d", s);
}
