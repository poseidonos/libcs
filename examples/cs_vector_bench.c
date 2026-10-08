/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_vector_bench.c
 * @brief Vector similarity search benchmark for CPCS.
 *
 * Models a RAG embedding lookup: find the most similar embeddings in a
 * dataset by dot-product similarity.  Demonstrates the data-movement
 * advantage of computational storage for AI workloads.
 *
 * Paths:
 *   CPU     — read entire file → compute N dot products on host → top-K
 *   CSD_SIM — embeddings pre-staged; per-embedding DOT_PRODUCT via sim backend
 *   CSD     — same as CSD_SIM on real hardware (--uri required)
 *
 * File format: raw float32 binary, N_VECS × DIM floats.
 *              The first vector (index 0) is used as the query.
 *
 * Usage:
 *   cs_vector_bench --path <CPU|CSD_SIM|CSD>
 *                   --file <embeddings.bin>
 *                   [--uri <device_uri>]
 *                   --n-vecs <N>  --dim <D>
 *                   [--top-k <K>]  [--iterations <N>]
 *
 * Output (stdout, parsed by 10_vector_bench.sh):
 *   elapsed_ms=<float>
 *   read_ms=<float>              (Local CPU path only)
 *   compute_ms=<float>
 *   data_moved_bytes=<N>         bytes transferred from storage/fabric
 *   result_bytes=<N>
 *   reduction_ratio=<float>      actual data_moved / result_bytes
 *   ideal_reduction_ratio=<float> theoretical for device-side batch CSF
 *   top1_idx=<N>
 *   top1_score=<float>
 *
 * Exit codes: 0 success, 1 usage error, 2 runtime error.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cs_helpers.h"

#define PATH_CPU     0
#define PATH_CSD_SIM 1
#define PATH_CSD     2

/* URI used when no real device is present (sim backend via LIBCS_BACKEND=sim) */
#define SIM_URI "sim://"

