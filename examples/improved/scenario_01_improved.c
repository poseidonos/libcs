/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file scenario_01_improved.c
 * @brief Improved version of scenario_01 - uses cs_helpers.h
 */

#include <stdio.h>
#include <stdlib.h>

#include "cs_helpers.h"

int
main(void)
{
	setenv("LIBCS_BACKEND", "sim", 1);

	float input[] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f };
	float output = 0.0f;

	csStatus status = csRunCompute("sim://libcs0", CS_CSF_DOT_PRODUCT,
				       input, sizeof(input),
				       &output, sizeof(output),
				       NULL, 0);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "csRunCompute failed: %d\n", status);
		return 1;
	}

	printf("Hello CS result: dot_product = %.2f\n", output);
	return 0;
}
