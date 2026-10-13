/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file test_cs_api.c
 * @brief CS API Unit Tests
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "cs_api.h"
#include "cs_helpers.h"
#include "cs_internal.h"

#define CSF_ID_SUM64 2U
#define CSF_ID_MAX64 3U
#define CSF_ID_MIN64 4U
#define CSF_ID_DOT_PRODUCT 5U
#define CSF_ID_FILTER_GT 6U
#define CSF_ID_MEMCPY 7U
#define CSF_ID_RLE_COMPRESS 8U
#define CSF_ID_MULTI_AGG64 9U
#define CSF_ID_L2_DISTANCE_SQ 10U
#define CSF_ID_COSINE_SIMILARITY 11U

struct sim_multi_agg64_result {
	uint64_t count;
	double sum;
	double min;
	double max;
};

#include "test_util.h"

#define ASSERT_EQ(expected, actual, msg) \
	do { \
		if ((expected) != (actual)) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "%s (expected %d, got %d)", \
				 msg, (int)(expected), (int)(actual)); \
			TEST_FAIL(buf); \
			return; \
		} \
	} while (0)

#define ASSERT_NE(not_expected, actual, msg) \
	do { \
		if ((not_expected) == (actual)) { \
			TEST_FAIL(msg); \
			return; \
		} \
	} while (0)

#define ASSERT_NOT_NULL(ptr, msg) \
	do { \
		if ((ptr) == NULL) { \
			TEST_FAIL(msg); \
			return; \
		} \
	} while (0)

static void
set_sim_backend(void)
{
	/* Keep tests deterministic regardless of host environment. */
	setenv("LIBCS_BACKEND", "sim", 1);
}

/**
 * Test: Library version
 */
static void
test_version(void)
{
	TEST_START("csGetVersion returns valid string");

	const char *version = csGetVersion();
	ASSERT_NOT_NULL(version, "version is NULL");

	if (strlen(version) == 0) {
		TEST_FAIL("version is empty");
		return;
	}

	TEST_PASS();
}

/**
 * Test: Library initialization
 */
static void
test_init_shutdown(void)
{
	TEST_START("csInit and csShutdown");

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	/* Double init should succeed */
	status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "double csInit failed");

	status = csShutdown();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csShutdown failed");

	/* Double shutdown should succeed */
	status = csShutdown();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "double csShutdown failed");

	TEST_PASS();
}

/**
 * Test: Device discovery
 */
static void
test_device_discovery(void)
{
	TEST_START("csQueryCSxList");

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	/* Query device count */
	uint32_t count = 0;
	status = csQueryCSxList(NULL, &count);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueryCSxList count failed");

	if (count == 0) {
		printf("[INFO: no devices, set LIBCS_DEVICE_URI] ");
	}

	/* Query device info if any devices */
	if (count > 0) {
		csDeviceInfo *devices = calloc(count, sizeof(csDeviceInfo));
		ASSERT_NOT_NULL(devices, "malloc failed");

		status = csQueryCSxList(devices, &count);
		ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueryCSxList info failed");

		printf("[found %u device(s)] ", count);
		free(devices);
	}

	csShutdown();
	TEST_PASS();
}

/**
 * Test: Invalid handle operations
 */
static void
test_invalid_handle(void)
{
	TEST_START("Invalid handle rejection");

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	/* Close invalid handle */
	status = csCloseCSx(CS_INVALID_HANDLE);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_HANDLE, status, "expected invalid handle error");

	/* Close random handle */
	status = csCloseCSx(0x12345678ABCDEF00ULL);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_HANDLE, status, "expected invalid handle error");

	csShutdown();
	TEST_PASS();
}

/**
 * Test: Event lifecycle (without device)
 */
static void
test_event_invalid(void)
{
	TEST_START("Event with invalid device");

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csEventHandle event;
	status = csCreateEvent(CS_INVALID_HANDLE, &event);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_HANDLE, status, "expected invalid handle");

	csShutdown();
	TEST_PASS();
}

/**
 * Test: Device open/close (requires LIBCS_DEVICE_URI)
 */
static void
test_device_lifecycle(void)
{
	TEST_START("Device open/close lifecycle");

	const char *uri = getenv("LIBCS_DEVICE_URI");
	if (!uri) {
		TEST_SKIP("LIBCS_DEVICE_URI not set");
		return;
	}

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	/* Open device */
	csCSxHandle handle;
	status = csOpenCSx(uri, &handle);
	if (status != CS_STATUS_SUCCESS) {
		printf("[cannot open device: %d] ", status);
		csShutdown();
		TEST_SKIP("device open failed");
		return;
	}

	ASSERT_NE(CS_INVALID_HANDLE, handle, "got invalid handle");

	/* Get device info */
	csDeviceInfo info;
	status = csGetCSxInfo(handle, &info);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGetCSxInfo failed");

	/* Close device */
	status = csCloseCSx(handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csCloseCSx failed");

	/* Double close should fail */
	status = csCloseCSx(handle);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_HANDLE, status, "double close should fail");

	csShutdown();
	TEST_PASS();
}

/**
 * Test: Memory allocation (requires device)
 */
