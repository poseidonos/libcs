/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file scenario_04_raw.c
 * @brief Scenario 04 - Production-Grade Usage
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_api.h"

#define CSF_ID_RLE_COMPRESS 8U

static size_t
encoded_length_from_pairs(const uint8_t *buffer, size_t max_len)
{
	size_t len = 0;
	for (size_t i = 0; i + 1 < max_len; i += 2) {
		if (buffer[i] == 0) {
			break;
		}
		len = i + 2;
	}
	return len;
}

static csStatus
compress_buffer_with_event(const uint8_t *input, size_t input_size,
			   uint8_t *output, size_t output_size,
			   size_t *encoded_size)
{
	csStatus status;
	csCSxHandle handle = CS_INVALID_HANDLE;
	csEventHandle event = CS_INVALID_HANDLE;
	csStatus event_result = CS_STATUS_ERROR_BACKEND;
	csComputeDesc desc;

	if (!input || input_size == 0 || !output || output_size == 0 || !encoded_size) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	setenv("LIBCS_BACKEND", "sim", 1);

	status = csInit();
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "[prod] csInit failed: %d\n", status);
		return status;
	}

	status = csOpenCSx("sim://libcs0", &handle);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "[prod] csOpenCSx failed: %d\n", status);
		goto out_shutdown;
	}

	status = csCreateEvent(handle, &event);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "[prod] csCreateEvent failed: %d\n", status);
		goto out_close;
	}

	memset(output, 0, output_size);
	memset(&desc, 0, sizeof(desc));
	desc.csf_id = CSF_ID_RLE_COMPRESS;
	desc.input_buffer = input;
	desc.input_size = input_size;
	desc.output_buffer = output;
	desc.output_size = output_size;
	desc.args = NULL;
	desc.args_size = 0;

	status = csQueueComputeRequest(handle, &desc, event, NULL, NULL);
	if (status != CS_STATUS_SUCCESS && status != CS_STATUS_PENDING) {
		fprintf(stderr, "[prod] csQueueComputeRequest failed: %d\n", status);
		goto out_event;
	}

	/* PAIN[P3]: Error handling boilerplate — async path requires explicit event creation, wait, status extraction, and multiple checks for one operation. */
	status = csWaitEvent(handle, event, 1000, &event_result);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "[prod] csWaitEvent failed: %d\n", status);
		goto out_event;
	}
	if (event_result != CS_STATUS_SUCCESS) {
		fprintf(stderr, "[prod] event completion reported failure: %d\n", event_result);
		status = event_result;
		goto out_event;
	}

	*encoded_size = encoded_length_from_pairs(output, output_size);
	if (*encoded_size == 0) {
		fprintf(stderr, "[prod] encoded output is empty\n");
		status = CS_STATUS_ERROR_BACKEND;
		goto out_event;
	}

	status = CS_STATUS_SUCCESS;

out_event:
	/* PAIN[P5]: Resource cleanup complexity — cleanup requires ordered teardown labels and duplicated control flow across every failure path. */
	if (event != CS_INVALID_HANDLE) {
		csStatus destroy_status = csDestroyEvent(handle, event);
		if (destroy_status != CS_STATUS_SUCCESS) {
			fprintf(stderr, "[prod] csDestroyEvent failed: %d\n", destroy_status);
			if (status == CS_STATUS_SUCCESS) {
				status = destroy_status;
			}
		}
	}

out_close:
	if (handle != CS_INVALID_HANDLE) {
		csStatus close_status = csCloseCSx(handle);
		if (close_status != CS_STATUS_SUCCESS) {
			fprintf(stderr, "[prod] csCloseCSx failed: %d\n", close_status);
			if (status == CS_STATUS_SUCCESS) {
				status = close_status;
			}
		}
	}

out_shutdown:
	{
		csStatus shutdown_status = csShutdown();
		if (shutdown_status != CS_STATUS_SUCCESS) {
			fprintf(stderr, "[prod] csShutdown failed: %d\n", shutdown_status);
			if (status == CS_STATUS_SUCCESS) {
				status = shutdown_status;
			}
		}
	}

	return status;
}

int
main(void)
{
	uint8_t input[1024];
	uint8_t output[2048];
	size_t encoded_size = 0;

	for (size_t i = 0; i < sizeof(input); i++) {
		input[i] = (uint8_t)((i / 16) & 0xFF);
	}

	csStatus status = compress_buffer_with_event(input, sizeof(input),
					     output, sizeof(output),
					     &encoded_size);
	if (status != CS_STATUS_SUCCESS) {
		fprintf(stderr, "Production integration failed: %d\n", status);
		return 1;
	}

	printf("RLE completed: input=%zu bytes, encoded=%zu bytes\n",
	       sizeof(input), encoded_size);
	return 0;
}
