/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file scenario_02_raw.c
 * @brief Scenario 02 - Filter My Dataset
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_api.h"

#define CSF_ID_FILTER_GT 6U
#define DATASET_COUNT 1000000U

static size_t
count_non_nan(const float *values, size_t max_count)
{
	size_t count = 0;
	while (count < max_count && !isnan(values[count])) {
		count++;
	}
	return count;
}

int
main(void)
{
	csStatus status;
	csCSxHandle handle = CS_INVALID_HANDLE;
	csMemHandle input_mem = CS_INVALID_HANDLE;
	csMemHandle output_mem = CS_INVALID_HANDLE;
	void *input_ptr = NULL;
	void *output_ptr = NULL;

	setenv("LIBCS_BACKEND", "sim", 1);

	status = csInit();
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csInit failed: %d\n", status);
		return 1;
	}

	status = csOpenCSx("sim://libcs0", &handle);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csOpenCSx failed: %d\n", status);
		csShutdown();
		return 1;
	}

	float *host_input = malloc((size_t)DATASET_COUNT * sizeof(float));
	float *host_output = malloc((size_t)DATASET_COUNT * sizeof(float));
	if (!host_input || !host_output) {
		fprintf(stderr, "host allocation failed\n");
		free(host_input);
		free(host_output);
		csCloseCSx(handle);
		csShutdown();
		return 1;
	}

	for (size_t i = 0; i < DATASET_COUNT; i++) {
		host_input[i] = (float)rand() / ((float)RAND_MAX + 1.0f);
		host_output[i] = NAN;
	}

	/* PAIN[P1]: Memory model complexity — csAllocMem requires 4 parameters and returns both a handle and a raw pointer with unclear ownership semantics. */
	csMemDesc mem_desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = (size_t)DATASET_COUNT * sizeof(float),
		.alignment = 0,
		.flags = 0,
	};

	status = csAllocMem(handle, &mem_desc, &input_mem, &input_ptr);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csAllocMem(input) failed: %d\n", status);
		free(host_input);
		free(host_output);
		csCloseCSx(handle);
		csShutdown();
		return 1;
	}

	status = csAllocMem(handle, &mem_desc, &output_mem, &output_ptr);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csAllocMem(output) failed: %d\n", status);
		csFreeMem(handle, input_mem);
		free(host_input);
		free(host_output);
		csCloseCSx(handle);
		csShutdown();
		return 1;
	}

	for (size_t i = 0; i < DATASET_COUNT; i++) {
		((float *)output_ptr)[i] = NAN;
	}

	/* PAIN[P4]: Discoverability — copy API takes pointers-or-handles through void* and requires casting handles, which is non-obvious and fragile. */
	status = csQueueCopyMemRequest(handle, NULL, CS_INVALID_HANDLE,
				       host_input, (void *)(uintptr_t)input_mem,
				       (size_t)DATASET_COUNT * sizeof(float),
				       CS_COPY_HOST_TO_DEVICE);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csQueueCopyMemRequest(H2D) failed: %d\n", status);
		csFreeMem(handle, output_mem);
		csFreeMem(handle, input_mem);
		free(host_input);
		free(host_output);
		csCloseCSx(handle);
		csShutdown();
		return 1;
	}

	float threshold = 0.75f;
	csComputeDesc compute = {
		.csf_id = CSF_ID_FILTER_GT,
		.input_buffer = input_ptr,
		.input_size = (size_t)DATASET_COUNT * sizeof(float),
		.output_buffer = output_ptr,
		.output_size = (size_t)DATASET_COUNT * sizeof(float),
		.args = &threshold,
		.args_size = sizeof(threshold),
	};

	status = csQueueComputeRequest(handle, &compute, CS_INVALID_HANDLE, NULL, NULL);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csQueueComputeRequest(filter) failed: %d\n", status);
		csFreeMem(handle, output_mem);
		csFreeMem(handle, input_mem);
		free(host_input);
		free(host_output);
		csCloseCSx(handle);
		csShutdown();
		return 1;
	}

	status = csQueueCopyMemRequest(handle, NULL, CS_INVALID_HANDLE,
				       (void *)(uintptr_t)output_mem, host_output,
				       (size_t)DATASET_COUNT * sizeof(float),
				       CS_COPY_DEVICE_TO_HOST);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csQueueCopyMemRequest(D2H) failed: %d\n", status);
		csFreeMem(handle, output_mem);
		csFreeMem(handle, input_mem);
		free(host_input);
		free(host_output);
		csCloseCSx(handle);
		csShutdown();
		return 1;
	}

	/* PAIN[P2]: API verbosity — there is no API-returned output length, so callers need ad-hoc conventions to infer filtered count. */
	size_t matched = count_non_nan(host_output, DATASET_COUNT);
	printf("Filtered matches (> %.2f): %zu\n", threshold, matched);

	status = csFreeMem(handle, output_mem);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csFreeMem(output) failed: %d\n", status);
	}

	status = csFreeMem(handle, input_mem);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csFreeMem(input) failed: %d\n", status);
	}

	free(host_input);
	free(host_output);

	status = csCloseCSx(handle);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csCloseCSx failed: %d\n", status);
		csShutdown();
		return 1;
	}

	status = csShutdown();
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csShutdown failed: %d\n", status);
		return 1;
	}

	return 0;
}
