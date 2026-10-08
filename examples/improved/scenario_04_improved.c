/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file scenario_04_improved.c
 * @brief Improved version of scenario_04 - uses cs_helpers.h
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cs_helpers.h"

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

	uint8_t *input = csScopeAllocMem(&scope, 1024, NULL);
	uint8_t *output = csScopeAllocMem(&scope, 2048, NULL);
	if (!input || !output) {
		fprintf(stderr, "csScopeAllocMem failed\n");
		csScopeClose(&scope);
		return 1;
	}

	for (size_t i = 0; i < 1024; i++) {
		input[i] = (uint8_t)((i / 16) & 0xFF);
	}

	csComputeDesc desc = csMakeComputeDesc(CS_CSF_RLE_COMPRESS,
					      input, 1024,
					      output, 2048);
	csComputeResult result;
	status = csRunComputeAsync(scope.device, &desc, 1000, &result);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "Production integration failed: %d\n", status);
		csScopeClose(&scope);
		return 1;
	}

	printf("RLE completed: input=%d bytes, encoded=%zu bytes\n", 1024, result.bytes_produced);
	csScopeClose(&scope);
	return 0;
}
