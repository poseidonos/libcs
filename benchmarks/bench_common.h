/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file bench_common.h
 * @brief Shared fixtures for the sim-backend functional benchmarks
 *
 * Common timing/median helpers and the sim-backend setup/teardown
 * boilerplate used by bench_compress.c, bench_cosine_similarity.c,
 * bench_dot_product.c and bench_filter.c. Each benchmark is a separate
 * binary, so all helpers here are static inline.
 */

#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "cs_api.h"

#define RUNS 5

static int
bench_cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a;
	double db = *(const double *)b;
	if (da < db) {
		return -1;
	}
	if (da > db) {
		return 1;
	}
	return 0;
}

static double
bench_median(double values[RUNS])
{
	qsort(values, RUNS, sizeof(double), bench_cmp_double);
	return values[RUNS / 2];
}

static double
bench_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
}

static int
bench_ensure_results_dir(void)
{
	if (mkdir("benchmark_results", 0755) == 0 || errno == EEXIST) {
		return 0;
	}
	return -1;
}

/** Sim-benchmark session: results CSV plus the opened sim device. */
typedef struct {
	FILE *csv;
	csCSxHandle handle;
} bench_sim_ctx;

/**
 * Select the sim backend and open the results CSV and sim device.
 *
 * @param csv_name  File name under benchmark_results/ for this bench
 *
 * @return 0 on success, nonzero (with message on stderr) on failure
 */
static int
bench_sim_open(bench_sim_ctx *b, const char *csv_name)
{
	setenv("LIBCS_BACKEND", "sim", 1);

	if (bench_ensure_results_dir() != 0) {
		fprintf(stderr, "Failed to create benchmark_results directory\n");
		return 1;
	}

	b->csv = fopen(csv_name, "w");
	if (!b->csv) {
		fprintf(stderr, "Failed to open %s\n", csv_name);
		return 1;
	}

	csStatus status = csInit();
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csInit failed: %d\n", status);
		fclose(b->csv);
		return 1;
	}

	status = csOpenCSx("sim://libcs0", &b->handle);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csOpenCSx failed: %d\n", status);
		csShutdown();
		fclose(b->csv);
		return 1;
	}

	return 0;
}

/** Close the sim device and results CSV opened by bench_sim_open(). */
static void
bench_sim_close(bench_sim_ctx *b)
{
	csCloseCSx(b->handle);
	csShutdown();
	fclose(b->csv);
}

#endif /* BENCH_COMMON_H */
