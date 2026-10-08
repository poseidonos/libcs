/*
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bench_compute_from_nvm.c — benchmark csRunComputeFromNvm vs csRunComputeExCached
 *
 * Measures three paths:
 *   1. nvm_to_slm:  csRunComputeFromNvm   (NVM->SLM device copy + MRS, no H2D)
 *   2. h2d:         csRunComputeExCached  (H2D host->SLM + MRS, every iter)
 *   3. nvm_cached:  csRunComputeExCached  (NVM->SLM + MRS, skip H2D after first)
 *
 * Build on .28:
 *   gcc -O2 -Wall -Ilibcs/include \
 *       libcs/tests/bench_compute_from_nvm.c \
 *       -Llibcs/build -lcs -Lxnvme/build/lib -lxnvme \
 *       -lm -lpthread -o bench_compute_from_nvm
 *
 * Run on .28 (target on .24):
 *   LIBCS_XNVME_BE=spdk LIBCS_XNVME_SUBNQN=nqn.2024-01.io.example:libcs \
 *   LIBCS_XNVME_NSID=2 LIBCS_XNVME_SHM_ID=0 LIBCS_XNVME_SLM_NSID=100 \
 *   LD_LIBRARY_PATH=libcs/build:xnvme/build/lib \
 *   ./bench_compute_from_nvm --uri 192.0.2.1:4420 \
 *     --csf SUM64 --len 1048576 --iterations 5 --warmup 1
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cs_api.h"
#include "cs_helpers.h"

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const struct {
	const char *name;
	csCSFId id;
	int is_vector; /* 0=scalar (uint64), 1=vector (float pairs) */
} csf_table[] = {
	{ "SUM64",            CS_CSF_SUM64,            0 },
	{ "MAX64",            CS_CSF_MAX64,            0 },
	{ "MIN64",            CS_CSF_MIN64,            0 },
	{ "DOT_PRODUCT",      CS_CSF_DOT_PRODUCT,      1 },
	{ "L2_DISTANCE_SQ",   CS_CSF_L2_DISTANCE_SQ,  1 },
	{ "COSINE_SIMILARITY", CS_CSF_COSINE_SIMILARITY, 1 },
	{ NULL, 0, 0 }
};

static csCSFId parse_csf(const char *s)
{
	for (int i = 0; csf_table[i].name; i++)
		if (!strcasecmp(s, csf_table[i].name))
			return csf_table[i].id;
	return 0;
}

static int csf_is_vector(csCSFId id)
{
	for (int i = 0; csf_table[i].name; i++)
		if (csf_table[i].id == id)
			return csf_table[i].is_vector;
	return 0;
}

