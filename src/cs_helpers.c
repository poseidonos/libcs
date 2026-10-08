/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_helpers.c
 * @brief Convenience helpers for CS API
 */

#include <math.h>
#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "cs_helpers.h"
#include "cs_internal.h"
#include "cs_programs.h"

bool
xnvme_uri_is_local_direct(const char *uri)
{
	if (!uri || uri[0] == '\0') {
		return false;
	}
	if (uri[0] == '/') {
		return true;
	}
	if (!strncmp(uri, "pci:", 4) || !strncmp(uri, "pcie:", 5)) {
		return true;
	}
	return false;
}
uint32_t
cs_element_size_for_csf(csCSFId csf_id)
{
	switch (csf_id) {
	case CS_CSF_SUM64:
	case CS_CSF_MAX64:
	case CS_CSF_MIN64:
		return sizeof(uint64_t);
	case CS_CSF_DOT_PRODUCT:
	case CS_CSF_FILTER_GT:
	case CS_CSF_L2_DISTANCE_SQ:
	case CS_CSF_COSINE_SIMILARITY:
		return sizeof(float);
	case CS_CSF_MULTI_AGG64:
		return sizeof(csMultiAgg64Result);
	default:
		return 1;
	}
}

size_t
cs_infer_bytes_produced(csCSFId csf_id,
			 const void *input, size_t input_size,
			 const void *output, size_t output_size)
{
	(void)input;

	switch (csf_id) {
	case CS_CSF_SUM64:
	case CS_CSF_MAX64:
	case CS_CSF_MIN64:
		return output_size >= sizeof(uint64_t) ? sizeof(uint64_t) : 0;
	case CS_CSF_DOT_PRODUCT:
	case CS_CSF_L2_DISTANCE_SQ:
	case CS_CSF_COSINE_SIMILARITY:
		return output_size >= sizeof(float) ? sizeof(float) : 0;
	case CS_CSF_MULTI_AGG64:
		return output_size >= sizeof(csMultiAgg64Result) ? sizeof(csMultiAgg64Result) : 0;
	case CS_CSF_MEMCPY:
		return input_size < output_size ? input_size : output_size;
	case CS_CSF_FILTER_GT: {
		if (!output || (output_size % sizeof(float)) != 0) {
			return 0;
		}
		const float *vals = (const float *)output;
		size_t max = output_size / sizeof(float);
		size_t count = 0;
		while (count < max && !isnan(vals[count])) {
			count++;
		}
		return count * sizeof(float);
	}
	case CS_CSF_RLE_COMPRESS: {
		uint64_t actual = cs_get_last_device_result();
		if (actual > 0 && actual <= output_size) {
			return (size_t)actual;
		}
		if (!output || (output_size % 2) != 0) {
			return 0;
		}
		const uint8_t *buf = (const uint8_t *)output;
		size_t len = 0;
		for (size_t i = 0; i + 1 < output_size; i += 2) {
			if (buf[i] == 0) {
				break;
			}
			len = i + 2;
		}
		return len;
	}
	default:
		return output_size;
	}
}

void
cs_prepare_variable_output(csCSFId csf_id, void *output, size_t output_size)
{
	if (!output || output_size == 0) {
		return;
	}

	if (csf_id == CS_CSF_FILTER_GT && (output_size % sizeof(float)) == 0) {
		float *vals = (float *)output;
		size_t count = output_size / sizeof(float);
		for (size_t i = 0; i < count; i++) {
			vals[i] = NAN;
		}
		return;
	}

	if (csf_id == CS_CSF_RLE_COMPRESS) {
		memset(output, 0, output_size);
	}
}

csStatus
csRunCompute(const char *uri, csCSFId csf_id,
	     const void *input, size_t input_size,
	     void *output, size_t output_size,
	     const void *args, size_t args_size)
{
	return csRunComputeEx(uri, csf_id, input, input_size,
			      output, output_size, args, args_size, NULL);
}

