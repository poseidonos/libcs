/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file backend_libnvme.c
 * @brief libnvme-based NVMe passthrough backend for CS API
 *
 * Submits CP/SLM admin and IO commands via libnvme's passthrough helpers.
 * Compared to backend_ioctl.c, this backend gains:
 *   - NVMe-oF support: fabric-connected controllers appear as /dev/nvmeX
 *     after `nvme connect`; open them identically to local devices.
 *   - NVME_IOCTL_IO64_CMD fallback for large-LBA NVMe-oF targets.
 *   - Proper LBA size detection via nvme_identify_ns().
 *
 * Known limitation: CDW4 (DLEN) cannot be set via nvme_io_passthru —
 * it is sent as 0. SPDK builtin programs tolerate this; custom eBPF
 * programs that read CDW4 explicitly may not.
 *
 * URI format: /dev/nvme0  (NVMe controller char device, local or fabric)
 *   Strip the namespace suffix: /dev/nvme0n1 → /dev/nvme0
 *   Explicit prefix also accepted: nvme:/dev/nvme0
 *
 * Environment variables (shared with other backends):
 *   LIBCS_XNVME_NSID      — compute namespace ID (default 2)
 *   LIBCS_XNVME_NVM_NSID  — NVM namespace ID (default 1)
 *   LIBCS_XNVME_SLM_NSID  — SLM namespace ID (optional)
 */

#ifndef WITH_LIBNVME_BACKEND

/* Stub when backend is disabled at compile time */
void backend_libnvme_register(void) {}

#else /* WITH_LIBNVME_BACKEND */

#ifndef __linux__

#include "cs_internal.h"

CS_DEFINE_BACKEND_STUBS(libnvme_backend)

#else /* __linux__ */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libnvme.h>

#include "cs_internal.h"
#include "cs_programs.h"
#include <stdio.h>
#include <math.h>

#define ALIGN4(_n) (((_n) + 3U) & ~((size_t)3U))

struct libcs_multi_agg64_result {
	uint64_t count;
	double   sum;
	double   min;
	double   max;
};

/* CP/SLM opcodes (same as ioctl backend) */
#define LIBNVME_OPC_CP_LOAD      0x85u
#define LIBNVME_OPC_CP_ACTIVATE  0x88u
#define LIBNVME_OPC_CP_EXECUTE   0x01u
#define LIBNVME_OPC_NVM_WRITE    0x01u
#define LIBNVME_OPC_NVM_READ     0x02u
#define LIBNVME_OPC_IDENTIFY     0x06u

#define LIBNVME_TIMEOUT_ADMIN_MS  5000u
#define LIBNVME_TIMEOUT_IO_MS     30000u

/* Advertised device capability defaults for this backend */
#define LIBNVME_DEFAULT_MAX_PROGRAMS   16
#define LIBNVME_DEFAULT_MAX_MEMORY_MB  256

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
                                       *  fabrics. */
};

/* ---------- helpers ---------- */

/* Strip optional "nvme:" prefix and namespace suffix: /dev/nvme0n1 → /dev/nvme0 */
static void
ctrl_path_from_uri(const char *uri, char *out, size_t out_sz)
{
	if (strncmp(uri, "nvme:", 5) == 0) uri += 5;

	strncpy(out, uri, out_sz - 1);
	out[out_sz - 1] = '\0';

	char *p = strstr(out, "n");
	while (p) {
		char *q = p + 1;
		bool all_digits = (*q != '\0');
		while (*q) { if (*q < '0' || *q > '9') { all_digits = false; break; } q++; }
		if (all_digits && q > p + 1) { *p = '\0'; break; }
		p = strstr(p + 1, "n");
	}
}

