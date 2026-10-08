/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file scenario_01_raw.c
 * @brief Scenario 01 - Hello Computational Storage
 */

#include <stdio.h>
#include <stdlib.h>

#include "cs_api.h"

#define CSF_ID_DOT_PRODUCT 5U

int
main(void)
{
	csStatus status;
	csCSxHandle handle = CS_INVALID_HANDLE;

	setenv("LIBCS_BACKEND", "sim", 1);

	/* PAIN[P2]: API verbosity — even a hello-world compute requires explicit init, open, descriptor setup, queue, close, and shutdown steps. */
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

	float input[] = {
		1.0f, 2.0f, 3.0f,
		4.0f, 5.0f, 6.0f,
	};
	float output = 0.0f;

	/* PAIN[P4]: Discoverability — CSF IDs are magic numbers unless the developer reads extra docs or defines local constants. */
	csComputeDesc desc = {
		.csf_id = CSF_ID_DOT_PRODUCT,
		.input_buffer = input,
		.input_size = sizeof(input),
		.output_buffer = &output,
		.output_size = sizeof(output),
		.args = NULL,
		.args_size = 0,
	};

	status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csQueueComputeRequest failed: %d\n", status);
		csCloseCSx(handle);
		csShutdown();
		return 1;
	}

	printf("Hello CS result: dot_product = %.2f\n", output);

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