csStatus
csRunComputeEx(const char *uri, csCSFId csf_id,
	       const void *input, size_t input_size,
	       void *output, size_t output_size,
	       const void *args, size_t args_size,
	       csComputeResult *result)
{
	csStatus status = CS_STATUS_SUCCESS;
	csStatus first_error = CS_STATUS_SUCCESS;
	csCSxHandle dev = CS_INVALID_HANDLE;
	csMemHandle in_mem = CS_INVALID_HANDLE;
	csMemHandle out_mem = CS_INVALID_HANDLE;
	void *in_ptr = NULL;
	void *out_ptr = NULL;
	csComputeDesc desc;

	if (result) {
		result->status = CS_STATUS_ERROR_INVALID_PARAM;
		result->bytes_produced = 0;
		result->elements_produced = 0;
	}

	if (!uri || !input || input_size == 0 || !output || output_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	status = csInit();
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	status = csOpenCSx(uri, &dev);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	csMemDesc in_desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = input_size,
		.alignment = 0,
		.flags = 0,
	};
	status = csAllocMem(dev, &in_desc, &in_mem, &in_ptr);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	csMemDesc out_desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = output_size,
		.alignment = 0,
		.flags = 0,
	};
	status = csAllocMem(dev, &out_desc, &out_mem, &out_ptr);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	status = csQueueCopyMemRequest(dev, NULL, CS_INVALID_HANDLE,
				       (void *)input, (void *)(uintptr_t)in_mem,
				       input_size, CS_COPY_HOST_TO_DEVICE);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	cs_prepare_variable_output(csf_id, out_ptr, output_size);

	desc = csMakeComputeDescArgs(csf_id, in_ptr, input_size,
				     out_ptr, output_size,
				     args, args_size);
	status = csQueueComputeRequest(dev, &desc, CS_INVALID_HANDLE, NULL, NULL);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	status = csQueueCopyMemRequest(dev, NULL, CS_INVALID_HANDLE,
				       (void *)(uintptr_t)out_mem, output,
				       output_size, CS_COPY_DEVICE_TO_HOST);
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
		goto done;
	}

	if (result) {
		uint32_t element_size = cs_element_size_for_csf(csf_id);
		result->bytes_produced = cs_infer_bytes_produced(csf_id, input, input_size,
							output, output_size);
		result->elements_produced = element_size > 0 ?
			(uint32_t)(result->bytes_produced / element_size) : 0;
	}

done:
	if (out_mem != CS_INVALID_HANDLE) {
		status = csFreeMem(dev, out_mem);
		if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
			first_error = status;
		}
	}
	if (in_mem != CS_INVALID_HANDLE) {
		status = csFreeMem(dev, in_mem);
		if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
			first_error = status;
		}
	}
	if (dev != CS_INVALID_HANDLE) {
		status = csCloseCSx(dev);
		if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
			first_error = status;
		}
	}
	status = csShutdown();
	if (status != CS_STATUS_SUCCESS && first_error == CS_STATUS_SUCCESS) {
		first_error = status;
	}

	if (result) {
		result->status = first_error;
	}
	return first_error;
}

csStatus
csRunComputeAsync(csCSxHandle device, const csComputeDesc *desc,
		  uint32_t timeout_ms, csComputeResult *result)
{
	if (result) {
		result->status = CS_STATUS_ERROR_INVALID_PARAM;
		result->bytes_produced = 0;
		result->elements_produced = 0;
	}

	if (device == CS_INVALID_HANDLE || !desc) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_prepare_variable_output(desc->csf_id, desc->output_buffer, desc->output_size);

	csEventHandle event = CS_INVALID_HANDLE;
	csStatus status = csCreateEvent(device, &event);
	if (status != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = status;
		}
		return status;
	}

	status = csQueueComputeRequest(device, desc, event, NULL, NULL);
	if (status != CS_STATUS_SUCCESS && status != CS_STATUS_PENDING) {
		csDestroyEvent(device, event);
		if (result) {
			result->status = status;
		}
		return status;
	}

	csStatus completed = CS_STATUS_ERROR_BACKEND;
	status = csWaitEvent(device, event, timeout_ms, &completed);
	csStatus destroy_status = csDestroyEvent(device, event);

	if (status != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = status;
		}
		return status;
	}
	if (completed != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = completed;
		}
		return completed;
	}
	if (destroy_status != CS_STATUS_SUCCESS) {
		if (result) {
			result->status = destroy_status;
		}
		return destroy_status;
	}

	if (result) {
		uint32_t element_size = cs_element_size_for_csf(desc->csf_id);
		result->status = CS_STATUS_SUCCESS;
		result->bytes_produced = cs_infer_bytes_produced(desc->csf_id,
							 desc->input_buffer, desc->input_size,
							 desc->output_buffer, desc->output_size);
		result->elements_produced = element_size > 0 ?
			(uint32_t)(result->bytes_produced / element_size) : 0;
	}

	return CS_STATUS_SUCCESS;
}

