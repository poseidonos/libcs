/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file scenario_03_improved.c
 * @brief Improved version of scenario_03 - uses cs_helpers.h
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cs_helpers.h"

int
main(void)
{
	setenv("LIBCS_BACKEND", "sim", 1);

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

	csComputeDesc d1 = csMakeComputeDescArgs(CS_CSF_FILTER_GT,
					 input, sizeof(input),
					 filtered, sizeof(filtered),
					 &threshold, sizeof(threshold));
	csComputeResult r1;
	csStatus status = csRunComputeEx("sim://libcs0", d1.csf_id,
					 d1.input_buffer, d1.input_size,
					 d1.output_buffer, d1.output_size,
					 d1.args, d1.args_size,
					 &r1);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "filter stage failed: %d\n", status);
		return 1;
	}

	csComputeDesc d2 = csMakeComputeDesc(CS_CSF_DOT_PRODUCT,
				     filtered, 1024 * sizeof(float),
				     &dot_result, sizeof(dot_result));
	csComputeResult r2;
	status = csRunComputeEx("sim://libcs0", d2.csf_id,
				d2.input_buffer, d2.input_size,
				d2.output_buffer, d2.output_size,
				d2.args, d2.args_size,
				&r2);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "dot stage failed: %d\n", status);
		return 1;
	}

	printf("Pipeline result: dot(filtered[0:512], filtered[512:1024]) = %.4f\n", dot_result);
	return 0;
}
