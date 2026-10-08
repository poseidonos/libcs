/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file bench_compress.c
 * @brief Functional correctness test: RLE_COMPRESS via sim backend vs CPU reference
 *
 * WARNING: This test uses the SIM backend (LIBCS_BACKEND=sim), which runs
 * computation on the host CPU with artificial latency (usleep). It does
 * NOT measure real computational storage offload performance.
 *
 * Purpose: Verify that csQueueComputeRequest(CS_CSF_RLE_COMPRESS) through
 * the sim backend produces identical results to the CPU reference
 * implementation. Timing columns (cpu_ref_ms, sim_ms) show API path
 * overhead, NOT device performance. Do not interpret sim_ms as device
 * execution time.
 */

#include <errno.h>
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
rle_encode(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size)
{
	size_t in_pos = 0;
	size_t out_pos = 0;

	while (in_pos < input_size) {
		uint8_t value = input[in_pos];
		uint8_t run = 1;
		while ((in_pos + run) < input_size && input[in_pos + run] == value && run < 255) {
			run++;
		}

		if (out_pos + 2 > output_size) {
			return 0;
		}

		output[out_pos++] = run;
		output[out_pos++] = value;
		in_pos += run;
	}

	return out_pos;
}

static size_t
rle_encoded_length_from_buffer(const uint8_t *output, size_t capacity)
{
	size_t length = 0;
	for (size_t i = 0; i + 1 < capacity; i += 2) {
		if (output[i] == 0) {
			break;
		}
		length = i + 2;
	}
	return length;
}

static void
fill_pattern_high(uint8_t *buffer, size_t size)
{
	const size_t run_len = 16;
	uint8_t value = 0;
	for (size_t i = 0; i < size; i++) {
		if ((i % run_len) == 0) {
			value++;
		}
		buffer[i] = value;
	}
}

static void
fill_pattern_low(uint8_t *buffer, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		buffer[i] = (uint8_t)(rand() & 0xFF);
	}
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
	srand(67890);
	setenv("LIBCS_BACKEND", "sim", 1);

	if (ensure_results_dir() != 0) {
		fprintf(stderr, "Failed to create benchmark_results directory\n");
		return 1;
	}

	FILE *csv = fopen("benchmark_results/bench_compress.csv", "w");
	if (!csv) {
		fprintf(stderr, "Failed to open benchmark_results/bench_compress.csv\n");
		return 1;
	}
	fprintf(csv, "size,pattern,cpu_ref_ms,sim_ms,match\n");

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

	const size_t sizes[] = { 64 * 1024, 256 * 1024, 1024 * 1024, 4 * 1024 * 1024 };
	const char *patterns[] = { "high", "low" };
	const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);
	const size_t num_patterns = sizeof(patterns) / sizeof(patterns[0]);

	printf("=== RLE_COMPRESS Functional Correctness Test (sim backend) ===\n");
	printf("Size        Pattern    CPU ref (ms)  Sim (ms)     Match\n");

	for (size_t s = 0; s < num_sizes; s++) {
		for (size_t p = 0; p < num_patterns; p++) {
			size_t size = sizes[s];
			const char *pattern = patterns[p];

			uint8_t *input = malloc(size);
			uint8_t *cpu_out = malloc(size * 2);
			uint8_t *cs_out = malloc(size * 2);
			if (!input || !cpu_out || !cs_out) {
				fprintf(stderr, "Allocation failed for size %zu\n", size);
				free(input);
				free(cpu_out);
				free(cs_out);
				csCloseCSx(handle);
				csShutdown();
				fclose(csv);
				return 1;
			}

			if (strcmp(pattern, "high") == 0) {
				fill_pattern_high(input, size);
			} else {
				fill_pattern_low(input, size);
			}

			double cpu_times[RUNS] = { 0 };
			double sim_times[RUNS] = { 0 };
			size_t expected_len = 0;

			for (size_t r = 0; r < RUNS; r++) {
				double t0 = now_ms();
				expected_len = rle_encode(input, size, cpu_out, size * 2);
				double t1 = now_ms();
				cpu_times[r] = t1 - t0;
				if (expected_len == 0) {
					fprintf(stderr, "CPU RLE encode failed for size %zu\n", size);
					free(input);
					free(cpu_out);
					free(cs_out);
					csCloseCSx(handle);
					csShutdown();
					fclose(csv);
					return 1;
				}
			}

			for (size_t r = 0; r < RUNS; r++) {
				memset(cs_out, 0, size * 2);

				csComputeDesc desc = {
					.csf_id = CS_CSF_RLE_COMPRESS,
					.input_buffer = input,
					.input_size = size,
					.output_buffer = cs_out,
					.output_size = size * 2,
					.args = NULL,
					.args_size = 0,
				};

				double t0 = now_ms();
				status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
				double t1 = now_ms();
				if (status != CS_STATUS_SUCCESS) {
					fprintf(stderr, "csQueueComputeRequest failed (%zu, %s): %d\n",
					        size, pattern, status);
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

			size_t cs_len = rle_encoded_length_from_buffer(cs_out, size * 2);
			int match = (cs_len == expected_len &&
			             memcmp(cpu_out, cs_out, expected_len) == 0);

			double cpu_ms = median(cpu_times);
			double sim_ms = median(sim_times);

			printf("%-11zu %-10s %-13.3f %-12.3f %s\n",
			       size, pattern, cpu_ms, sim_ms, match ? "PASS" : "FAIL");
			fprintf(csv, "%zu,%s,%.6f,%.6f,%s\n",
			        size, pattern, cpu_ms, sim_ms, match ? "PASS" : "FAIL");

			if (!match) {
				fprintf(stderr, "RLE mismatch for size=%zu pattern=%s\n", size, pattern);
			}

			free(input);
			free(cpu_out);
			free(cs_out);
		}
	}

	csCloseCSx(handle);
	csShutdown();
	fclose(csv);
	return 0;
}