static uint64_t now_ns(void)
{
	struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int read_embeddings(const char *filepath, float *buf, size_t n_floats)
{
	int fd = open(filepath, O_RDONLY);
	if (fd < 0) { perror("open"); return -1; }

	size_t remaining = n_floats * sizeof(float);
	char  *p = (char *)buf;
	while (remaining > 0) {
		ssize_t n = read(fd, p, remaining);
		if (n <= 0) {
			if (n < 0) perror("read");
			else fprintf(stderr, "short read: %zu bytes remaining\n",
				     remaining);
			close(fd);
			return -1;
		}
		p += n;
		remaining -= (size_t)n;
	}
	close(fd);
	return 0;
}

static void print_analysis(uint32_t n_vecs, uint32_t dim, uint32_t top_k)
{
	size_t cpu_bytes = (size_t)n_vecs * dim * sizeof(float);
	/* Ideal CSD: host sends one query, device reads its own NVM, returns N scores */
	size_t ideal_in  = (size_t)dim * sizeof(float);
	size_t ideal_out = (size_t)n_vecs * sizeof(double);
	double ideal_ratio = (double)cpu_bytes / (double)(ideal_in + ideal_out);

	fprintf(stderr,
	        "[vector_bench] Data movement analysis (n_vecs=%u dim=%u top_k=%u):\n"
	        "  Local CPU path:         %zu bytes from storage (%.2f MB)\n"
	        "    Ideal CSD (device-side batch): %zu query + %zu scores = %.1f KB\n"
	        "  Ideal fabric reduction:      %.0f×\n",
	        n_vecs, dim, top_k,
	        cpu_bytes, (double)cpu_bytes / (1024.0 * 1024.0),
	        ideal_in, ideal_out, (double)(ideal_in + ideal_out) / 1024.0,
	        ideal_ratio);

	printf("ideal_reduction_ratio=%.1f\n", ideal_ratio);
}

/* Local CPU path: read entire file, compute N-1 dot products, find top-K */
static int run_cpu(const char *filepath, uint32_t n_vecs, uint32_t dim,
                   uint32_t top_k, int iterations)
{
	size_t n_floats   = (size_t)n_vecs * dim;
	size_t data_bytes = n_floats * sizeof(float);
	size_t result_b   = (size_t)top_k * (sizeof(double) + sizeof(uint32_t));

	float  *buf    = malloc(data_bytes);
	double *scores = malloc((size_t)n_vecs * sizeof(double));
	if (!buf || !scores) {
		fprintf(stderr, "malloc failed\n");
		free(buf); free(scores);
		return 2;
	}

	for (int it = 0; it < iterations; it++) {
		uint64_t t_start      = now_ns();
		uint64_t t_read_start = now_ns();

		if (read_embeddings(filepath, buf, n_floats) != 0) {
			free(buf); free(scores);
			return 2;
		}
		double read_ms = (double)(now_ns() - t_read_start) / 1e6;

		const float *query = buf;

		uint64_t t_compute_start = now_ns();
		for (uint32_t i = 1; i < n_vecs; i++) {
			const float *emb = buf + (size_t)i * dim;
			double dp = 0.0;
			for (uint32_t d = 0; d < dim; d++)
				dp += (double)query[d] * (double)emb[d];
			scores[i] = dp;
		}
		scores[0] = -1e300; /* exclude the query itself */
		double compute_ms = (double)(now_ns() - t_compute_start) / 1e6;
		double elapsed_ms = (double)(now_ns() - t_start) / 1e6;

		uint32_t top1_idx   = 1;
		double   top1_score = scores[1];
		for (uint32_t i = 2; i < n_vecs; i++) {
			if (scores[i] > top1_score) {
				top1_score = scores[i];
				top1_idx   = i;
			}
		}

		double reduction = (double)data_bytes / (double)result_b;

		if (iterations <= 1) {
			printf("elapsed_ms=%.4f\n",      elapsed_ms);
			printf("read_ms=%.4f\n",         read_ms);
			printf("compute_ms=%.4f\n",      compute_ms);
			printf("data_moved_bytes=%zu\n", data_bytes);
			printf("result_bytes=%zu\n",     result_b);
			printf("reduction_ratio=%.1f\n", reduction);
			printf("top1_idx=%u\n",          top1_idx);
			printf("top1_score=%.6f\n",      top1_score);
		} else {
			printf("iter=%d elapsed_ms=%.4f read_ms=%.4f compute_ms=%.4f"
			       " top1_idx=%u top1_score=%.6f\n",
			       it + 1, elapsed_ms, read_ms, compute_ms,
			       top1_idx, top1_score);
		}
	}

	free(buf); free(scores);
	return 0;
}

/*
 * CSD path: embeddings pre-loaded into host memory (staging cost excluded),
 * then per-embedding DOT_PRODUCT issued via csRunComputeExCached.
 *
 * Input to each CSF call: [query[dim], embedding_i[dim]] as concatenated float32.
 * Output: 8 bytes (double dot-product result).
 *
 * This models the SLM-staged scenario: embeddings already live on the CSD;
 * the host sends only the query + embedding pair descriptor per call.
 * The ideal_reduction_ratio column shows the savings a future batch CSF
 * (CS_CSF_BATCH_DOT_PRODUCT) would achieve by scanning NVM directly.
 */
static int run_csd(const char *filepath, const char *uri,
                   uint32_t n_vecs, uint32_t dim,
                   uint32_t top_k, int iterations)
{
	size_t pair_floats = (size_t)dim * 2;
	size_t pair_bytes  = pair_floats * sizeof(float);
	size_t data_bytes  = (size_t)n_vecs * dim * sizeof(float);
	size_t result_b    = (size_t)top_k * (sizeof(double) + sizeof(uint32_t));

	float  *embeddings = malloc(data_bytes);
	float  *pair       = malloc(pair_bytes);
	double *scores     = malloc((size_t)n_vecs * sizeof(double));
	if (!embeddings || !pair || !scores) {
		fprintf(stderr, "malloc failed\n");
		free(embeddings); free(pair); free(scores);
		return 2;
	}

	/* Read embeddings once (staging cost, excluded from timed loop) */
	if (read_embeddings(filepath, embeddings, (size_t)n_vecs * dim) != 0) {
		free(embeddings); free(pair); free(scores);
		return 2;
	}

	const char *dev_uri = uri ? uri : SIM_URI;

	for (int it = 0; it < iterations; it++) {
		const float *query = embeddings;
		memcpy(pair, query, (size_t)dim * sizeof(float));

		uint64_t t_start = now_ns();

		for (uint32_t i = 1; i < n_vecs; i++) {
			const float *emb = embeddings + (size_t)i * dim;
			memcpy(pair + dim, emb, (size_t)dim * sizeof(float));

			uint64_t       result_bits = 0;
			csComputeResult cr         = {0};
			csStatus        st = csRunComputeExCached(
				dev_uri, CS_CSF_DOT_PRODUCT,
				pair, pair_bytes,
				&result_bits, sizeof(result_bits),
				NULL, 0, &cr);

			if (st != CS_STATUS_SUCCESS) {
				fprintf(stderr,
				        "csRunComputeExCached failed i=%u: %s (%d)\n",
				        i, csStatusToString(st), (int)st);
				csRunComputeExCachedCleanup();
				free(embeddings); free(pair); free(scores);
				return 2;
			}

			double score;
			memcpy(&score, &result_bits, sizeof(double));
			scores[i] = score;
		}
		scores[0] = -1e300;

		double elapsed_ms = (double)(now_ns() - t_start) / 1e6;

		uint32_t top1_idx   = 1;
		double   top1_score = scores[1];
		for (uint32_t i = 2; i < n_vecs; i++) {
			if (scores[i] > top1_score) {
				top1_score = scores[i];
				top1_idx   = i;
			}
		}

		/* Bytes sent to device: pair_bytes per embedding call.
		 * Bytes received: 8B per call.  CPU comparison: data_bytes. */
		size_t bytes_sent = (size_t)(n_vecs - 1) * pair_bytes;
		size_t bytes_recv = (size_t)(n_vecs - 1) * sizeof(double);
		size_t total_csd  = bytes_sent + bytes_recv;
		double reduction  = (double)data_bytes / (double)result_b;

		if (iterations <= 1) {
			printf("elapsed_ms=%.4f\n",       elapsed_ms);
			printf("compute_ms=%.4f\n",       elapsed_ms);
			printf("data_moved_bytes=%zu\n",  total_csd);
			printf("result_bytes=%zu\n",      result_b);
			printf("reduction_ratio=%.1f\n",  reduction);
			printf("top1_idx=%u\n",           top1_idx);
			printf("top1_score=%.6f\n",       top1_score);
		} else {
			printf("iter=%d elapsed_ms=%.4f top1_idx=%u top1_score=%.6f\n",
			       it + 1, elapsed_ms, top1_idx, top1_score);
		}
	}

	csRunComputeExCachedCleanup();
	free(embeddings); free(pair); free(scores);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
	        "Usage: %s --path <CPU|CSD_SIM|CSD>\n"
	        "          --file <embeddings.bin>\n"
	        "          [--uri <device_uri>]   (CSD path; default: sim://)\n"
	        "          --n-vecs <N>  --dim <D>\n"
	        "          [--top-k <K>]          (default: 1)\n"
	        "          [--iterations <N>]     (default: 1)\n"
	        "\n"
	        "File format: raw float32, N_VECS × DIM floats.\n"
	        "             Vector 0 is the query; vectors 1..N-1 are the database.\n"
	        "\n"
	        "Paths:\n"
	        "  CPU      read file + compute N dot products on host\n"
	        "  CSD_SIM  per-embedding DOT_PRODUCT via sim backend (LIBCS_BACKEND=sim)\n"
	        "  CSD      per-embedding DOT_PRODUCT via real device (--uri required)\n",
	        prog);
}

