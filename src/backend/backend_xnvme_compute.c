/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file backend_xnvme_compute.c
 * @brief xNVMe Backend: MRS and compute execution paths
 *
 * Split from backend_xnvme.c (mechanical move, no behavior change):
 * MRS create/delete/execute helpers and every compute path (MRS,
 * eval, inline/async execute, execute_nvm, submit/reap).
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
void
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
csStatus
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
void
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

csStatus
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

csStatus
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

csStatus
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

csStatus
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

csStatus
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

csStatus
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
