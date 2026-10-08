/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file bench_cosine_similarity.c
 * @brief Functional correctness test: COSINE_SIMILARITY via sim backend vs CPU reference
 *
 * WARNING: This test uses the SIM backend (LIBCS_BACKEND=sim), which runs
 * computation on the host CPU with artificial latency (usleep). It does
 * NOT measure real computational storage offload performance.
 *
 * Purpose: Verify that csQueueComputeRequest(CS_CSF_COSINE_SIMILARITY)
 * through the sim backend produces identical results to the CPU reference
 * implementation. Timing columns (cpu_ref_ms, sim_ms) show API path
 * overhead, NOT device performance. Do not interpret sim_ms as device
 * execution time.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cs_api.h"

#define RUNS 5
#define AMPLIFY 1000  /* Repeat CPU ref to exceed clock_gettime resolution */

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

static float
cpu_cosine_similarity(const float *a, const float *b, size_t n)
{
	float dot = 0.0f;
	float norm_a = 0.0f;
	float norm_b = 0.0f;

	for (size_t i = 0; i < n; i++) {
		dot    += a[i] * b[i];
		norm_a += a[i] * a[i];
		norm_b += b[i] * b[i];
	}

	float denom = sqrtf(norm_a * norm_b);
	if (denom <= 0.0f) {
		return 0.0f;
	}
	float result = dot / denom;
	if (result < -1.0f) {
		return -1.0f;
	}
	if (result > 1.0f) {
		return 1.0f;
	}
	return result;
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

	FILE *csv = fopen("benchmark_results/bench_cosine_similarity.csv", "w");
	if (!csv) {
		fprintf(stderr, "Failed to open benchmark_results/bench_cosine_similarity.csv\n");
		return 1;
	}
	fprintf(csv, "dims,cpu_ref_ms,sim_ms,match\n");

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

	const size_t dims[] = { 128, 384, 768, 1536, 3072 };
	const size_t num_dims = sizeof(dims) / sizeof(dims[0]);

	printf("=== COSINE_SIMILARITY Functional Correctness Test (sim backend) ===\n");
	printf("Dims   CPU ref (ms)  Sim (ms)     Match\n");

	for (size_t s = 0; s < num_dims; s++) {
		size_t n = dims[s];
		float *vec_a    = malloc(n * sizeof(float));
		float *vec_b    = malloc(n * sizeof(float));
		float *cs_input = malloc(n * 2 * sizeof(float));
		float cs_result = 0.0f;

		if (!vec_a || !vec_b || !cs_input) {
			fprintf(stderr, "Allocation failed for dim %zu\n", n);
			free(vec_a);
			free(vec_b);
			free(cs_input);
			csCloseCSx(handle);
			csShutdown();
			fclose(csv);
			return 1;
		}

		for (size_t i = 0; i < n; i++) {
			vec_a[i] = (float)rand() / ((float)RAND_MAX + 1.0f);
			vec_b[i] = (float)rand() / ((float)RAND_MAX + 1.0f);
		}
		memcpy(cs_input,     vec_a, n * sizeof(float));
		memcpy(cs_input + n, vec_b, n * sizeof(float));

		double cpu_times[RUNS] = { 0 };
		double sim_times[RUNS]  = { 0 };
		float  cpu_result = 0.0f;

		for (size_t r = 0; r < RUNS; r++) {
			double t0 = now_ms();
			float sink = 0.0f;
			for (size_t k = 0; k < AMPLIFY; k++) {
				float tmp = cpu_cosine_similarity(vec_a, vec_b, n);
				__asm__ __volatile__("" : "+r"(tmp) :: "memory");
				sink += tmp;
			}
			cpu_result = sink / (float)AMPLIFY;
			double t1 = now_ms();
			cpu_times[r] = (t1 - t0) / (double)AMPLIFY;
		}

		for (size_t r = 0; r < RUNS; r++) {
			cs_result = 0.0f;

			csComputeDesc desc = {
				.csf_id       = CS_CSF_COSINE_SIMILARITY,
				.input_buffer = cs_input,
				.input_size   = n * 2 * sizeof(float),
				.output_buffer = &cs_result,
				.output_size  = sizeof(cs_result),
				.args         = NULL,
				.args_size    = 0,
			};

			double t0 = now_ms();
			for (size_t k = 0; k < AMPLIFY; k++) {
				status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
				if (status != CS_STATUS_SUCCESS) {
					fprintf(stderr, "csQueueComputeRequest failed (dim=%zu): %d\n", n, status);
					free(vec_a);
					free(vec_b);
					free(cs_input);
					csCloseCSx(handle);
					csShutdown();
					fclose(csv);
					return 1;
				}
			}
			double t1 = now_ms();

			sim_times[r] = (t1 - t0) / (double)AMPLIFY;
		}

		float diff = cs_result - cpu_result;
		if (diff < 0.0f) {
			diff = -diff;
		}
		int match = (diff <= 1e-4f);

		double cpu_ms = median(cpu_times);
		double sim_ms = median(sim_times);

		printf("%-6zu %-13.4f %-12.4f %s\n",
		       n, cpu_ms, sim_ms, match ? "PASS" : "FAIL");
		fprintf(csv, "%zu,%.6f,%.6f,%s\n",
		        n, cpu_ms, sim_ms, match ? "PASS" : "FAIL");

		if (!match) {
			fprintf(stderr, "Result mismatch for dim %zu: cpu=%.6f cs=%.6f\n",
			        n, (double)cpu_result, (double)cs_result);
		}

		free(vec_a);
		free(vec_b);
		free(cs_input);
	}

	csCloseCSx(handle);
	csShutdown();
	fclose(csv);
	return 0;
}
