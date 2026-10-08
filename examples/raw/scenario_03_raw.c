/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file scenario_03_raw.c
 * @brief Scenario 03 - Pipeline Two Operations
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_api.h"

#define CSF_ID_FILTER_GT 6U
#define CSF_ID_DOT_PRODUCT 5U

int
main(void)
{
	csStatus status;
	csCSxHandle handle = CS_INVALID_HANDLE;

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

	float input[2048];
	float filtered[2048];
	float dot_result = 0.0f;
	float threshold = 0.75f;

	for (size_t i = 0; i < 1024; i++) {
		input[i] = 0.80f + (float)(i % 5) * 0.01f;
	}
	for (size_t i = 1024; i < 2048; i++) {
		input[i] = 0.10f + (float)(i % 5) * 0.01f;
	}
	for (size_t i = 0; i < 2048; i++) {
		filtered[i] = NAN;
	}

	/* PAIN[P4]: Discoverability — csRequest uses a deep union and nested fields that are difficult to read without repeatedly checking struct definitions. */
	csRequest batch[2];
	memset(batch, 0, sizeof(batch));

	batch[0].type = CS_REQ_TYPE_COMPUTE;
	batch[0].u.compute.desc.csf_id = CSF_ID_FILTER_GT;
	batch[0].u.compute.desc.input_buffer = input;
	batch[0].u.compute.desc.input_size = sizeof(input);
	batch[0].u.compute.desc.output_buffer = filtered;
	batch[0].u.compute.desc.output_size = sizeof(filtered);
	batch[0].u.compute.desc.args = &threshold;
	batch[0].u.compute.desc.args_size = sizeof(threshold);

	/* PAIN[P2]: API verbosity — chaining operations still requires manually materializing intermediate buffers and filling full descriptors for each stage. */
	batch[1].type = CS_REQ_TYPE_COMPUTE;
	batch[1].u.compute.desc.csf_id = CSF_ID_DOT_PRODUCT;
	batch[1].u.compute.desc.input_buffer = filtered;
	batch[1].u.compute.desc.input_size = 1024 * sizeof(float);
	batch[1].u.compute.desc.output_buffer = &dot_result;
	batch[1].u.compute.desc.output_size = sizeof(dot_result);
	batch[1].u.compute.desc.args = NULL;
	batch[1].u.compute.desc.args_size = 0;

	status = csQueueBatchRequest(handle, NULL, CS_INVALID_HANDLE, batch, 2);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csQueueBatchRequest failed: %d\n", status);
		csCloseCSx(handle);
		csShutdown();
		return 1;
	}

	/* PAIN[P6]: Result semantics — the API does not expose per-stage outputs or counts in batch mode, making correctness checks manual and brittle. */
	printf("Pipeline result: dot(filtered[0:512], filtered[512:1024]) = %.4f\n", dot_result);

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
