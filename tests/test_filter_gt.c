/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * test_filter_gt.c — Focused FILTER_GT unit tests
 *
 * Tests the full csRunComputeEx → backend → cs_infer_bytes_produced pipeline
 * for FILTER_GT with specific attention to:
 *   1. Correct element count (not 3x or Nx)
 *   2. NaN sentinel handling in output buffer
 *   3. Large data correctness
 *   4. Edge cases (all pass, none pass, threshold at boundary)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_api.h"
#include "cs_helpers.h"
#include "test_util.h"

static void set_sim_backend(void)
{
	setenv("LIBCS_BACKEND", "sim", 1);
}

/* ------------------------------------------------------------------ */
/* TEST 1: Basic filter — known input, exact count                    */
/* ------------------------------------------------------------------ */
static void test_filter_basic_count(void)
{
	TEST_START("FILTER_GT basic count (10 vals, threshold 0.5)");

	set_sim_backend();

	float input[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
	size_t n = sizeof(input) / sizeof(input[0]);
	float output[10];
	for (size_t i = 0; i < n; i++) output[i] = NAN;

	float threshold = 0.5f;
	csComputeResult result;
	memset(&result, 0, sizeof(result));

	csStatus status = csRunComputeEx(
		"sim://libcs0", CS_CSF_FILTER_GT,
		input, sizeof(input),
		output, sizeof(output),
		&threshold, sizeof(threshold),
		&result);

	if (status != CS_STATUS_SUCCESS) {
		char msg[64];
		snprintf(msg, sizeof(msg), "csRunComputeEx failed: %d", status);
		TEST_FAIL(msg);
		return;
	}

	/* Expected: 5 values > 0.5 → {0.6, 0.7, 0.8, 0.9, 1.0} */
	if (result.elements_produced != 5) {
		char msg[128];
		snprintf(msg, sizeof(msg),
			 "expected 5 elements, got %u (bytes_produced=%zu)",
			 result.elements_produced, result.bytes_produced);
		TEST_FAIL(msg);
		return;
	}

	/* Verify values */
	float expected[] = {0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
	for (int i = 0; i < 5; i++) {
		if (fabsf(output[i] - expected[i]) > 1e-6f) {
			char msg[128];
			snprintf(msg, sizeof(msg),
				 "output[%d]=%.6f expected=%.6f", i, output[i], expected[i]);
			TEST_FAIL(msg);
			return;
		}
	}

	/* Verify remaining slots are NaN (sentinel preserved) */
	for (size_t i = 5; i < n; i++) {
		if (!isnan(output[i])) {
			char msg[128];
			snprintf(msg, sizeof(msg),
				 "output[%zu]=%.6f expected NaN", i, output[i]);
			TEST_FAIL(msg);
			return;
		}
	}

	TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* TEST 2: All values pass filter                                     */
/* ------------------------------------------------------------------ */
static void test_filter_all_pass(void)
{
	TEST_START("FILTER_GT all pass (threshold -1.0)");

	set_sim_backend();

	float input[] = {0.1f, 0.5f, 1.0f, 2.0f};
	size_t n = 4;
	float output[4];
	for (size_t i = 0; i < n; i++) output[i] = NAN;

	float threshold = -1.0f;
	csComputeResult result;
	memset(&result, 0, sizeof(result));

	csStatus status = csRunComputeEx(
		"sim://libcs0", CS_CSF_FILTER_GT,
		input, sizeof(input),
		output, sizeof(output),
		&threshold, sizeof(threshold),
		&result);

	if (status != CS_STATUS_SUCCESS) {
		TEST_FAIL("csRunComputeEx failed");
		return;
	}

	if (result.elements_produced != 4) {
		char msg[128];
		snprintf(msg, sizeof(msg), "expected 4, got %u", result.elements_produced);
		TEST_FAIL(msg);
		return;
	}

	TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* TEST 3: No values pass filter                                      */
/* ------------------------------------------------------------------ */
static void test_filter_none_pass(void)
{
	TEST_START("FILTER_GT none pass (threshold 100.0)");

	set_sim_backend();

	float input[] = {0.1f, 0.5f, 1.0f, 2.0f};
	size_t n = 4;
	float output[4];
	for (size_t i = 0; i < n; i++) output[i] = NAN;

	float threshold = 100.0f;
	csComputeResult result;
	memset(&result, 0, sizeof(result));

	csStatus status = csRunComputeEx(
		"sim://libcs0", CS_CSF_FILTER_GT,
		input, sizeof(input),
		output, sizeof(output),
		&threshold, sizeof(threshold),
		&result);

	if (status != CS_STATUS_SUCCESS) {
		TEST_FAIL("csRunComputeEx failed");
		return;
	}

	if (result.elements_produced != 0) {
		char msg[128];
		snprintf(msg, sizeof(msg), "expected 0, got %u", result.elements_produced);
		TEST_FAIL(msg);
		return;
	}

	/* All should be NaN */
	for (size_t i = 0; i < n; i++) {
		if (!isnan(output[i])) {
			char msg[128];
			snprintf(msg, sizeof(msg), "output[%zu]=%.6f expected NaN", i, output[i]);
			TEST_FAIL(msg);
			return;
		}
	}

	TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* TEST 4: Boundary — threshold equals value (strict >)               */
/* ------------------------------------------------------------------ */
static void test_filter_boundary(void)
{
	TEST_START("FILTER_GT boundary (threshold == value, strict >)");

	set_sim_backend();

	float input[] = {0.5f, 0.5f, 0.5f};
	float output[3];
	for (int i = 0; i < 3; i++) output[i] = NAN;

	float threshold = 0.5f;
	csComputeResult result;
	memset(&result, 0, sizeof(result));

	csStatus status = csRunComputeEx(
		"sim://libcs0", CS_CSF_FILTER_GT,
		input, sizeof(input),
		output, sizeof(output),
		&threshold, sizeof(threshold),
		&result);

	if (status != CS_STATUS_SUCCESS) {
		TEST_FAIL("csRunComputeEx failed");
		return;
	}

	/* 0.5 is NOT > 0.5, so all should be filtered out */
	if (result.elements_produced != 0) {
		char msg[128];
		snprintf(msg, sizeof(msg), "expected 0 (strict >), got %u", result.elements_produced);
		TEST_FAIL(msg);
		return;
	}

	TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* TEST 5: Large data (131072 floats, matching benchmark AGG_ROWS)    */
/* ------------------------------------------------------------------ */
static void test_filter_large_data(void)
{
	TEST_START("FILTER_GT large data (131072 floats, threshold 0.5)");

	set_sim_backend();

	size_t n = 131072;
	float *input = malloc(n * sizeof(float));
	float *output = malloc(n * sizeof(float));
	if (!input || !output) {
		TEST_FAIL("malloc failed");
		free(input);
		free(output);
		return;
	}

	/* Fill with deterministic pattern: i / n → range [0, 1) */
	size_t expected_count = 0;
	double expected_sum = 0.0;
	for (size_t i = 0; i < n; i++) {
		input[i] = (float)i / (float)n;
		output[i] = NAN;
		if (input[i] > 0.5f) {
			expected_count++;
			expected_sum += (double)input[i];
		}
	}

	float threshold = 0.5f;
	csComputeResult result;
	memset(&result, 0, sizeof(result));

	csStatus status = csRunComputeEx(
		"sim://libcs0", CS_CSF_FILTER_GT,
		input, n * sizeof(float),
		output, n * sizeof(float),
		&threshold, sizeof(threshold),
		&result);

	if (status != CS_STATUS_SUCCESS) {
		char msg[64];
		snprintf(msg, sizeof(msg), "csRunComputeEx failed: %d", status);
		TEST_FAIL(msg);
		free(input);
		free(output);
		return;
	}

	if (result.elements_produced != (uint32_t)expected_count) {
		char msg[128];
		snprintf(msg, sizeof(msg),
			 "COUNT mismatch: expected %zu, got %u (ratio=%.2f)",
			 expected_count, result.elements_produced,
			 (double)result.elements_produced / (double)expected_count);
		TEST_FAIL(msg);
		free(input);
		free(output);
		return;
	}

	/* Verify sum */
	double actual_sum = 0.0;
	for (uint32_t i = 0; i < result.elements_produced; i++) {
		actual_sum += (double)output[i];
	}
	double rel_err = fabs(actual_sum - expected_sum) / fmax(fabs(expected_sum), 1e-10);
	if (rel_err > 1e-4) {
		char msg[128];
		snprintf(msg, sizeof(msg),
			 "SUM mismatch: expected=%.2f actual=%.2f rel_err=%.2e",
			 expected_sum, actual_sum, rel_err);
		TEST_FAIL(msg);
		free(input);
		free(output);
		return;
	}

	/* Verify NaN sentinel after valid data */
	int nan_ok = 1;
	for (size_t i = result.elements_produced; i < n; i++) {
		if (!isnan(output[i])) {
			nan_ok = 0;
			break;
		}
	}
	if (!nan_ok) {
		TEST_FAIL("NaN sentinel not preserved after valid data");
		free(input);
		free(output);
		return;
	}

	free(input);
	free(output);
	TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* TEST 6: Verify output buffer not 3x inflated                       */
/* ------------------------------------------------------------------ */
static void test_filter_no_3x_inflation(void)
{
	TEST_START("FILTER_GT no 3x inflation (regression test)");

	set_sim_backend();

	/* Exactly the pattern from the benchmark: random-like floats 0..1 */
	size_t n = 1000;
	float *input = malloc(n * sizeof(float));
	float *output = malloc(n * sizeof(float));
	if (!input || !output) {
		TEST_FAIL("malloc failed");
		free(input);
		free(output);
		return;
	}

	size_t cpu_count = 0;
	for (size_t i = 0; i < n; i++) {
		/* Simple PRNG-like pattern */
		input[i] = (float)((i * 7 + 13) % 100) / 100.0f;
		output[i] = NAN;
		if (input[i] > 0.5f) cpu_count++;
	}

	float threshold = 0.5f;
	csComputeResult result;
	memset(&result, 0, sizeof(result));

	csStatus status = csRunComputeEx(
		"sim://libcs0", CS_CSF_FILTER_GT,
		input, n * sizeof(float),
		output, n * sizeof(float),
		&threshold, sizeof(threshold),
		&result);

	if (status != CS_STATUS_SUCCESS) {
		TEST_FAIL("csRunComputeEx failed");
		free(input);
		free(output);
		return;
	}

	uint32_t cs_count = result.elements_produced;

	if (cs_count != (uint32_t)cpu_count) {
		char msg[128];
		snprintf(msg, sizeof(msg),
			 "COUNT: CPU=%zu CS=%u ratio=%.2f (should be 1.0)",
			 cpu_count, cs_count, (double)cs_count / (double)cpu_count);
		TEST_FAIL(msg);
		free(input);
		free(output);
		return;
	}

	/* Explicitly check no 3x pattern */
	if (cs_count == cpu_count * 3) {
		TEST_FAIL("3x inflation detected — NaN sentinel bug");
		free(input);
		free(output);
		return;
	}

	free(input);
	free(output);
	TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* TEST 7: csRunCompute (non-Ex) also works                           */
/* ------------------------------------------------------------------ */
static void test_filter_run_compute(void)
{
	TEST_START("FILTER_GT via csRunCompute (non-Ex path)");

	set_sim_backend();

	float input[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
	float output[5];
	for (int i = 0; i < 5; i++) output[i] = NAN;

	float threshold = 2.5f;

	csStatus status = csRunCompute(
		"sim://libcs0", CS_CSF_FILTER_GT,
		input, sizeof(input),
		output, sizeof(output),
		&threshold, sizeof(threshold));

	if (status != CS_STATUS_SUCCESS) {
		TEST_FAIL("csRunCompute failed");
		return;
	}

	/* Expected: {3.0, 4.0, 5.0} */
	int count = 0;
	for (int i = 0; i < 5; i++) {
		if (!isnan(output[i])) count++;
	}

	if (count != 3) {
		char msg[128];
		snprintf(msg, sizeof(msg), "expected 3 non-NaN, got %d", count);
		TEST_FAIL(msg);
		return;
	}

	if (fabsf(output[0] - 3.0f) > 1e-6f ||
	    fabsf(output[1] - 4.0f) > 1e-6f ||
	    fabsf(output[2] - 5.0f) > 1e-6f) {
		TEST_FAIL("values mismatch");
		return;
	}

	TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
	printf("===========================================\n");
	printf("  FILTER_GT Focused Unit Tests\n");
	printf("===========================================\n\n");

	test_filter_basic_count();
	test_filter_all_pass();
	test_filter_none_pass();
	test_filter_boundary();
	test_filter_large_data();
	test_filter_no_3x_inflation();
	test_filter_run_compute();

	TEST_SUMMARY();

	csShutdown();
	unsetenv("LIBCS_BACKEND");

	return TEST_EXIT();
}