static void
test_memory_allocation(void)
{
	TEST_START("Memory allocation/free");

	const char *uri = getenv("LIBCS_DEVICE_URI");
	if (!uri) {
		TEST_SKIP("LIBCS_DEVICE_URI not set");
		return;
	}

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx(uri, &handle);
	if (status != CS_STATUS_SUCCESS) {
		csShutdown();
		TEST_SKIP("device open failed");
		return;
	}

	/* Allocate memory */
	csMemDesc desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = 4096,
		.alignment = 0,
		.flags = 0,
	};

	csMemHandle mem_handle;
	void *ptr = NULL;
	status = csAllocMem(handle, &desc, &mem_handle, &ptr);
	if (status != CS_STATUS_SUCCESS) {
		printf("[alloc failed: %d] ", status);
		csCloseCSx(handle);
		csShutdown();
		TEST_SKIP("memory allocation not supported");
		return;
	}

	ASSERT_NOT_NULL(ptr, "got NULL pointer");
	ASSERT_NE(CS_INVALID_HANDLE, mem_handle, "got invalid mem handle");

	/* Write and read back */
	memset(ptr, 0xAB, 4096);
	if (((uint8_t *)ptr)[0] != 0xAB) {
		TEST_FAIL("memory write/read failed");
		csFreeMem(handle, mem_handle);
		csCloseCSx(handle);
		csShutdown();
		return;
	}

	/* Free memory */
	status = csFreeMem(handle, mem_handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csFreeMem failed");

	/* Double free should fail */
	status = csFreeMem(handle, mem_handle);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_HANDLE, status, "double free should fail");

	csCloseCSx(handle);
	csShutdown();
	TEST_PASS();
}

/**
 * Test: Event lifecycle (requires device)
 */
static void
test_event_lifecycle(void)
{
	TEST_START("Event create/poll/destroy");

	const char *uri = getenv("LIBCS_DEVICE_URI");
	if (!uri) {
		TEST_SKIP("LIBCS_DEVICE_URI not set");
		return;
	}

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx(uri, &handle);
	if (status != CS_STATUS_SUCCESS) {
		csShutdown();
		TEST_SKIP("device open failed");
		return;
	}

	/* Create event */
	csEventHandle event;
	status = csCreateEvent(handle, &event);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csCreateEvent failed");
	ASSERT_NE(CS_INVALID_HANDLE, event, "got invalid event handle");

	/* Poll event (should be pending) */
	csEventStatus evt_status;
	csStatus result;
	status = csPollEvent(handle, event, &evt_status, &result);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csPollEvent failed");
	ASSERT_EQ(CS_EVENT_STATUS_PENDING, evt_status, "event should be pending");

	/* Destroy event */
	status = csDestroyEvent(handle, event);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csDestroyEvent failed");

	/* Double destroy should fail */
	status = csDestroyEvent(handle, event);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_HANDLE, status, "double destroy should fail");

	csCloseCSx(handle);
	csShutdown();
	TEST_PASS();
}

/**
 * Test: CSF ID retrieval
 */
static void
test_csf_id(void)
{
	TEST_START("CSF ID retrieval");

	const char *uri = getenv("LIBCS_DEVICE_URI");
	if (!uri) {
		TEST_SKIP("LIBCS_DEVICE_URI not set");
		return;
	}

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx(uri, &handle);
	if (status != CS_STATUS_SUCCESS) {
		csShutdown();
		TEST_SKIP("device open failed");
		return;
	}

	/* Get CSF ID for program index 0 */
	csCSFId csf_id;
	status = csGetCSFId(handle, 0, &csf_id);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGetCSFId failed");

	csCloseCSx(handle);
	csShutdown();
	TEST_PASS();
}

/**
 * Test: Compute argument helper
 */
static void
test_compute_arg_helper(void)
{
	TEST_START("csHelperSetComputeArg");

	uint8_t args[64];
	memset(args, 0, sizeof(args));

	uint64_t value = 0x123456789ABCDEF0ULL;
	csStatus status = csHelperSetComputeArg(args, sizeof(args), 0, &value, sizeof(value));
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "arg set failed");

	/* Verify value was written */
	uint64_t *read_value = (uint64_t *)args;
	if (*read_value != value) {
		TEST_FAIL("arg value mismatch");
		return;
	}

	/* Test out of bounds */
	status = csHelperSetComputeArg(args, sizeof(args), 100, &value, sizeof(value));
	ASSERT_EQ(CS_STATUS_ERROR_LIMIT, status, "expected limit error");

	/* Test NULL buffer */
	status = csHelperSetComputeArg(NULL, sizeof(args), 0, &value, sizeof(value));
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_PARAM, status, "expected invalid param");

	TEST_PASS();
}

/**
 * Test: Compute request (sync, requires device with loaded program)
 */