int main(int argc, char *argv[])
{
	const char *uri = NULL;
	const char *csf_name = "SUM64";
	csCSFId csf = CS_CSF_SUM64;
	size_t data_len = 1048576; /* 1 MB default */
	int iterations = 5;
	int warmup = 1;
	uint32_t nvm_nsid = 1;
	uint32_t slm_nsid = 100;
	const char *out_csv = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--uri") && i + 1 < argc)
			uri = argv[++i];
		else if (!strcmp(argv[i], "--csf") && i + 1 < argc)
			csf_name = argv[++i];
		else if (!strcmp(argv[i], "--len") && i + 1 < argc)
			data_len = (size_t)strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--iterations") && i + 1 < argc)
			iterations = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--warmup") && i + 1 < argc)
			warmup = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--nvm-nsid") && i + 1 < argc)
			nvm_nsid = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--slm-nsid") && i + 1 < argc)
			slm_nsid = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--out-csv") && i + 1 < argc)
			out_csv = argv[++i];
	}

	if (!uri) {
		fprintf(stderr, "Usage: %s --uri <traddr:port> [--csf NAME] "
				"[--len BYTES] [--iterations N] [--warmup N] "
				"[--nvm-nsid N] [--slm-nsid N] [--out-csv FILE]\n"
				"  CSF: SUM64|MAX64|MIN64|DOT_PRODUCT|"
				"L2_DISTANCE_SQ|COSINE_SIMILARITY\n",
			argv[0]);
		return 1;
	}

	csf = parse_csf(csf_name);
	if (!csf) {
		fprintf(stderr, "Unknown CSF: %s\n", csf_name);
		return 1;
	}

	int is_vec = csf_is_vector(csf);
	uint32_t n_elements = is_vec
		? (uint32_t)(data_len / sizeof(float))
		: (uint32_t)(data_len / sizeof(uint64_t));

	/* Generate input data */
	void *in_buf = malloc(data_len);
	if (!in_buf) { fprintf(stderr, "malloc failed\n"); return 2; }

	if (is_vec) {
		float *f = (float *)in_buf;
		for (uint32_t i = 0; i < n_elements; i++)
			f[i] = (float)(i + 1) * 0.001f;
	} else {
		uint64_t *u = (uint64_t *)in_buf;
		for (uint32_t i = 0; i < n_elements; i++)
			u[i] = (uint64_t)(i + 1);
	}

	/* Output buffers */
	size_t out_size = is_vec ? sizeof(float) : sizeof(uint64_t);

	FILE *csv = NULL;
	if (out_csv) {
		csv = fopen(out_csv, "w");
		if (!csv) { fprintf(stderr, "Cannot open %s\n", out_csv); return 2; }
	}
	fprintf(csv ? csv : stdout,
		"#csf=%s data_bytes=%zu iterations=%d warmup=%d nvm_nsid=%u slm_nsid=%u\n",
		csf_name, data_len, iterations, warmup, nvm_nsid, slm_nsid);
	fprintf(csv ? csv : stdout,
		"csf,data_bytes,iteration,path,total_ms,h2d_ms,execute_ms,d2h_ms,status\n");

	/* ── Path 1: csRunComputeFromNvm (NVM->SLM + MRS, no H2D) ── */
	/* First, write data to NVM via Path 2 with LIBCS_MRS_STAGE_VIA_NVM=1.
	 * This leaves data in NVM for csRunComputeFromNvm to read. */
	setenv("LIBCS_MRS_STAGE_VIA_NVM", "1", 1);
	{
		uint64_t out_tmp = 0;
		csRunComputeExCached(uri, csf, in_buf, data_len,
				    &out_tmp, out_size, NULL, 0, NULL);
	}
	csRunComputeExCachedCleanup();
	unsetenv("LIBCS_MRS_STAGE_VIA_NVM");

	/* Now measure csRunComputeFromNvm (warmup + timed) */
	for (int iter = -warmup; iter <= iterations; iter++) {
		uint64_t out_val = 0;
		float out_f = 0;
		void *out_ptr = is_vec ? (void *)&out_f : (void *)&out_val;

		uint64_t t0 = now_ns();
		csStatus st = csRunComputeFromNvm(uri,
						  nvm_nsid, 0,
						  slm_nsid, 0,
						  is_vec ? n_elements / 2 : n_elements,
						  csf,
						  0, 0,
						  out_ptr, out_size, NULL);
		uint64_t t1 = now_ns();

		double total_ms = (double)(t1 - t0) / 1e6;
		int real_iter = iter;
		if (real_iter < 1) continue; /* skip warmup in CSV */

		fprintf(csv ? csv : stdout,
			"%s,%zu,%d,nvm_to_slm,%.4f,,,,%d\n",
			csf_name, data_len, real_iter, total_ms, (int)st);
	}

	/* ── Path 2: csRunComputeExCached (H2D every iteration) ── */
	/* Unset LIBCS_MRS_STAGE_VIA_NVM so H2D runs every call */
	unsetenv("LIBCS_MRS_STAGE_VIA_NVM");
	csRunComputeExCachedCleanup(); /* clear cache */

	for (int iter = -warmup; iter <= iterations; iter++) {
		uint64_t out_val = 0;
		float out_f = 0;
		void *out_ptr = is_vec ? (void *)&out_f : (void *)&out_val;

		uint64_t t0 = now_ns();
		csStatus st = csRunComputeExCached(uri, csf, in_buf, data_len,
						   out_ptr, out_size, NULL, 0, NULL);
		uint64_t t1 = now_ns();

		double total_ms = (double)(t1 - t0) / 1e6;
		csLatencyBreakdown lb = csGetLastLatencyBreakdown();

		int real_iter = iter;
		if (real_iter < 1) continue;

		fprintf(csv ? csv : stdout,
			"%s,%zu,%d,h2d,%.4f,%.4f,%.4f,%.4f,%d\n",
			csf_name, data_len, real_iter, total_ms,
			lb.h2d_ms, lb.execute_ms, lb.d2h_ms, (int)st);
	}
	csRunComputeExCachedCleanup();

	/* ── Path 3: csRunComputeExCached (NVM staging, skip H2D) ── */
	setenv("LIBCS_MRS_STAGE_VIA_NVM", "1", 1);
	csRunComputeExCachedCleanup(); /* clear cache */

	/* First call writes to NVM (untimed warmup) */
	{
		uint64_t out_tmp = 0;
		csRunComputeExCached(uri, csf, in_buf, data_len,
				    &out_tmp, out_size, NULL, 0, NULL);
	}

	for (int iter = -warmup; iter <= iterations; iter++) {
		uint64_t out_val = 0;
		float out_f = 0;
		void *out_ptr = is_vec ? (void *)&out_f : (void *)&out_val;

		uint64_t t0 = now_ns();
		csStatus st = csRunComputeExCached(uri, csf, in_buf, data_len,
						   out_ptr, out_size, NULL, 0, NULL);
		uint64_t t1 = now_ns();

		double total_ms = (double)(t1 - t0) / 1e6;
		csLatencyBreakdown lb = csGetLastLatencyBreakdown();

		int real_iter = iter;
		if (real_iter < 1) continue;

		fprintf(csv ? csv : stdout,
			"%s,%zu,%d,nvm_cached,%.4f,%.4f,%.4f,%.4f,%d\n",
			csf_name, data_len, real_iter, total_ms,
			lb.h2d_ms, lb.execute_ms, lb.d2h_ms, (int)st);
	}
	csRunComputeExCachedCleanup();
	unsetenv("LIBCS_MRS_STAGE_VIA_NVM");

	if (csv) fclose(csv);
	free(in_buf);
	return 0;
}
