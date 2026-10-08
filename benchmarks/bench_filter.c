/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file bench_filter.c
 * @brief Functional correctness test: FILTER_GT via sim backend vs CPU reference
 *
 * WARNING: This test uses the SIM backend (LIBCS_BACKEND=sim), which runs
 * computation on the host CPU with artificial latency (usleep). It does
 * NOT measure real computational storage offload performance.
 *
 * Purpose: Verify that csQueueComputeRequest(CS_CSF_FILTER_GT) through the
 * sim backend produces identical results to the CPU reference implementation.
 * Timing columns (cpu_ref_ms, sim_ms) show API path overhead, NOT device
 * performance. Do not interpret sim_ms as device execution time.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cs_api.h"

#define RUNS 5

static int
cmp_double(const void *a, const void *b)
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
median(double values[RUNS])
{
	qsort(values, RUNS, sizeof(double), cmp_double);
	return values[RUNS / 2];
}

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
}

static size_t
cpu_filter_gt(const float *input, size_t count, float threshold, float *output)
{
	size_t out_count = 0;
	for (size_t i = 0; i < count; i++) {
		if (input[i] > threshold) {
			output[out_count++] = input[i];
		}
	}
	return out_count;
}

static size_t
cs_filter_count(const float *output, size_t capacity)
{
	size_t out_count = 0;
	while (out_count < capacity && !isnan(output[out_count])) {
		out_count++;
	}
	return out_count;
}

static int
ensure_results_dir(void)
{
	if (mkdir("benchmark_results", 0755) == 0 || errno == EEXIST) {
		return 0;
	}
	return -1;
}

int
main(void)
{
	srand(12345);
	setenv("LIBCS_BACKEND", "sim", 1);

	if (ensure_results_dir() != 0) {
		fprintf(stderr, "Failed to create benchmark_results directory\n");
		return 1;
	}

	FILE *csv = fopen("benchmark_results/bench_filter.csv", "w");
	if (!csv) {
		fprintf(stderr, "Failed to open benchmark_results/bench_filter.csv\n");
		return 1;
	}
	fprintf(csv, "elements,cpu_ref_ms,sim_ms,match\n");

	csStatus status = csInit();
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csInit failed: %d\n", status);
		fclose(csv);
		return 1;
	}

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csOpenCSx failed: %d\n", status);
		csShutdown();
		fclose(csv);
		return 1;
	}

	const size_t sizes[] = { 10000, 100000, 1000000, 10000000 };
	const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);
	const float threshold = 0.5f;

	printf("=== FILTER_GT Functional Correctness Test (sim backend) ===\n");
	printf("Elements    CPU ref (ms)  Sim (ms)     Match\n");

	for (size_t s = 0; s < num_sizes; s++) {
		size_t n = sizes[s];
		float *input = malloc(n * sizeof(float));
		float *cpu_out = malloc(n * sizeof(float));
		float *cs_out = malloc(n * sizeof(float));
		if (!input || !cpu_out || !cs_out) {
			fprintf(stderr, "Allocation failed for size %zu\n", n);
			free(input);
			free(cpu_out);
			free(cs_out);
			csCloseCSx(handle);
			csShutdown();
			fclose(csv);
			return 1;
		}

		for (size_t i = 0; i < n; i++) {
			input[i] = (float)rand() / ((float)RAND_MAX + 1.0f);
		}

		double cpu_times[RUNS] = { 0 };
		double sim_times[RUNS] = { 0 };
		size_t expected_count = 0;

		for (size_t r = 0; r < RUNS; r++) {
			double t0 = now_ms();
			expected_count = cpu_filter_gt(input, n, threshold, cpu_out);
			double t1 = now_ms();
			cpu_times[r] = t1 - t0;
		}

		for (size_t r = 0; r < RUNS; r++) {
			for (size_t i = 0; i < n; i++) {
				cs_out[i] = NAN;
			}

			csComputeDesc desc = {
				.csf_id = CS_CSF_FILTER_GT,
				.input_buffer = input,
				.input_size = n * sizeof(float),
				.output_buffer = cs_out,
				.output_size = n * sizeof(float),
				.args = &threshold,
				.args_size = sizeof(threshold),
			};

			double t0 = now_ms();
			status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
			double t1 = now_ms();
			if (status != CS_STATUS_SUCCESS) {
				fprintf(stderr, "csQueueComputeRequest failed (%zu): %d\n", n, status);
				free(input);
				free(cpu_out);
				free(cs_out);
				csCloseCSx(handle);
				csShutdown();
				fclose(csv);
				return 1;
			}

			sim_times[r] = t1 - t0;
		}

		size_t cs_count = cs_filter_count(cs_out, n);
		int match = (cs_count == expected_count &&
		             memcmp(cpu_out, cs_out, expected_count * sizeof(float)) == 0);

		double cpu_ms = median(cpu_times);
		double sim_ms = median(sim_times);

		printf("%-11zu %-13.3f %-12.3f %s\n",
		       n, cpu_ms, sim_ms, match ? "PASS" : "FAIL");
		fprintf(csv, "%zu,%.6f,%.6f,%s\n",
		        n, cpu_ms, sim_ms, match ? "PASS" : "FAIL");

		if (!match) {
			fprintf(stderr, "Result mismatch for %zu elements\n", n);
		}

		free(input);
		free(cpu_out);
		free(cs_out);
	}

	csCloseCSx(handle);
	csShutdown();
	fclose(csv);
	return 0;
}