static void
test_compute_sync(void)
{
	TEST_START("Sync compute request");

	const char *uri = getenv("LIBCS_DEVICE_URI");
	if (!uri) {
		TEST_SKIP("LIBCS_DEVICE_URI not set");
		return;
	}

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx(uri, &handle);
	if (status != CS_STATUS_SUCCESS) {
		csShutdown();
		TEST_SKIP("device open failed");
		return;
	}

	/* Allocate input buffer */
	csMemDesc desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = 4096,
		.alignment = 0,
		.flags = 0,
	};

	csMemHandle input_mem;
	void *input_ptr = NULL;
	status = csAllocMem(handle, &desc, &input_mem, &input_ptr);
	if (status != CS_STATUS_SUCCESS) {
		csCloseCSx(handle);
		csShutdown();
		TEST_SKIP("memory allocation failed");
		return;
	}

	/* Allocate output buffer */
	csMemHandle output_mem;
	void *output_ptr = NULL;
	status = csAllocMem(handle, &desc, &output_mem, &output_ptr);
	if (status != CS_STATUS_SUCCESS) {
		csFreeMem(handle, input_mem);
		csCloseCSx(handle);
		csShutdown();
		TEST_SKIP("output memory allocation failed");
		return;
	}

	/* Initialize input */
	memset(input_ptr, 0x55, 4096);

	/* Create compute descriptor */
	csCSFId csf_id;
	csGetCSFId(handle, 0, &csf_id);

	csComputeDesc compute_desc = {
		.csf_id = csf_id,
		.input_buffer = input_ptr,
		.input_size = 4096,
		.output_buffer = output_ptr,
		.output_size = 4096,
		.args = NULL,
		.args_size = 0,
	};

	/* Execute synchronously */
	status = csQueueComputeRequest(handle, &compute_desc, CS_INVALID_HANDLE, NULL, NULL);

	/* Note: This will likely fail without a real CS device with loaded program */
	if (status == CS_STATUS_SUCCESS) {
		printf("[compute succeeded] ");
	} else if (status == CS_STATUS_ERROR_IO || status == CS_STATUS_ERROR_BACKEND) {
		printf("[expected: no program loaded] ");
	} else {
		printf("[status=%d] ", status);
	}

	/* Cleanup */
	csFreeMem(handle, output_mem);
	csFreeMem(handle, input_mem);
	csCloseCSx(handle);
	csShutdown();
	TEST_PASS();
}

/**
 * Test: Simulator device open/info
 */
