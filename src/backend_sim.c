/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file backend_sim.c
 * @brief In-process simulator backend for CS API
 */

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cs_internal.h"
#include "cs_programs.h"

#define SIM_URI_PREFIX "sim://"
#define SIM_DEFAULT_URI "sim://libcs0"
#define SIM_VENDOR "SimVendor"
#define SIM_MODEL "CS-Sim-v1"
#define SIM_MAX_PROGRAMS 32
#define SIM_MAX_MEMORY_MB 256
#define SIM_DEFAULT_ALIGNMENT 4096U

/* Dot-product wire protocol header prepended before float vectors */
#define LIBCS_DOT_MAGIC 0x43504353u
typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t elem_size;
	uint32_t dim;
	float    result;
	uint32_t flags;
} CsDotProductHeader;

struct sim_multi_agg64_result {
	uint64_t count;
	double sum;
	double min;
	double max;
};

struct sim_program_slot {
	void *program;
	size_t program_size;
	bool loaded;
	bool is_active;
};

struct sim_allocation {
	void *ptr;
	size_t size;
	struct sim_allocation *next;
};

/**
 * Simulator backend context
 */
struct cs_backend_ctx {
	char uri[256];
	struct sim_program_slot slots[SIM_MAX_PROGRAMS];
	struct sim_allocation *allocations;
	pthread_mutex_t lock;
};

/**
 * Simulator backend state
 */
static struct {
	bool initialized;  /* backend init flag: written by sim_backend_init/shutdown
	                   * (under the library init_lock), read as re-entry guard */
} sim_state = { .initialized = false };

static bool
sim_uri_is_valid(const char *uri)
{
	if (!uri) {
		return false;
	}

	size_t prefix_len = strlen(SIM_URI_PREFIX);
	return strncmp(uri, SIM_URI_PREFIX, prefix_len) == 0;
}