int main(int argc, char *argv[])
{
	const char *filepath   = NULL;
	const char *uri        = NULL;
	uint32_t    n_vecs     = 0;
	uint32_t    dim        = 0;
	uint32_t    top_k      = 1;
	int         path       = -1;
	int         iterations = 1;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
			const char *p = argv[++i];
			if (strcasecmp(p, "CPU") == 0)
				path = PATH_CPU;
			else if (strcasecmp(p, "CSD_SIM") == 0)
				path = PATH_CSD_SIM;
			else if (strcasecmp(p, "CSD") == 0)
				path = PATH_CSD;
			else {
				fprintf(stderr, "Unknown path: %s\n", p);
				usage(argv[0]); return 1;
			}
		} else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
			filepath = argv[++i];
		} else if (strcmp(argv[i], "--uri") == 0 && i + 1 < argc) {
			uri = argv[++i];
		} else if (strcmp(argv[i], "--n-vecs") == 0 && i + 1 < argc) {
			n_vecs = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--dim") == 0 && i + 1 < argc) {
			dim = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--top-k") == 0 && i + 1 < argc) {
			top_k = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
			iterations = (int)strtol(argv[++i], NULL, 0);
			if (iterations < 1) iterations = 1;
		} else if (strcmp(argv[i], "--help") == 0 ||
			   strcmp(argv[i], "-h") == 0) {
			usage(argv[0]); return 0;
		} else {
			fprintf(stderr, "Unknown argument: %s\n", argv[i]);
			usage(argv[0]); return 1;
		}
	}

	if (path < 0) {
		fprintf(stderr, "Missing required: --path\n");
		usage(argv[0]); return 1;
	}
	if (!filepath) {
		fprintf(stderr, "Missing required: --file\n");
		usage(argv[0]); return 1;
	}
	if (n_vecs < 2) {
		fprintf(stderr, "--n-vecs must be >= 2 (first vector is the query)\n");
		return 1;
	}
	if (dim == 0) {
		fprintf(stderr, "Missing required: --dim\n");
		usage(argv[0]); return 1;
	}
	if (path == PATH_CSD && !uri) {
		fprintf(stderr, "CSD path requires --uri\n");
		usage(argv[0]); return 1;
	}
	if (top_k >= n_vecs)
		top_k = n_vecs - 1;

	fprintf(stderr,
	        "[vector_bench] path=%s file=%s n_vecs=%u dim=%u"
	        " top_k=%u iterations=%d\n",
	        path == PATH_CPU ? "CPU"
	        : (path == PATH_CSD_SIM ? "CSD_SIM" : "CSD"),
	        filepath, n_vecs, dim, top_k, iterations);

	print_analysis(n_vecs, dim, top_k);

	if (path == PATH_CPU)
		return run_cpu(filepath, n_vecs, dim, top_k, iterations);
	else
		return run_csd(filepath, uri, n_vecs, dim, top_k, iterations);
}