static void
test_sim_device_open(void)
{
	TEST_START("SIM backend device open/info");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");
	ASSERT_NE(CS_INVALID_HANDLE, handle, "invalid sim device handle");

	csDeviceInfo info;
	status = csGetCSxInfo(handle, &info);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGetCSxInfo failed");

	if (strcmp(info.vendor, "SimVendor") != 0) {
		TEST_FAIL("unexpected sim vendor");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}
	if (strcmp(info.model, "CS-Sim-v1") != 0) {
		TEST_FAIL("unexpected sim model");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}
	if (info.max_programs != 32 || info.max_memory_mb != 256) {
		TEST_FAIL("unexpected sim device limits");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	status = csCloseCSx(handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csCloseCSx failed");

	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/**
 * Test: Simulator dot-product CSF
 */
static void
test_sim_dot_product(void)
{
	TEST_START("SIM CSF dot product");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	float input[] = {
		1.0f, 2.0f, 3.0f,
		4.0f, 5.0f, 6.0f,
	};
	float output = 0.0f;
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
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "dot-product execute failed");

	if (output != 32.0f) {
		TEST_FAIL("dot-product result mismatch");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_integer_reductions(void)
{
	TEST_START("SIM CSF sum/max/min reductions");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	uint64_t input[] = { 3, 7, 11, 5 };
	uint64_t output = 0;
	csComputeDesc desc = {
		.input_buffer = input,
		.input_size = sizeof(input),
		.output_buffer = &output,
		.output_size = sizeof(output),
		.args = NULL,
		.args_size = 0,
	};

	desc.csf_id = CSF_ID_SUM64;
	status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "sum64 execute failed");
	if (output != 26) {
		TEST_FAIL("sum64 result mismatch");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	desc.csf_id = CSF_ID_MAX64;
	status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "max64 execute failed");
	if (output != 11) {
		TEST_FAIL("max64 result mismatch");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	desc.csf_id = CSF_ID_MIN64;
	status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "min64 execute failed");
	if (output != 3) {
		TEST_FAIL("min64 result mismatch");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_vector_similarity(void)
{
	TEST_START("SIM CSF l2/cosine vector similarity");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	float input[] = {
		1.0f, 2.0f, 3.0f,
		4.0f, 5.0f, 6.0f,
	};
	float output = 0.0f;
	csComputeDesc desc = {
		.input_buffer = input,
		.input_size = sizeof(input),
		.output_buffer = &output,
		.output_size = sizeof(output),
		.args = NULL,
		.args_size = 0,
	};

	desc.csf_id = CSF_ID_L2_DISTANCE_SQ;
	status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "l2 execute failed");
	if (fabsf(output - 27.0f) > 1e-5f) {
		TEST_FAIL("l2 distance result mismatch");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	desc.csf_id = CSF_ID_COSINE_SIMILARITY;
	status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "cosine execute failed");
	if (fabsf(output - 0.9746318f) > 1e-5f) {
		TEST_FAIL("cosine similarity result mismatch");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_multi_agg64(void)
{
	TEST_START("SIM CSF multi_agg64");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	double input[] = { 1.5, 3.0, 2.5, 7.0 };
	struct sim_multi_agg64_result output;
	memset(&output, 0, sizeof(output));

	csComputeDesc desc = {
		.csf_id = CSF_ID_MULTI_AGG64,
		.input_buffer = input,
		.input_size = sizeof(input),
		.output_buffer = &output,
		.output_size = sizeof(output),
		.args = NULL,
		.args_size = 0,
	};

	status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "multi_agg execute failed");

	if (output.count != 4 || fabs(output.sum - 14.0) > 1e-9 || fabs(output.min - 1.5) > 1e-9 ||
	    fabs(output.max - 7.0) > 1e-9) {
		TEST_FAIL("multi_agg result mismatch");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/**
 * Test: Simulator filter-greater-than CSF
 */
static void
test_sim_filter_gt(void)
{
	TEST_START("SIM CSF filter greater-than");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	float input[] = { 1.0f, 6.0f, 3.0f, 8.0f, 10.0f };
	float output[5] = { 0 };
	float threshold = 5.0f;

	csComputeDesc desc = {
		.csf_id = CSF_ID_FILTER_GT,
		.input_buffer = input,
		.input_size = sizeof(input),
		.output_buffer = output,
		.output_size = sizeof(output),
		.args = &threshold,
		.args_size = sizeof(threshold),
	};

	status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "filter execute failed");

	if (output[0] != 6.0f || output[1] != 8.0f || output[2] != 10.0f) {
		TEST_FAIL("filter output mismatch");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

struct sim_callback_state {
	int called;
	csStatus status;
};

static void
sim_compute_callback(csStatus status, void *user_data)
{
	struct sim_callback_state *cb = (struct sim_callback_state *)user_data;
	if (!cb) {
		return;
	}

	cb->called++;
	cb->status = status;
}

/**
 * Test: Simulator compute callback/event behavior
 */
static void
test_sim_async_callback(void)
{
	TEST_START("SIM async callback/event completion");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	csEventHandle event;
	status = csCreateEvent(handle, &event);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csCreateEvent failed");

	uint8_t input[] = { 1, 2, 3, 4, 5, 6 };
	uint8_t output[sizeof(input)];
	memset(output, 0, sizeof(output));

	struct sim_callback_state cb_state = {
		.called = 0,
		.status = CS_STATUS_ERROR_BACKEND,
	};

	csComputeDesc desc = {
		.csf_id = CSF_ID_MEMCPY,
		.input_buffer = input,
		.input_size = sizeof(input),
		.output_buffer = output,
		.output_size = sizeof(output),
		.args = NULL,
		.args_size = 0,
	};

	status = csQueueComputeRequest(handle, &desc, event, sim_compute_callback, &cb_state);
	ASSERT_EQ(CS_STATUS_PENDING, status, "expected PENDING from async compute");

	/* Poll for completion — callback fires inside csPollEvent */
	csEventStatus evt_status;
	csStatus evt_result;
	status = csPollEvent(handle, event, &evt_status, &evt_result);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csPollEvent failed");
	ASSERT_EQ(CS_EVENT_STATUS_COMPLETE, evt_status, "event should be complete");
	ASSERT_EQ(CS_STATUS_SUCCESS, evt_result, "event completion result mismatch");

	if (cb_state.called != 1 || cb_state.status != CS_STATUS_SUCCESS) {
		TEST_FAIL("callback was not called with success");
		csDestroyEvent(handle, event);
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	if (memcmp(input, output, sizeof(input)) != 0) {
		TEST_FAIL("memcpy output mismatch");
		csDestroyEvent(handle, event);
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csDestroyEvent(handle, event);
	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/**
 * Test: Simulator RLE compression CSF
 */
static void
test_sim_rle_compress(void)
{
	TEST_START("SIM CSF RLE compress");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	uint8_t input[] = { 0x41, 0x41, 0x41, 0x42, 0x42 };
	uint8_t output[16];
	memset(output, 0, sizeof(output));

	csComputeDesc desc = {
		.csf_id = CSF_ID_RLE_COMPRESS,
		.input_buffer = input,
		.input_size = sizeof(input),
		.output_buffer = output,
		.output_size = sizeof(output),
		.args = NULL,
		.args_size = 0,
	};

	status = csQueueComputeRequest(handle, &desc, CS_INVALID_HANDLE, NULL, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "RLE execute failed");

	uint8_t expected[] = { 0x03, 0x41, 0x02, 0x42 };
	if (memcmp(output, expected, sizeof(expected)) != 0) {
		TEST_FAIL("RLE output mismatch");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	/* Count bytes by scanning run-count positions (0 count marks end). */
	size_t encoded_len = 0;
	for (size_t i = 0; i + 1 < sizeof(output); i += 2) {
		if (output[i] == 0) {
			break;
		}
		encoded_len = i + 2;
	}
	if (encoded_len != 4) {
		TEST_FAIL("unexpected RLE encoded length");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/* =========================================================
 * Tier 1-2: Extended API functions
 * ========================================================= */

static void
test_sim_get_csf_id_by_name(void)
{
	TEST_START("SIM csGetCSFIdByName");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	csCSFId csf_id;
	status = csGetCSFIdByName(handle, "SUM64", &csf_id);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGetCSFIdByName(SUM64) failed");
	ASSERT_EQ(CSF_ID_SUM64, csf_id, "wrong csf_id for SUM64");

	status = csGetCSFIdByName(handle, "COSINE_SIMILARITY", &csf_id);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGetCSFIdByName(COSINE) failed");
	ASSERT_EQ(CSF_ID_COSINE_SIMILARITY, csf_id, "wrong csf_id for COSINE");

	status = csGetCSFIdByName(handle, "NONEXISTENT", &csf_id);
	ASSERT_EQ(CS_STATUS_ERROR_NOT_FOUND, status, "expected NOT_FOUND");

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_query_csf_list(void)
{
	TEST_START("SIM csQueryCSFList");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	uint32_t count = 0;
	status = csQueryCSFList(handle, NULL, &count);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueryCSFList count failed");
	ASSERT_EQ(14u, count, "expected 14 built-in CSFs");

	csCSFInfo csfs[14];
	status = csQueryCSFList(handle, csfs, &count);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueryCSFList failed");

	if (strcmp(csfs[0].name, "SUM64") != 0) {
		TEST_FAIL("first CSF should be SUM64");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_init_mem(void)
{
	TEST_START("SIM csInitMem");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	csMemDesc desc = { .type = CS_MEM_TYPE_AFDM, .size = 4096 };
	csMemHandle mem;
	void *ptr = NULL;
	status = csAllocMem(handle, &desc, &mem, &ptr);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csAllocMem failed");

	memset(ptr, 0xFF, 4096);

	status = csInitMem(handle, mem, 0, 4096);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInitMem failed");

	for (size_t i = 0; i < 4096; i++) {
		if (((uint8_t *)ptr)[i] != 0) {
			TEST_FAIL("memory not zeroed");
			csFreeMem(handle, mem);
			csCloseCSx(handle);
			csShutdown();
			unsetenv("LIBCS_BACKEND");
			return;
		}
	}

	csFreeMem(handle, mem);
	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_abort_request(void)
{
	TEST_START("SIM csAbortRequest");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	csEventHandle event;
	status = csCreateEvent(handle, &event);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csCreateEvent failed");

	uint8_t input[] = { 1, 2, 3 };
	uint8_t output[3];
	csComputeDesc desc = {
		.csf_id = CSF_ID_MEMCPY,
		.input_buffer = input, .input_size = sizeof(input),
		.output_buffer = output, .output_size = sizeof(output),
	};

	status = csQueueComputeRequest(handle, &desc, event, NULL, NULL);
	ASSERT_EQ(CS_STATUS_PENDING, status, "expected PENDING");

	status = csAbortRequest(handle, event);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csAbortRequest failed");

	csDestroyEvent(handle, event);
	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/* =========================================================
 * Tier 3: Granular Batch API
 * ========================================================= */

static void
test_sim_batch_api(void)
{
	TEST_START("SIM batch alloc/add/reconfigure/resize/free");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	csBatchHandle batch;
	status = csAllocBatchRequest(handle, CS_BATCH_MODE_SERIALIZED, 4, &batch);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csAllocBatchRequest failed");
	ASSERT_NE(CS_INVALID_HANDLE, batch, "invalid batch handle");

	uint8_t input[] = { 1, 2, 3, 4 };
	uint8_t output[4];
	csRequest entry = {
		.type = CS_REQ_TYPE_COMPUTE,
		.u.compute.desc = {
			.csf_id = CSF_ID_MEMCPY,
			.input_buffer = input, .input_size = sizeof(input),
			.output_buffer = output, .output_size = sizeof(output),
		},
	};

	status = csAddBatchEntry(batch, &entry);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csAddBatchEntry failed");

	status = csHelperReconfigureBatchEntry(batch, 0, &entry);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csHelperReconfigureBatchEntry failed");

	status = csHelperResizeBatchRequest(batch, 8);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csHelperResizeBatchRequest failed");

	status = csFreeBatchRequest(handle, batch);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csFreeBatchRequest failed");

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_batch_submit(void)
{
	TEST_START("SIM batch alloc→add→submit→free with SUM64 compute");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	uint64_t input[] = { 3, 7, 11, 5 };
	uint64_t output = 0;

	csBatchHandle batch;
	status = csAllocBatchRequest(handle, CS_BATCH_MODE_SERIALIZED, 4, &batch);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csAllocBatchRequest failed");
	ASSERT_NE(CS_INVALID_HANDLE, batch, "invalid batch handle");

	csRequest entry = {
		.type = CS_REQ_TYPE_COMPUTE,
		.u.compute.desc = {
			.csf_id = CSF_ID_SUM64,
			.input_buffer = input, .input_size = sizeof(input),
			.output_buffer = &output, .output_size = sizeof(output),
		},
	};

	status = csAddBatchEntry(batch, &entry);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csAddBatchEntry failed");

	status = csSubmitBatchRequest(handle, batch, NULL, CS_INVALID_HANDLE);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csSubmitBatchRequest failed");

	if (output != 26) {
		char buf[64];
		snprintf(buf, sizeof(buf), "SUM64 result=%lu expected 26",
			 (unsigned long)output);
		TEST_FAIL(buf);
		csFreeBatchRequest(handle, batch);
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	status = csFreeBatchRequest(handle, batch);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csFreeBatchRequest failed");

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_batch_queue_request(void)
{
	TEST_START("SIM csQueueBatchRequest convenience API with 2 compute entries");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	uint64_t input1[] = { 1, 2, 3, 4 };
	uint64_t output1 = 0;
	uint64_t input2[] = { 10, 20, 30, 40 };
	uint64_t output2 = 0;

	csRequest batch[2] = {
		{
			.type = CS_REQ_TYPE_COMPUTE,
			.u.compute.desc = {
				.csf_id = CSF_ID_SUM64,
				.input_buffer = input1, .input_size = sizeof(input1),
				.output_buffer = &output1, .output_size = sizeof(output1),
			},
		},
		{
			.type = CS_REQ_TYPE_COMPUTE,
			.u.compute.desc = {
				.csf_id = CSF_ID_SUM64,
				.input_buffer = input2, .input_size = sizeof(input2),
				.output_buffer = &output2, .output_size = sizeof(output2),
			},
		},
	};

	status = csQueueBatchRequest(handle, NULL, CS_INVALID_HANDLE, batch, 2);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueueBatchRequest failed");

	if (output1 != 10 || output2 != 100) {
		char buf[128];
		snprintf(buf, sizeof(buf),
			 "results: out1=%lu (exp 10), out2=%lu (exp 100)",
			 (unsigned long)output1, (unsigned long)output2);
		TEST_FAIL(buf);
	} else {
		TEST_PASS();
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
}

/* =========================================================
 * Tier 4: Device Management
 * ========================================================= */

static void
test_sim_device_properties(void)
{
	TEST_START("SIM csQueryDeviceProperties");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	csDeviceProperties props;
	memset(&props, 0, sizeof(props));
	status = csQueryDeviceProperties(handle, &props);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueryDeviceProperties failed");

	if (props.csf_count == 0) {
		TEST_FAIL("csf_count should be > 0");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_device_config_reset(void)
{
	TEST_START("SIM csConfig + csReset + csQueryDeviceStatistics");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	uint32_t val = 42;
	status = csConfig(handle, 0, &val, sizeof(val));
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csConfig should succeed on sim");

	status = csReset(handle, 0);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csReset should succeed on sim");

	csDeviceStatistics stats;
	memset(&stats, 0, sizeof(stats));
	status = csQueryDeviceStatistics(handle, &stats);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueryDeviceStatistics should succeed on sim");
	ASSERT_EQ(0ULL, stats.total_executes,
		  "csQueryDeviceStatistics starts at zero before any compute");

	/* A compute request must accumulate execution counters (device-activity
	 * accounting happens in the shared csQueueComputeRequest() funnel). */
	uint64_t in[2] = { 1, 2 };
	uint64_t sm = 0;
	csComputeDesc cdesc = {
		.csf_id = CSF_ID_SUM64,
		.input_buffer = in,
		.input_size = sizeof(in),
		.output_buffer = &sm,
		.output_size = sizeof(sm),
	};
	status = csQueueComputeRequest(handle, &cdesc, CS_INVALID_HANDLE, NULL, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueueComputeRequest (SUM64) should succeed on sim");
	ASSERT_EQ(3ULL, sm, "SUM64 of {1,2} should be 3");

	memset(&stats, 0, sizeof(stats));
	status = csQueryDeviceStatistics(handle, &stats);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueryDeviceStatistics (2nd) should succeed on sim");
	ASSERT_EQ(1ULL, stats.total_executes, "total_executes should be 1 after one compute");
	ASSERT_EQ(16ULL, stats.total_bytes_processed, "16 input bytes processed");
	ASSERT_EQ(0ULL, stats.total_errors, "no errors after a successful compute");

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/* =========================================================
 * Tier 5: Download
 * ========================================================= */

static void
test_sim_csf_download(void)
{
	TEST_START("SIM csCSFDownload + csCSEEDownload");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	uint8_t fake_csf[] = { 0x00, 0x01, 0x02, 0x03 };
	csCSFId csf_id = 0;
	status = csCSFDownload(handle, fake_csf, sizeof(fake_csf), &csf_id);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csCSFDownload failed");

	uint8_t fake_ebpf[] = { 0xBF, 0x00, 0x00, 0x00 };
	uint32_t csee_id = 0;
	status = csCSEEDownload(handle, fake_ebpf, sizeof(fake_ebpf), &csee_id);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csCSEEDownload failed");

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/* =========================================================
 * Tier 5.5: Previously untested coverage-gap APIs
 * (see docs/coverage-gaps.md §1)
 * ========================================================= */

/**
 * Test: csGetCSFInfo (no-caller API, coverage-gaps §1)
 */
static void
test_sim_get_csf_info(void)
{
	TEST_START("SIM csGetCSFInfo");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	/* Error paths */
	csCSFInfo dummy;
	status = csGetCSFInfo(handle, CSF_ID_DOT_PRODUCT, NULL);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_PARAM, status, "NULL info should fail");
	status = csGetCSFInfo(CS_INVALID_HANDLE, CSF_ID_DOT_PRODUCT, &dummy);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_HANDLE, status, "invalid device handle should fail");

	/* Valid query: implementation formats name as "prog_%u" */
	csCSFInfo info;
	memset(&info, 0, sizeof(info));
	status = csGetCSFInfo(handle, CSF_ID_DOT_PRODUCT, &info);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGetCSFInfo failed");

	char expected_name[32];
	snprintf(expected_name, sizeof(expected_name), "prog_%u", (unsigned)CSF_ID_DOT_PRODUCT);
	if (strcmp(info.name, expected_name) != 0) {
		char buf[128];
		snprintf(buf, sizeof(buf), "csGetCSFInfo name mismatch (got '%s')", info.name);
		TEST_FAIL(buf);
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}
	if (info.version != 0 || info.flags != 0) {
		TEST_FAIL("csGetCSFInfo version/flags should be 0");
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/**
 * Test: csGetMemInfo (no-caller API, coverage-gaps §1)
 *
 * Uses before/after deltas so leftover state from earlier tests in this
 * process cannot make the absolute counters fail.
 */
static void
test_sim_get_mem_info(void)
{
	TEST_START("SIM csGetMemInfo");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	/* Error paths */
	csMemInfo minfo;
	status = csGetMemInfo(handle, NULL);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_PARAM, status, "NULL info should fail");
	status = csGetMemInfo(CS_INVALID_HANDLE, &minfo);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_HANDLE, status, "invalid device handle should fail");

	/* Baseline counters */
	status = csGetMemInfo(handle, &minfo);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGetMemInfo failed");
	ASSERT_NE(0, minfo.max_allocations, "max_allocations should be > 0");

	uint32_t count_before = minfo.allocation_count;
	size_t total_before = minfo.total_allocated_bytes;

	/* One allocation should move both counters */
	csMemDesc desc = { .type = CS_MEM_TYPE_AFDM, .size = 4096 };
	csMemHandle mem;
	void *ptr = NULL;
	status = csAllocMem(handle, &desc, &mem, &ptr);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csAllocMem(sim) failed");

	status = csGetMemInfo(handle, &minfo);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGetMemInfo after alloc failed");
	if (minfo.allocation_count != count_before + 1 ||
	    minfo.total_allocated_bytes != total_before + 4096) {
		char buf[160];
		snprintf(buf, sizeof(buf),
			 "counters wrong after alloc (count %u->%u, total %zu->%zu)",
			 count_before, minfo.allocation_count,
			 total_before, minfo.total_allocated_bytes);
		TEST_FAIL(buf);
		csFreeMem(handle, mem);
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	/* Free should restore both counters */
	status = csFreeMem(handle, mem);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csFreeMem failed");

	status = csGetMemInfo(handle, &minfo);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGetMemInfo after free failed");
	if (minfo.allocation_count != count_before ||
	    minfo.total_allocated_bytes != total_before) {
		char buf[160];
		snprintf(buf, sizeof(buf),
			 "counters wrong after free (count %u, expected %u)",
			 minfo.allocation_count, count_before);
		TEST_FAIL(buf);
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/**
 * Test: csRunComputeAsync (no-caller API, coverage-gaps §1)
 *
 * Exercises the event/create-queue-wait-destroy path against the sim
 * backend with the dot-product CSF (expected result 32.0).
 */
static void
test_sim_run_compute_async(void)
{
	TEST_START("SIM csRunComputeAsync");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx(sim) failed");

	float input[] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f };
	float output = 0.0f;
	csComputeDesc desc = {
		.csf_id = CSF_ID_DOT_PRODUCT,
		.input_buffer = input,
		.input_size = sizeof(input),
		.output_buffer = &output,
		.output_size = sizeof(output),
		.args = NULL,
		.args_size = 0,
	};

	/* Error paths */
	csComputeResult cr;
	status = csRunComputeAsync(CS_INVALID_HANDLE, &desc, 5000, &cr);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_PARAM, status, "invalid device handle should fail");
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_PARAM, cr.status, "result.status should mirror error");
	status = csRunComputeAsync(handle, NULL, 5000, &cr);
	ASSERT_EQ(CS_STATUS_ERROR_INVALID_PARAM, status, "NULL desc should fail");

	/* Valid async execution */
	memset(&cr, 0, sizeof(cr));
	status = csRunComputeAsync(handle, &desc, 5000, &cr);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csRunComputeAsync failed");
	ASSERT_EQ(CS_STATUS_SUCCESS, cr.status, "async completion status should be SUCCESS");
	if (output != 32.0f) {
		char buf[128];
		snprintf(buf, sizeof(buf), "dot-product mismatch (got %f, want 32.0)", (double)output);
		TEST_FAIL(buf);
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/* =========================================================
 * Tier 6: Plugin System
 * ========================================================= */

static void
test_query_library_support(void)
{
	TEST_START("csQueryLibrarySupport");

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csLibrarySupport support;
	memset(&support, 0, sizeof(support));
	status = csQueryLibrarySupport(&support);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csQueryLibrarySupport failed");

	if (support.api_version == 0) {
		TEST_FAIL("api_version should be nonzero");
		csShutdown();
		return;
	}

	if (support.num_backends == 0) {
		TEST_FAIL("num_backends should be > 0");
		csShutdown();
		return;
	}

	if ((support.features & CS_FEATURE_ASYNC_COMPUTE) == 0) {
		TEST_FAIL("async compute feature should be set");
		csShutdown();
		return;
	}

	if ((support.features & CS_FEATURE_BATCH) == 0) {
		TEST_FAIL("batch feature should be set");
		csShutdown();
		return;
	}

	csShutdown();
	TEST_PASS();
}

static void
test_deregister_nonexistent_plugin(void)
{
	TEST_START("cs_backend_unregister nonexistent");

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	status = cs_backend_unregister("nonexistent_backend");
	ASSERT_EQ(CS_STATUS_ERROR_NOT_FOUND, status, "expected NOT_FOUND");

	csShutdown();
	TEST_PASS();
}

/* =========================================================
 * Tier 7: Discovery & Notification
 * ========================================================= */

static int g_notify_called = 0;
static uint32_t g_notify_event_type = 0;

static void
test_notify_callback(uint32_t event_type, void *user_data)
{
	(void)user_data;
	g_notify_called++;
	g_notify_event_type = event_type;
}

static void
test_sim_register_deregister_notify(void)
{
	TEST_START("SIM csRegisterNotify + csDeregisterNotify");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	g_notify_called = 0;
	status = csRegisterNotify(handle, 1, test_notify_callback, NULL);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csRegisterNotify failed");

	status = csDeregisterNotify(handle, 1);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csDeregisterNotify failed");

	status = csDeregisterNotify(handle, 1);
	ASSERT_EQ(CS_STATUS_ERROR_NOT_FOUND, status, "expected NOT_FOUND after deregister");

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_notify_limit(void)
{
	TEST_START("SIM csRegisterNotify limit (CS_MAX_NOTIFIERS=32)");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	csStatus last = CS_STATUS_SUCCESS;
	for (int i = 0; i < 40; i++) {
		last = csRegisterNotify(handle, (uint32_t)i, test_notify_callback, NULL);
		if (last == CS_STATUS_ERROR_LIMIT) {
			break;
		}
	}
	ASSERT_EQ(CS_STATUS_ERROR_LIMIT, last, "expected LIMIT at 32 notifiers");

	csDeregisterNotify(handle, 0xFFFFFFFF);
	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

static void
test_sim_group_compute(void)
{
	TEST_START("SIM csGroupComputeByIds + csUngroupComputeFromGroupId");

	set_sim_backend();

	csStatus status = csInit();
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csInit failed");

	csCSxHandle handle;
	status = csOpenCSx("sim://libcs0", &handle);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csOpenCSx failed");

	csCSFId ids[] = { CS_CSF_SUM64, CS_CSF_MAX64, CS_CSF_MIN64 };
	uint32_t group_id = 0;
	status = csGroupComputeByIds(handle, ids, 3, &group_id);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csGroupComputeByIds failed");
	ASSERT_NE(0, group_id + 1, "group_id should be a valid slot (0 is valid)");

	status = csUngroupComputeFromGroupId(handle, group_id);
	ASSERT_EQ(CS_STATUS_SUCCESS, status, "csUngroupComputeFromGroupId failed");

	status = csUngroupComputeFromGroupId(handle, group_id);
	ASSERT_NE(CS_STATUS_SUCCESS, status, "double-ungroup should fail");

	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
	TEST_PASS();
}

/**
 * Main test runner
 */
int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	printf("\n");
	printf("===========================================\n");
	printf("  CS API Unit Tests\n");
	printf("===========================================\n\n");

	printf("Environment:\n");
	const char *uri = getenv("LIBCS_DEVICE_URI");
	printf("  LIBCS_DEVICE_URI: %s\n", uri ? uri : "(not set)");
	const char *debug = getenv("LIBCS_DEBUG");
	printf("  LIBCS_DEBUG: %s\n", debug ? debug : "(not set)");
	printf("\n");

	printf("Running tests:\n\n");

	/* Basic library tests (no device needed) */
	test_version();
	test_init_shutdown();
	test_device_discovery();
	test_invalid_handle();
	test_event_invalid();
	test_compute_arg_helper();
	test_sim_device_open();
	test_sim_integer_reductions();
	test_sim_dot_product();
	test_sim_vector_similarity();
	test_sim_filter_gt();
	test_sim_multi_agg64();
	test_sim_async_callback();
	test_sim_rle_compress();

	/* Tier 1-2: Extended API */
	test_sim_get_csf_id_by_name();
	test_sim_query_csf_list();
	test_sim_init_mem();
	test_sim_abort_request();

	/* Tier 3: Batch API */
	test_sim_batch_api();
	test_sim_batch_submit();
	test_sim_batch_queue_request();

	/* Tier 4: Device management */
	test_sim_device_properties();
	test_sim_device_config_reset();

	/* Tier 5: Download */
	test_sim_csf_download();

	/* Tier 5.5: Coverage-gap APIs (docs/coverage-gaps.md §1) */
	test_sim_get_csf_info();
	test_sim_get_mem_info();
	test_sim_run_compute_async();

	/* Tier 6: Plugin system */
	test_query_library_support();
	test_deregister_nonexistent_plugin();

	/* Tier 7: Discovery & notification */
	test_sim_register_deregister_notify();
	test_sim_notify_limit();

	/* Tier 8: Compute grouping (SNIA CS API v1.0 Table 7) */
	test_sim_group_compute();

	/* Device-dependent tests */
	test_device_lifecycle();
	test_memory_allocation();
	test_event_lifecycle();
	test_csf_id();
	test_compute_sync();

	TEST_SUMMARY();

	return TEST_EXIT();
}
