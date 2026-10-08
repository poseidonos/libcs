/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file backend_xnvme_slm.c
 * @brief xNVMe Backend: SLM staging, readback, regions, storage I/O
 *
 * Split from backend_xnvme.c (mechanical move, no behavior change):
 * SLM staging and readback machinery, SLM region I/O, NVM-backed
 * storage I/O, and the poll loop.
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

/* ------------------------------------------------------------------ */
/*  SLM / MRS helpers for near-data compute path                      */
/* ------------------------------------------------------------------ */

/**
 * Write host data to NVM namespace starting at LBA 0.
 */
csStatus
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

csStatus
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
csStatus
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

void
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
csStatus
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
csStatus
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
csStatus
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
csStatus
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

csStatus
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

int
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
