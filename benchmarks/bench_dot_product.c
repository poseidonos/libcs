/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file bench_dot_product.c
 * @brief Functional correctness test: DOT_PRODUCT via sim backend vs CPU reference
 *
 * WARNING: This test uses the SIM backend (LIBCS_BACKEND=sim), which runs
 * computation on the host CPU with artificial latency (usleep). It does
 * NOT measure real computational storage offload performance.
 *
 * Purpose: Verify that csQueueComputeRequest(CS_CSF_DOT_PRODUCT) through
 * the sim backend produces identical results to the CPU reference
 * implementation. Timing columns (cpu_ref_ms, sim_ms) show API path
 * overhead, NOT device performance. Do not interpret sim_ms as device
 * execution time.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_common.h"
#include "cs_api.h"

#define AMPLIFY 1000  /* Repeat CPU ref to exceed clock_gettime resolution */

static float
cpu_dot_product(const float *a, const float *b, size_t n)
{
	float sum = 0.0f;
	for (size_t i = 0; i < n; i++) {
		sum += a[i] * b[i];
	}
	return sum;
}

int
main(void)
{
	srand(12345);

	bench_sim_ctx sim;
	if (bench_sim_open(&sim, "benchmark_results/bench_dot_product.csv") != 0) {
		return 1;
	}
	FILE *csv = sim.csv;
	csCSxHandle handle = sim.handle;
	csStatus status;

	fprintf(csv, "elements,cpu_ref_ms,sim_ms,match\n");

	const size_t sizes[] = { 2048, 8192, 32768, 49152, 65536, 98304, 131072 };
	const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

	printf("=== DOT_PRODUCT Functional Correctness Test (sim backend) ===\n");
	printf("Elements    CPU ref (ms)  Sim (ms)     Match\n");

	for (size_t s = 0; s < num_sizes; s++) {
		size_t n = sizes[s];
		float *vec_a = malloc(n * sizeof(float));
		float *vec_b = malloc(n * sizeof(float));
		float *cs_input = malloc(n * 2 * sizeof(float));
		float cs_result = 0.0f;
		if (!vec_a || !vec_b || !cs_input) {
			fprintf(stderr, "Allocation failed for size %zu\n", n);
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
		memcpy(cs_input, vec_a, n * sizeof(float));
		memcpy(cs_input + n, vec_b, n * sizeof(float));

		double cpu_times[RUNS] = { 0 };
		double sim_times[RUNS] = { 0 };
		float cpu_result = 0.0f;

		for (size_t r = 0; r < RUNS; r++) {
			double t0 = bench_now_ms();
			float sink = 0.0f;
			for (size_t k = 0; k < AMPLIFY; k++) {
				float tmp = cpu_dot_product(vec_a, vec_b, n);
				__asm__ __volatile__("" : "+r"(tmp) :: "memory");
				sink += tmp;
			}
			cpu_result = sink / (float)AMPLIFY;
			double t1 = bench_now_ms();
			cpu_times[r] = (t1 - t0) / (double)AMPLIFY;
		}

		for (size_t r = 0; r < RUNS; r++) {
			cs_result = 0.0f;

			csComputeDesc desc = {
				.csf_id = CS_CSF_DOT_PRODUCT,
				.input_buffer = cs_input,
				.input_size = n * 2 * sizeof(float),
				.output_buffer = &cs_result,
				.output_size = sizeof(cs_result),
				.args = NULL,
				.args_size = 0,
			};

			double t0 = bench_now_ms();
			for (size_t k = 0; k < AMPLIFY; k++) {
				status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
				if (status != CS_STATUS_SUCCESS) {
					fprintf(stderr, "csQueueComputeRequest failed (%zu): %d\n", n, status);
					free(vec_a);
					free(vec_b);
					free(cs_input);
					csCloseCSx(handle);
					csShutdown();
					fclose(csv);
					return 1;
				}
			}
			double t1 = bench_now_ms();

			sim_times[r] = (t1 - t0) / (double)AMPLIFY;
		}

		float diff = cs_result - cpu_result;
		if (diff < 0.0f) {
			diff = -diff;
		}
		float tol = cpu_result * 1e-4f;
		if (tol < 0.0f) {
			tol = -tol;
		}
		int match = (diff <= tol + 1e-6f);

		double cpu_ms = bench_median(cpu_times);
		double sim_ms = bench_median(sim_times);

		printf("%-11zu %-13.4f %-12.4f %s\n",
		       n, cpu_ms, sim_ms, match ? "PASS" : "FAIL");
		fprintf(csv, "%zu,%.6f,%.6f,%s\n",
		        n, cpu_ms, sim_ms, match ? "PASS" : "FAIL");

		if (!match) {
			fprintf(stderr, "Result mismatch for %zu elements: cpu=%.6f cs=%.6f\n",
			        n, (double)cpu_result, (double)cs_result);
		}

		free(vec_a);
		free(vec_b);
		free(cs_input);
	}

	bench_sim_close(&sim);
	return 0;
}