static csStatus
libnvme_admin_cmd(int fd, uint8_t opcode, uint32_t nsid,
		  void *buf, uint32_t buf_len,
		  uint32_t cdw2, uint32_t cdw3,
		  uint32_t cdw10, uint32_t cdw11,
		  uint32_t cdw12, uint32_t cdw13,
		  uint32_t cdw14, uint32_t cdw15,
		  uint32_t *result_out)
{
	uint32_t result = 0;
	int ret = nvme_admin_passthru(fd, opcode, 0, 0, nsid,
				      cdw2, cdw3, cdw10, cdw11,
				      cdw12, cdw13, cdw14, cdw15,
				      buf_len, buf, 0, NULL,
				      LIBNVME_TIMEOUT_ADMIN_MS, &result);
	if (ret < 0) return cs_map_errno(errno);
	if (ret != 0) return CS_STATUS_ERROR_IO;
	if (result_out) *result_out = result;
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_io_cmd(int fd, uint8_t opcode, uint32_t nsid,
	       void *buf, uint32_t buf_len,
	       uint32_t cdw2, uint32_t cdw3,
	       uint32_t cdw10, uint32_t cdw11,
	       uint32_t cdw12, uint32_t cdw13,
	       uint32_t cdw14, uint32_t cdw15,
	       uint32_t *result_out)
{
	uint32_t result = 0;
	/*
	 * nvme_io_passthru() tries NVME_IOCTL_IO64_CMD first (large LBA,
	 * required for some NVMe-oF targets), then falls back to
	 * NVME_IOCTL_IO_CMD — advantage over raw ioctl backend.
	 */
	int ret = nvme_io_passthru(fd, opcode, 0, 0, nsid,
				   cdw2, cdw3, cdw10, cdw11,
				   cdw12, cdw13, cdw14, cdw15,
				   buf_len, buf, 0, NULL,
				   LIBNVME_TIMEOUT_IO_MS, &result);
	if (ret < 0) return cs_map_errno(errno);
	if (ret != 0) return CS_STATUS_ERROR_IO;
	if (result_out) *result_out = result;
	return CS_STATUS_SUCCESS;
}

/* ---------- vtable implementations ---------- */

/* Backend init flag: written by backend init/shutdown (both invoked under the
 * library init_lock via the vtable), read as a re-entry guard only here. */
static bool g_initialized = false;

static csStatus
libnvme_backend_init(void)
{
	if (g_initialized) return CS_STATUS_SUCCESS;
	CS_LOG_INFO("libnvme backend initializing");
	g_initialized = true;
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_shutdown(void)
{
	g_initialized = false;
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_dev_query(csDeviceInfo *devices, uint32_t *count)
{
	if (!count) return CS_STATUS_ERROR_INVALID_PARAM;

	/*
	 * Primary: honour LIBCS_DEVICE_URI if set.
	 * Secondary: scan /sys/class/nvme/ and return all controllers
	 * (includes fabric-connected devices that appear after nvme-connect).
	 */
	const char *uri = getenv("LIBCS_DEVICE_URI");
	if (uri) {
		if (!devices) { *count = 1; return CS_STATUS_SUCCESS; }
		if (*count < 1) { *count = 1; return CS_STATUS_ERROR_LIMIT; }
		memset(&devices[0], 0, sizeof(csDeviceInfo));
		strncpy(devices[0].uri, uri, sizeof(devices[0].uri) - 1);
		strncpy(devices[0].vendor, "NVMe/libnvme", sizeof(devices[0].vendor) - 1);
		strncpy(devices[0].model, "CS Device", sizeof(devices[0].model) - 1);
		devices[0].max_programs = LIBNVME_DEFAULT_MAX_PROGRAMS;
		devices[0].max_memory_mb = LIBNVME_DEFAULT_MAX_MEMORY_MB;
		*count = 1;
		return CS_STATUS_SUCCESS;
	}

	/* Scan sysfs for NVMe controllers (local and fabric) */
	DIR *d = opendir("/sys/class/nvme");
	if (!d) { *count = 0; return CS_STATUS_SUCCESS; }

	uint32_t found = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;
		if (strncmp(ent->d_name, "nvme", 4) != 0) continue;

		if (devices && found < *count) {
			memset(&devices[found], 0, sizeof(csDeviceInfo));
			snprintf(devices[found].uri, sizeof(devices[found].uri),
				 "/dev/%s", ent->d_name);
			strncpy(devices[found].vendor, "NVMe/libnvme",
				sizeof(devices[found].vendor) - 1);
			strncpy(devices[found].model, "CS Device",
				sizeof(devices[found].model) - 1);
			devices[found].max_programs = LIBNVME_DEFAULT_MAX_PROGRAMS;
			devices[found].max_memory_mb = LIBNVME_DEFAULT_MAX_MEMORY_MB;
		}
		found++;
	}
	closedir(d);

	*count = found;
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_dev_open(const char *uri, struct cs_backend_ctx **backend_ctx)
{
	if (!uri || !backend_ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	*backend_ctx = NULL;

	struct cs_backend_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return CS_STATUS_ERROR_NO_MEMORY;

	ctrl_path_from_uri(uri, ctx->ctrl_path, sizeof(ctx->ctrl_path));

	ctx->ctrl_fd = nvme_open(ctx->ctrl_path);
	if (ctx->ctrl_fd < 0) {
		int err = errno;
		CS_LOG_ERROR("libnvme: open(%s) failed: %s", ctx->ctrl_path, strerror(err));
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

	/* Detect LBA size via Identify Namespace */
	struct nvme_id_ns ns_data;
	memset(&ns_data, 0, sizeof(ns_data));
	if (nvme_identify_ns(ctx->ctrl_fd, ctx->nvm_nsid, &ns_data) == 0) {
		uint8_t flbas = ns_data.flbas & 0x0f;
		uint8_t lbads = ns_data.lbaf[flbas].ds;
		if (lbads >= 9 && lbads <= 16) {
			ctx->lba_nbytes = 1u << lbads;
		}
	}

	*backend_ctx = ctx;
	CS_LOG_INFO("libnvme: opened %s (libcs_nsid=%u nvm_nsid=%u slm_nsid=%u lba=%u)",
		    ctx->ctrl_path, ctx->libcs_nsid, ctx->nvm_nsid, ctx->slm_nsid, ctx->lba_nbytes);
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_dev_close(struct cs_backend_ctx *ctx)
{
	if (!ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	if (ctx->ctrl_fd >= 0) close(ctx->ctrl_fd);
	free(ctx);
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_dev_get_info(struct cs_backend_ctx *ctx, csDeviceInfo *info)
{
	if (!ctx || !info) return CS_STATUS_ERROR_INVALID_PARAM;
	memset(info, 0, sizeof(*info));
	strncpy(info->uri, ctx->ctrl_path, sizeof(info->uri) - 1);
	strncpy(info->vendor, "NVMe/libnvme", sizeof(info->vendor) - 1);
	strncpy(info->model, "CS Device", sizeof(info->model) - 1);
	info->max_programs = LIBNVME_DEFAULT_MAX_PROGRAMS;
	info->max_memory_mb = LIBNVME_DEFAULT_MAX_MEMORY_MB;
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_mem_alloc(struct cs_backend_ctx *ctx, size_t size,
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
libnvme_backend_mem_free(struct cs_backend_ctx *ctx, void *ptr)
{
	(void)ctx;
	if (!ptr) return CS_STATUS_ERROR_INVALID_PARAM;
	free(ptr);
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_prog_load(struct cs_backend_ctx *ctx, uint16_t prog_index,
			  const void *program, size_t program_size)
{
	if (!ctx || !program || !program_size) return CS_STATUS_ERROR_INVALID_PARAM;

	void *buf = NULL;
	size_t padded = (program_size + CS_DMA_ALIGNMENT - 1) & ~(size_t)(CS_DMA_ALIGNMENT - 1);
	if (posix_memalign(&buf, CS_DMA_ALIGNMENT, padded)) return CS_STATUS_ERROR_NO_MEMORY;
	memcpy(buf, program, program_size);

	csStatus s = libnvme_admin_cmd(ctx->ctrl_fd, LIBNVME_OPC_CP_LOAD, ctx->libcs_nsid,
				       buf, (uint32_t)program_size,
				       0, 0,
				       (uint32_t)prog_index,
				       (uint32_t)program_size,
				       0, 0,
				       (uint32_t)program_size,
				       0,
				       NULL);
	free(buf);

	if (s != CS_STATUS_SUCCESS)
		CS_LOG_ERROR("libnvme: prog_load pind=%" PRIu16 " failed: %d", prog_index, s);
	else
		CS_LOG_INFO("libnvme: loaded program pind=%" PRIu16 " (%zu bytes)", prog_index, program_size);
	return s;
}

static csStatus
libnvme_backend_prog_activate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	csStatus s = libnvme_admin_cmd(ctx->ctrl_fd, LIBNVME_OPC_CP_ACTIVATE, ctx->libcs_nsid,
				       NULL, 0, 0, 0,
				       (uint32_t)prog_index,
				       0, 0, 0, 0, 0, NULL);
	if (s != CS_STATUS_SUCCESS)
		CS_LOG_ERROR("libnvme: prog_activate pind=%" PRIu16 " failed: %d", prog_index, s);
	else
		CS_LOG_INFO("libnvme: activated program pind=%" PRIu16 "", prog_index);
	return s;
}

static csStatus
libnvme_backend_prog_deactivate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) return CS_STATUS_ERROR_INVALID_PARAM;
	csStatus s = libnvme_admin_cmd(ctx->ctrl_fd, LIBNVME_OPC_CP_ACTIVATE, ctx->libcs_nsid,
				       NULL, 0, 0, 0,
				       (1u << 16) | (uint32_t)prog_index,
				       0, 0, 0, 0, 0, NULL);
	if (s != CS_STATUS_SUCCESS)
		CS_LOG_ERROR("libnvme: prog_deactivate pind=%" PRIu16 " failed: %d", prog_index, s);
	return s;
}

static csStatus
libnvme_backend_compute_execute(struct cs_backend_ctx *ctx, uint16_t prog_index,
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
		csStatus s = libnvme_io_cmd(ctx->ctrl_fd, LIBNVME_OPC_CP_EXECUTE,
					   ctx->libcs_nsid, buf, (uint32_t)input_size,
					   (uint32_t)prog_index, 0,
					   0, 0, 0, 0, 0, 0, &result);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("libnvme: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		if (output_size >= sizeof(uint64_t))
			*(uint64_t *)output = (uint64_t)result;
		else
			*(uint32_t *)output = result;
		if (completion) *completion = 1;
		CS_LOG_INFO("libnvme: compute_execute pind=%" PRIu16 " result=0x%x", prog_index, result);
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
		csStatus s = libnvme_io_cmd(ctx->ctrl_fd, LIBNVME_OPC_CP_EXECUTE,
					   ctx->libcs_nsid, buf, (uint32_t)input_size,
					   (uint32_t)prog_index, 0,
					   0, 0, 0, 0, 0, 0, &result);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("libnvme: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		memcpy(output, &result, sizeof(result));
		if (completion) *completion = 1;
		CS_LOG_INFO("libnvme: compute_execute pind=%" PRIu16 " result=0x%x", prog_index, result);
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
		csStatus s = libnvme_io_cmd(ctx->ctrl_fd, LIBNVME_OPC_CP_EXECUTE,
					   ctx->libcs_nsid, buf, (uint32_t)combined,
					   (uint32_t)prog_index, 0,
					   0, 0, 0, 0, 0, 0, &result);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("libnvme: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
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
		CS_LOG_INFO("libnvme: compute_execute pind=%" PRIu16 " (FILTER_GT)", prog_index);
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
		csStatus s = libnvme_io_cmd(ctx->ctrl_fd, LIBNVME_OPC_CP_EXECUTE,
					   ctx->libcs_nsid, buf, (uint32_t)combined,
					   (uint32_t)prog_index, 0,
					   0, 0, 0, 0, 0, 0, &cdw0);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("libnvme: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
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
		CS_LOG_INFO("libnvme: compute_execute pind=%" PRIu16 " (MULTI_AGG64)", prog_index);
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
		csStatus s = libnvme_io_cmd(ctx->ctrl_fd, LIBNVME_OPC_CP_EXECUTE,
					   ctx->libcs_nsid, buf, (uint32_t)combined,
					   (uint32_t)prog_index, 0,
					   0, 0, 0, 0, 0, 0, &cdw0);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("libnvme: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
			return s;
		}
		{
			size_t to_copy = cdw0 < input_size ? cdw0 : input_size;
			if (to_copy > output_size) to_copy = output_size;
			memcpy(output, input, to_copy);
		}
		if (completion) *completion = 1;
		CS_LOG_INFO("libnvme: compute_execute pind=%" PRIu16 " (MEMCPY_INLINE)", prog_index);
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
		csStatus s = libnvme_io_cmd(ctx->ctrl_fd, LIBNVME_OPC_CP_EXECUTE,
					   ctx->libcs_nsid, buf, (uint32_t)combined,
					   (uint32_t)prog_index, 0,
					   0, 0, 0, 0, 0, 0, &cdw0);
		free(buf);
		if (s != CS_STATUS_SUCCESS) {
			CS_LOG_ERROR("libnvme: compute_execute pind=%" PRIu16 " failed: %d", prog_index, s);
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
		CS_LOG_INFO("libnvme: compute_execute pind=%" PRIu16 " (RLE_COMPRESS)", prog_index);
		return CS_STATUS_SUCCESS;
	}

	default:
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}
}

static csStatus
libnvme_backend_storage_io(struct cs_backend_ctx *ctx, csStorageOp op,
			   uint64_t lba, uint32_t blocks,
			   void *buffer, size_t buffer_size,
			   volatile int *completion)
{
	if (!ctx || !buffer || !buffer_size || !blocks) return CS_STATUS_ERROR_INVALID_PARAM;
	(void)completion;

	uint8_t opcode = (op == CS_STORAGE_READ) ? LIBNVME_OPC_NVM_READ : LIBNVME_OPC_NVM_WRITE;

	csStatus s = libnvme_io_cmd(ctx->ctrl_fd, opcode, ctx->nvm_nsid,
				    buffer, (uint32_t)((size_t)blocks * ctx->lba_nbytes),
				    0, 0,
				    (uint32_t)lba,           /* cdw10: SLBA low */
				    (uint32_t)(lba >> 32),   /* cdw11: SLBA high */
				    blocks - 1,              /* cdw12: NLB (0-based) */
				    0, 0, 0,
				    NULL);
	if (s != CS_STATUS_SUCCESS)
		CS_LOG_ERROR("libnvme: storage_io op=%d lba=%lu blk=%u failed: %d",
			     op, lba, blocks, s);
	return s;
}

static int
libnvme_backend_poll(struct cs_backend_ctx *ctx, uint32_t max)
{
	/* Synchronous backend — all ops complete before returning */
	(void)ctx; (void)max;
	return 0;
}

static csStatus
libnvme_backend_mem_init(struct cs_backend_ctx *ctx, void *ptr,
			 uint8_t fill_value, size_t size)
{
	(void)ctx;
	if (!ptr) return CS_STATUS_ERROR_INVALID_PARAM;
	memset(ptr, fill_value, size);
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_prog_get_name(struct cs_backend_ctx *ctx, uint16_t prog_index,
			      char *name, size_t name_size)
{
	(void)ctx;
	if (!name || name_size == 0) return CS_STATUS_ERROR_INVALID_PARAM;
	const char *prog_name = cs_prog_name_from_pind(prog_index);
	if (!prog_name) return CS_STATUS_ERROR_NOT_FOUND;
	snprintf(name, name_size, "%s", prog_name);
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_query_csf_list(struct cs_backend_ctx *ctx, csCSFInfo *csfs, uint32_t *count)
{
	(void)ctx;
	if (!count) return CS_STATUS_ERROR_INVALID_PARAM;
	uint32_t total;
	const cs_csf_entry_t *tbl = cs_csf_table(&total);
	if (!csfs) { *count = total; return CS_STATUS_SUCCESS; }
	if (*count < total) { *count = total; return CS_STATUS_ERROR_LIMIT; }
	for (uint32_t i = 0; i < total; i++) {
		memset(&csfs[i], 0, sizeof(csfs[i]));
		snprintf(csfs[i].name, sizeof(csfs[i].name), "%s", tbl[i].name);
		csfs[i].version = 1;
	}
	*count = total;
	return CS_STATUS_SUCCESS;
}

static csStatus
libnvme_backend_compute_submit(struct cs_backend_ctx *ctx, uint16_t prog_index,
			       const void *input, size_t input_size,
			       void *output, size_t output_size,
			       const void *args, size_t args_size,
			       volatile int *completion)
{
	csStatus status = libnvme_backend_compute_execute(ctx, prog_index,
							  input, input_size,
							  output, output_size,
							  args, args_size, completion);
	if (status == CS_STATUS_SUCCESS && completion)
		return CS_STATUS_PENDING;
	return status;
}

static csStatus
libnvme_backend_compute_reap(struct cs_backend_ctx *ctx, volatile int *completion)
{
	(void)ctx;
	if (!completion) return CS_STATUS_ERROR_INVALID_PARAM;
	if (*completion == 0) return CS_STATUS_PENDING;
	return (*completion > 0) ? CS_STATUS_SUCCESS : CS_STATUS_ERROR_IO;
}

static csStatus
libnvme_backend_abort_request(struct cs_backend_ctx *ctx, volatile int *completion)
{
	(void)ctx;
	if (!completion) return CS_STATUS_ERROR_INVALID_PARAM;
	if (*completion == 0) { *completion = -1; return CS_STATUS_SUCCESS; }
	return CS_STATUS_ERROR_BUSY;
}

#endif /* __linux__ */

/* ---------- registration ---------- */

#include "cs_backend.h"

static const cs_backend_ops_t libnvme_backend_ops = {
	.name             = "libnvme",
	.init             = libnvme_backend_init,
	.shutdown         = libnvme_backend_shutdown,
	.dev_query        = libnvme_backend_dev_query,
	.dev_open         = libnvme_backend_dev_open,
	.dev_close        = libnvme_backend_dev_close,
	.dev_get_info     = libnvme_backend_dev_get_info,
	.mem_alloc        = libnvme_backend_mem_alloc,
	.mem_free         = libnvme_backend_mem_free,
	.mem_init         = libnvme_backend_mem_init,
	.prog_load        = libnvme_backend_prog_load,
	.prog_activate    = libnvme_backend_prog_activate,
	.prog_deactivate  = libnvme_backend_prog_deactivate,
	.prog_get_name    = libnvme_backend_prog_get_name,
	.query_csf_list   = libnvme_backend_query_csf_list,
	.compute_execute  = libnvme_backend_compute_execute,
	.compute_submit   = libnvme_backend_compute_submit,
	.compute_reap     = libnvme_backend_compute_reap,
	.abort_request    = libnvme_backend_abort_request,
	.storage_io       = libnvme_backend_storage_io,
	.poll             = libnvme_backend_poll,
};

void
backend_libnvme_register(void)
{
	csStatus s = cs_backend_register(&libnvme_backend_ops);
	if (s != CS_STATUS_SUCCESS && s != CS_STATUS_ERROR_ALREADY_EXISTS)
		CS_LOG_ERROR("Failed to register libnvme backend: %d", s);
}

#endif /* WITH_LIBNVME_BACKEND */