csStatus
csScopeOpen(csScope *scope, const char *uri)
{
	if (!scope || !uri) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	memset(scope, 0, sizeof(*scope));
	scope->device = CS_INVALID_HANDLE;
	for (int i = 0; i < CS_SCOPE_MAX_MEMS; i++) {
		scope->mems[i] = CS_INVALID_HANDLE;
	}
	for (int i = 0; i < CS_SCOPE_MAX_EVENTS; i++) {
		scope->events[i] = CS_INVALID_HANDLE;
	}

	csStatus status = csInit();
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	status = csOpenCSx(uri, &scope->device);
	if (status != CS_STATUS_SUCCESS) {
		csShutdown();
		return status;
	}

	return CS_STATUS_SUCCESS;
}

void *
csScopeAllocMem(csScope *scope, size_t size, csMemHandle *out_handle)
{
	if (out_handle) {
		*out_handle = CS_INVALID_HANDLE;
	}
	if (!scope || scope->device == CS_INVALID_HANDLE || size == 0) {
		return NULL;
	}
	if (scope->mem_count >= CS_SCOPE_MAX_MEMS) {
		return NULL;
	}

	csMemDesc desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = size,
		.alignment = 0,
		.flags = 0,
	};

	csMemHandle mem = CS_INVALID_HANDLE;
	void *ptr = NULL;
	csStatus status = csAllocMem(scope->device, &desc, &mem, &ptr);
	if (status != CS_STATUS_SUCCESS) {
		return NULL;
	}

	scope->mems[scope->mem_count] = mem;
	scope->mem_ptrs[scope->mem_count] = ptr;
	scope->mem_count++;

	if (out_handle) {
		*out_handle = mem;
	}
	return ptr;
}

void
csScopeClose(csScope *scope)
{
	if (!scope) {
		return;
	}

	for (int i = scope->event_count - 1; i >= 0; i--) {
		if (scope->events[i] != CS_INVALID_HANDLE && scope->device != CS_INVALID_HANDLE) {
			csDestroyEvent(scope->device, scope->events[i]);
		}
		scope->events[i] = CS_INVALID_HANDLE;
	}
	scope->event_count = 0;

	for (int i = scope->mem_count - 1; i >= 0; i--) {
		if (scope->mems[i] != CS_INVALID_HANDLE && scope->device != CS_INVALID_HANDLE) {
			csFreeMem(scope->device, scope->mems[i]);
		}
		scope->mems[i] = CS_INVALID_HANDLE;
		scope->mem_ptrs[i] = NULL;
	}
	scope->mem_count = 0;

	if (scope->device != CS_INVALID_HANDLE) {
		csCloseCSx(scope->device);
		scope->device = CS_INVALID_HANDLE;
	}

	csShutdown();
}

csComputeDesc
csMakeComputeDesc(csCSFId csf_id,
		  const void *input, size_t input_size,
		  void *output, size_t output_size)
{
	return csMakeComputeDescArgs(csf_id, input, input_size,
				    output, output_size,
				    NULL, 0);
}

csComputeDesc
csMakeComputeDescArgs(csCSFId csf_id,
		      const void *input, size_t input_size,
		      void *output, size_t output_size,
		      const void *args, size_t args_size)
{
	csComputeDesc desc;
	memset(&desc, 0, sizeof(desc));
	desc.csf_id = csf_id;
	desc.input_buffer = input;
	desc.input_size = input_size;
	desc.output_buffer = output;
	desc.output_size = output_size;
	desc.args = args;
	desc.args_size = args_size;
	return desc;
}
