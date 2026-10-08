/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file scenario_02_improved.c
 * @brief Improved version of scenario_02 - uses cs_helpers.h
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cs_helpers.h"

#define DATASET_COUNT 1000000U

int
main(void)
{
	setenv("LIBCS_BACKEND", "sim", 1);

	csScope scope;
	csStatus status = csScopeOpen(&scope, "sim://libcs0");
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csScopeOpen failed: %d\n", status);
		return 1;
	}

	float *input = csScopeAllocMem(&scope, (size_t)DATASET_COUNT * sizeof(float), NULL);
	float *output = csScopeAllocMem(&scope, (size_t)DATASET_COUNT * sizeof(float), NULL);
	if (!input || !output) {
		fprintf(stderr, "csScopeAllocMem failed\n");
		csScopeClose(&scope);
		return 1;
	}

	for (size_t i = 0; i < DATASET_COUNT; i++) {
		input[i] = (float)rand() / ((float)RAND_MAX + 1.0f);
		output[i] = NAN;
	}

	float threshold = 0.75f;
	csComputeResult result;
	status = csRunComputeEx("sim://libcs0", CS_CSF_FILTER_GT,
				input, (size_t)DATASET_COUNT * sizeof(float),
				output, (size_t)DATASET_COUNT * sizeof(float),
				&threshold, sizeof(threshold),
				&result);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csRunComputeEx failed: %d\n", status);
		csScopeClose(&scope);
		return 1;
	}

	printf("Filtered matches (> %.2f): %u\n", threshold, result.elements_produced);
	csScopeClose(&scope);
	return 0;
}