static csStatus
sim_backend_init(void)
{
	sim_state.initialized = true;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_shutdown(void)
{
	sim_state.initialized = false;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_dev_query(csDeviceInfo *devices, uint32_t *count)
{
	if (!count) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (!devices) {
		*count = 1;
		return CS_STATUS_SUCCESS;
	}

	if (*count < 1) {
		*count = 1;
		return CS_STATUS_ERROR_LIMIT;
	}

	memset(&devices[0], 0, sizeof(devices[0]));
	strncpy(devices[0].uri, SIM_DEFAULT_URI, sizeof(devices[0].uri) - 1);
	strncpy(devices[0].vendor, SIM_VENDOR, sizeof(devices[0].vendor) - 1);
	strncpy(devices[0].model, SIM_MODEL, sizeof(devices[0].model) - 1);
	devices[0].max_programs = SIM_MAX_PROGRAMS;
	devices[0].max_memory_mb = SIM_MAX_MEMORY_MB;

	*count = 1;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_dev_open(const char *uri, struct cs_backend_ctx **backend_ctx)
{
	if (!uri || !backend_ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (!sim_uri_is_valid(uri)) {
		return CS_STATUS_ERROR_NOT_FOUND;
	}

	*backend_ctx = NULL;

	struct cs_backend_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return CS_STATUS_ERROR_NO_MEMORY;
	}

	pthread_mutex_init(&ctx->lock, NULL);
	strncpy(ctx->uri, uri, sizeof(ctx->uri) - 1);
	ctx->uri[sizeof(ctx->uri) - 1] = '\0';

	*backend_ctx = ctx;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_dev_close(struct cs_backend_ctx *ctx)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&ctx->lock);

	for (size_t i = 0; i < SIM_MAX_PROGRAMS; i++) {
		free(ctx->slots[i].program);
		ctx->slots[i].program = NULL;
		ctx->slots[i].program_size = 0;
		ctx->slots[i].loaded = false;
		ctx->slots[i].is_active = false;
	}

	struct sim_allocation *node = ctx->allocations;
	while (node) {
		struct sim_allocation *next = node->next;
		free(node->ptr);
		free(node);
		node = next;
	}
	ctx->allocations = NULL;

	pthread_mutex_unlock(&ctx->lock);
	pthread_mutex_destroy(&ctx->lock);
	free(ctx);
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_dev_get_info(struct cs_backend_ctx *ctx, csDeviceInfo *info)
{
	if (!ctx || !info) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	memset(info, 0, sizeof(*info));
	snprintf(info->uri, sizeof(info->uri), "%s", ctx->uri[0] ? ctx->uri : SIM_DEFAULT_URI);
	strncpy(info->vendor, SIM_VENDOR, sizeof(info->vendor) - 1);
	strncpy(info->model, SIM_MODEL, sizeof(info->model) - 1);
	info->max_programs = SIM_MAX_PROGRAMS;
	info->max_memory_mb = SIM_MAX_MEMORY_MB;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_query_device_properties(struct cs_backend_ctx *ctx,
				     csDeviceProperties *props)
{
	if (!ctx || !props) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	memset(props, 0, sizeof(*props));
	props->num_cses = 1;
	props->num_csees = 1;
	props->fdm_total_bytes = 0;
	props->afdm_total_bytes = (size_t)SIM_MAX_MEMORY_MB * 1024 * 1024;
	props->max_concurrent_progs = SIM_MAX_PROGRAMS;
	props->csf_count = 15;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_query_device_statistics(struct cs_backend_ctx *ctx,
				    csDeviceStatistics *stats)
{
	if (!ctx || !stats) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	memset(stats, 0, sizeof(*stats));
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_device_config(struct cs_backend_ctx *ctx, uint32_t config_id,
			  const void *data, size_t size)
{
	if (!ctx || !data || size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	(void)config_id;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_device_reset(struct cs_backend_ctx *ctx, uint32_t reset_type)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	(void)reset_type;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_mem_alloc(struct cs_backend_ctx *ctx, size_t size,
		      uint32_t alignment, void **ptr)
{
	if (!ctx || !ptr || size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	size_t effective_alignment = alignment ? alignment : SIM_DEFAULT_ALIGNMENT;
	if (effective_alignment < SIM_DEFAULT_ALIGNMENT) {
		effective_alignment = SIM_DEFAULT_ALIGNMENT;
	}

	if ((effective_alignment & (effective_alignment - 1)) != 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	void *buffer = NULL;
	int ret = posix_memalign(&buffer, effective_alignment, size);
	if (ret != 0) {
		return cs_map_errno(ret == ENOMEM ? ENOMEM : EINVAL);
	}
	memset(buffer, 0, size);

	struct sim_allocation *node = calloc(1, sizeof(*node));
	if (!node) {
		free(buffer);
		return CS_STATUS_ERROR_NO_MEMORY;
	}

	node->ptr = buffer;
	node->size = size;

	pthread_mutex_lock(&ctx->lock);
	node->next = ctx->allocations;
	ctx->allocations = node;
	pthread_mutex_unlock(&ctx->lock);

	*ptr = buffer;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_mem_free(struct cs_backend_ctx *ctx, void *ptr)
{
	if (!ctx || !ptr) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&ctx->lock);

	struct sim_allocation **cursor = &ctx->allocations;
	while (*cursor) {
		if ((*cursor)->ptr == ptr) {
			struct sim_allocation *node = *cursor;
			*cursor = node->next;
			free(node->ptr);
			free(node);
			pthread_mutex_unlock(&ctx->lock);
			return CS_STATUS_SUCCESS;
		}
		cursor = &(*cursor)->next;
	}

	pthread_mutex_unlock(&ctx->lock);
	return CS_STATUS_ERROR_INVALID_PARAM;
}

static csStatus
sim_backend_prog_load(struct cs_backend_ctx *ctx, uint16_t prog_index,
		      const void *program, size_t program_size)
{
	if (!ctx || !program || program_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	if (prog_index >= SIM_MAX_PROGRAMS) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	void *program_copy = malloc(program_size);
	if (!program_copy) {
		return CS_STATUS_ERROR_NO_MEMORY;
	}
	memcpy(program_copy, program, program_size);

	pthread_mutex_lock(&ctx->lock);
	free(ctx->slots[prog_index].program);
	ctx->slots[prog_index].program = program_copy;
	ctx->slots[prog_index].program_size = program_size;
	ctx->slots[prog_index].loaded = true;
	ctx->slots[prog_index].is_active = false;
	pthread_mutex_unlock(&ctx->lock);

	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_prog_activate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	if (prog_index >= SIM_MAX_PROGRAMS) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&ctx->lock);
	if (!ctx->slots[prog_index].loaded) {
		pthread_mutex_unlock(&ctx->lock);
		return CS_STATUS_ERROR_NOT_FOUND;
	}
	ctx->slots[prog_index].is_active = true;
	pthread_mutex_unlock(&ctx->lock);
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_prog_deactivate(struct cs_backend_ctx *ctx, uint16_t prog_index)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	if (prog_index >= SIM_MAX_PROGRAMS) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	pthread_mutex_lock(&ctx->lock);
	if (!ctx->slots[prog_index].loaded) {
		pthread_mutex_unlock(&ctx->lock);
		return CS_STATUS_ERROR_NOT_FOUND;
	}
	ctx->slots[prog_index].is_active = false;
	pthread_mutex_unlock(&ctx->lock);
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_prog_get_name(struct cs_backend_ctx *ctx, uint16_t prog_index,
			  char *name, size_t name_size)
{
	if (!ctx || !name || name_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	const char *prog_name = cs_prog_name_from_pind(prog_index);
	if (!prog_name) {
		return CS_STATUS_ERROR_NOT_FOUND;
	}
	snprintf(name, name_size, "%s", prog_name);
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_query_csf_list(struct cs_backend_ctx *ctx, csCSFInfo *csfs, uint32_t *count)
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
sim_backend_mem_init(struct cs_backend_ctx *ctx, void *ptr,
		     uint8_t fill_value, size_t size)
{
	(void)ctx;
	if (!ptr) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	memset(ptr, fill_value, size);
	return CS_STATUS_SUCCESS;
}

static int
sim_get_latency_us(void)
{
	static int latency_us = -1;
	if (latency_us < 0) {
		uint32_t v = 0;
		latency_us = cs_parse_u32_env("LIBCS_SIM_LATENCY_US", &v) ? (int)v : 0;
	}
	return latency_us;
}

static csStatus
sim_backend_compute_execute(struct cs_backend_ctx *ctx, uint16_t prog_index,
			    const void *input, size_t input_size,
			    void *output, size_t output_size,
			    const void *args, size_t args_size,
			    volatile int *completion)
{
	if (!ctx) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	(void)ctx;

	/* Simulate storage read latency before executing the compute kernel. */
	int latency_us = sim_get_latency_us();
	if (latency_us > 0) {
		usleep((useconds_t)latency_us);
	}

	switch (prog_index) {
	case LIBCS_PIND_SUM64:
	case LIBCS_PIND_MAX64:
	case LIBCS_PIND_MIN64: {
		if (!input || !output) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		if (output_size < sizeof(uint64_t) || input_size == 0 || (input_size % sizeof(uint64_t)) != 0) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}

		const uint64_t *in = (const uint64_t *)input;
		size_t count = input_size / sizeof(uint64_t);
		uint64_t result = in[0];

		if (prog_index == LIBCS_PIND_SUM64) {
			result = 0;
			for (size_t i = 0; i < count; i++) {
				result += in[i];
			}
		} else if (prog_index == LIBCS_PIND_MAX64) {
			for (size_t i = 1; i < count; i++) {
				if (in[i] > result) {
					result = in[i];
				}
			}
		} else {
			for (size_t i = 1; i < count; i++) {
				if (in[i] < result) {
					result = in[i];
				}
			}
		}

		memcpy(output, &result, sizeof(result));
		break;
	}
	case LIBCS_PIND_DOT_PRODUCT: {
		if (!input || !output) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}

		/* Check for dot-product wire protocol header (magic = 0x43504353) */
		if (input_size >= sizeof(CsDotProductHeader) &&
		    ((const CsDotProductHeader *)input)->magic == LIBCS_DOT_MAGIC) {
			const CsDotProductHeader *hdr = (const CsDotProductHeader *)input;
			uint32_t dim = hdr->dim;
			const float *vecs = (const float *)((const uint8_t *)input +
							     sizeof(CsDotProductHeader));
			size_t expected = sizeof(CsDotProductHeader) + 2u * dim * sizeof(float);
			if (input_size < expected || output_size < sizeof(CsDotProductHeader)) {
				return CS_STATUS_ERROR_INVALID_PARAM;
			}
			float sum = 0.0f;
			for (uint32_t i = 0; i < dim; i++) {
				sum += vecs[i] * vecs[i + dim];
			}
			((CsDotProductHeader *)output)->result = sum;
			break;
		}

		/* Legacy path: raw float pairs */
		if (output_size < sizeof(float)) {
			return CS_STATUS_ERROR_LIMIT;
		}
		if (input_size == 0 || (input_size % (2 * sizeof(float))) != 0) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}

		const float *in = (const float *)input;
		size_t total_floats = input_size / sizeof(float);
		size_t half = total_floats / 2;
		float sum = 0.0f;
		for (size_t i = 0; i < half; i++) {
			sum += in[i] * in[i + half];
		}
		memcpy(output, &sum, sizeof(sum));
		break;
	}
	case LIBCS_PIND_FILTER_GT: {
		if (!input || !output || !args || args_size < sizeof(float)) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		if (input_size % sizeof(float) != 0) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}

		float threshold = *(const float *)args;
		const float *in = (const float *)input;
		float *out = (float *)output;
		size_t in_count = input_size / sizeof(float);
		size_t out_capacity = output_size / sizeof(float);
		size_t out_count = 0;

		for (size_t i = 0; i < in_count; i++) {
			if (in[i] > threshold) {
				if (out_count >= out_capacity) {
					return CS_STATUS_ERROR_LIMIT;
				}
				out[out_count++] = in[i];
			}
		}
		for (size_t i = out_count; i < out_capacity; i++) {
			out[i] = NAN;
		}
		break;
	}
	case LIBCS_PIND_MEMCPY:
		if (!input || !output) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		if (output_size < input_size) {
			return CS_STATUS_ERROR_LIMIT;
		}
		memcpy(output, input, input_size);
		break;
	case LIBCS_PIND_RLE_COMPRESS: {
		if (!input || !output || input_size == 0) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}

		const uint8_t *src = (const uint8_t *)input;
		uint8_t *dst = (uint8_t *)output;
		size_t out_pos = 0;
		size_t i = 0;

		while (i < input_size) {
			uint8_t value = src[i];
			uint8_t run = 1;
			while ((i + run) < input_size && src[i + run] == value && run < 255) {
				run++;
			}

			if (out_pos + 2 > output_size) {
				return CS_STATUS_ERROR_LIMIT;
			}

			dst[out_pos++] = run;
			dst[out_pos++] = value;
			i += run;
		}
		break;
	}
	case LIBCS_PIND_MULTI_AGG64: {
		if (!input || !output) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		if (output_size < sizeof(struct sim_multi_agg64_result) || input_size == 0 ||
		    (input_size % sizeof(double)) != 0) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}

		const double *in = (const double *)input;
		size_t count = input_size / sizeof(double);
		struct sim_multi_agg64_result result = {
			.count = count,
			.sum = 0.0,
			.min = in[0],
			.max = in[0],
		};

		for (size_t i = 0; i < count; i++) {
			double value = in[i];
			result.sum += value;
			if (value < result.min) {
				result.min = value;
			}
			if (value > result.max) {
				result.max = value;
			}
		}

		memcpy(output, &result, sizeof(result));
		break;
	}
	case LIBCS_PIND_L2_DISTANCE_SQ:
	case LIBCS_PIND_COSINE_SIMILARITY: {
		if (!input || !output) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}
		if (output_size < sizeof(float) || input_size == 0 || (input_size % (2 * sizeof(float))) != 0) {
			return CS_STATUS_ERROR_INVALID_PARAM;
		}

		const float *in = (const float *)input;
		size_t total_floats = input_size / sizeof(float);
		size_t half = total_floats / 2;
		float result = 0.0f;

		if (prog_index == LIBCS_PIND_L2_DISTANCE_SQ) {
			for (size_t i = 0; i < half; i++) {
				float diff = in[i] - in[i + half];
				result += diff * diff;
			}
		} else {
			float dot = 0.0f;
			float lhs_norm = 0.0f;
			float rhs_norm = 0.0f;

			for (size_t i = 0; i < half; i++) {
				float lhs = in[i];
				float rhs = in[i + half];
				dot += lhs * rhs;
				lhs_norm += lhs * lhs;
				rhs_norm += rhs * rhs;
			}

			float denom = sqrtf(lhs_norm * rhs_norm);
			result = (denom > 0.0f) ? (dot / denom) : 0.0f;
			if (result < -1.0f) {
				result = -1.0f;
			} else if (result > 1.0f) {
				result = 1.0f;
			}
		}

		memcpy(output, &result, sizeof(result));
		break;
	}
	default:
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	if (completion) {
		*completion = 1;
	}

	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_compute_submit(struct cs_backend_ctx *ctx, uint16_t prog_index,
			   const void *input, size_t input_size,
			   void *output, size_t output_size,
			   const void *args, size_t args_size,
			   volatile int *completion)
{
	csStatus status = sim_backend_compute_execute(ctx, prog_index,
						      input, input_size,
						      output, output_size,
						      args, args_size, completion);
	if (status == CS_STATUS_SUCCESS && completion) {
		return CS_STATUS_PENDING;
	}
	return status;
}

static csStatus
sim_backend_compute_reap(struct cs_backend_ctx *ctx, volatile int *completion)
{
	(void)ctx;
	if (!completion) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	if (*completion == 0) {
		return CS_STATUS_PENDING;
	}
	return (*completion > 0) ? CS_STATUS_SUCCESS : CS_STATUS_ERROR_IO;
}

static csStatus
sim_backend_abort_request(struct cs_backend_ctx *ctx, volatile int *completion)
{
	(void)ctx;
	if (!completion) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}
	*completion = -1;
	return CS_STATUS_SUCCESS;
}

static csStatus
sim_backend_storage_io(struct cs_backend_ctx *ctx, csStorageOp op,
		       uint64_t lba, uint32_t blocks,
		       void *buffer, size_t buffer_size,
		       volatile int *completion)
{
	(void)ctx;
	(void)op;
	(void)lba;
	(void)blocks;
	(void)buffer;
	(void)buffer_size;
	(void)completion;
	return CS_STATUS_ERROR_NOT_SUPPORTED;
}

static int
sim_backend_poll(struct cs_backend_ctx *ctx, uint32_t max)
{
	(void)ctx;
	(void)max;
	return 0;
}

static csStatus
sim_backend_batch_submit(struct cs_backend_ctx *ctx, csBatchHandle batch,
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

	/* ponytail: sequential submit — replace with native batch when the
	 * simulator gains multi-CSF parallel execution. */
	for (uint32_t i = 0; i < bctx->entry_count; i++) {
		const csRequest *req = &bctx->entries[i];
		volatile int entry_comp = 0;

		switch (req->type) {
		case CS_REQ_TYPE_COMPUTE: {
			uint16_t pind = cs_prog_index_from_csf(
				req->u.compute.desc.csf_id);
			status = sim_backend_compute_execute(ctx, pind,
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
			status = sim_backend_storage_io(ctx,
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

static const cs_backend_ops_t sim_backend_ops = {
	.name = "sim",

	.init = sim_backend_init,
	.shutdown = sim_backend_shutdown,

	.dev_query = sim_backend_dev_query,
	.dev_open = sim_backend_dev_open,
	.dev_close = sim_backend_dev_close,
	.dev_get_info = sim_backend_dev_get_info,

	.mem_alloc = sim_backend_mem_alloc,
	.mem_free = sim_backend_mem_free,
	.mem_init = sim_backend_mem_init,

	.prog_load = sim_backend_prog_load,
	.prog_activate = sim_backend_prog_activate,
	.prog_deactivate = sim_backend_prog_deactivate,
	.prog_get_name = sim_backend_prog_get_name,
	.query_csf_list = sim_backend_query_csf_list,

	.compute_execute = sim_backend_compute_execute,
	.compute_submit = sim_backend_compute_submit,
	.compute_reap = sim_backend_compute_reap,
	.abort_request = sim_backend_abort_request,
	.storage_io = sim_backend_storage_io,
	.poll = sim_backend_poll,

	.batch_submit = sim_backend_batch_submit,

	.query_device_properties = sim_backend_query_device_properties,
	.query_device_statistics = sim_backend_query_device_statistics,
	.device_config = sim_backend_device_config,
	.device_reset = sim_backend_device_reset,
};

void
backend_sim_register(void)
{
	csStatus status = cs_backend_register(&sim_backend_ops);
	if (status != CS_STATUS_SUCCESS && status != CS_STATUS_ERROR_ALREADY_EXISTS) {
		CS_LOG_ERROR("Failed to register sim backend: %d", status);
	}
}
