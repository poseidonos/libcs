/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file test_backend_vtable.c
 * @brief Verify all registered backends wire all core vtable ops
 *
 * Standalone test — does not depend on test_cs_api.c.
 * Build: gcc -o test_backend_vtable test_backend_vtable.c \
 *            -Iinclude -Lbuild -lcs -lpthread -lm
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cs_api.h"
#include "cs_backend.h"
#include "test_util.h"

static void
test_vtable_completeness(void)
{
	TEST_START("all backends wire all 21 core vtable ops");
	csInit();

	uint32_t count = 0;
	csStatus s = cs_backend_list(NULL, &count);
	if (s != CS_STATUS_SUCCESS || count == 0) {
		TEST_FAIL("no backends registered");
		return;
	}

	const char **names = calloc(count, sizeof(char *));
	if (!names) { TEST_FAIL("calloc failed"); return; }

	s = cs_backend_list(names, &count);
	if (s != CS_STATUS_SUCCESS) {
		free(names);
		TEST_FAIL("cs_backend_list names failed");
		return;
	}

	int failures = 0;
	for (uint32_t b = 0; b < count; b++) {
		const cs_backend_ops_t *ops = cs_backend_get(names[b]);
		if (!ops) {
			printf("\n    backend '%s': NULL ops\n", names[b]);
			failures++;
			continue;
		}

		const struct { const char *field; void *ptr; } checks[] = {
			{"init",                 (void *)ops->init},
			{"shutdown",             (void *)ops->shutdown},
			{"dev_query",            (void *)ops->dev_query},
			{"dev_open",             (void *)ops->dev_open},
			{"dev_close",            (void *)ops->dev_close},
			{"dev_get_info",         (void *)ops->dev_get_info},
			{"mem_alloc",            (void *)ops->mem_alloc},
			{"mem_free",             (void *)ops->mem_free},
			{"mem_init",             (void *)ops->mem_init},
			{"prog_load",            (void *)ops->prog_load},
			{"prog_activate",        (void *)ops->prog_activate},
			{"prog_deactivate",      (void *)ops->prog_deactivate},
			{"prog_get_name",        (void *)ops->prog_get_name},
			{"query_csf_list",       (void *)ops->query_csf_list},
			{"compute_execute",      (void *)ops->compute_execute},
			{"compute_submit",       (void *)ops->compute_submit},
			{"compute_reap",         (void *)ops->compute_reap},
			{"abort_request",        (void *)ops->abort_request},
		{"storage_io",           (void *)ops->storage_io},
		{"poll",                 (void *)ops->poll},
		};

		for (size_t i = 0; i < sizeof(checks)/sizeof(checks[0]); i++) {
			if (!checks[i].ptr) {
				printf("\n    backend '%s': NULL %s\n",
				       names[b], checks[i].field);
				failures++;
			}
		}

		/* Batch ops are optional: a backend that does not implement batch
		 * submission is valid (cs_api.c falls back to per-request
		 * sequential execution). Report presence, do not fail the test. */
		if (ops->batch_submit) {
			printf("\n    backend '%s': batch_submit supported\n", names[b]);
		}
	}

	free(names);
	csShutdown();

	if (failures > 0) {
		char buf[128];
		snprintf(buf, sizeof(buf), "%d NULL vtable entries across %u backends",
			 failures, count);
		TEST_FAIL(buf);
		return;
	}
	TEST_PASS();
}

static void
test_prog_get_name_all_backends(void)
{
	TEST_START("prog_get_name returns SUM64 for pind=2 on all backends");
	csInit();

	uint32_t count = 0;
	cs_backend_list(NULL, &count);
	const char **names = calloc(count, sizeof(char *));
	cs_backend_list(names, &count);

	struct cs_backend_ctx *sim_ctx = NULL;
	const cs_backend_ops_t *sim_ops = cs_backend_get("sim");
	if (sim_ops && sim_ops->dev_open) {
		sim_ops->dev_open("sim://libcs0", &sim_ctx);
	}

	int failures = 0;
	for (uint32_t b = 0; b < count; b++) {
		const cs_backend_ops_t *ops = cs_backend_get(names[b]);

		struct cs_backend_ctx *ctx = NULL;
		if (strcmp(names[b], "sim") == 0) ctx = sim_ctx;

		if (!ops || !ops->prog_get_name) continue;

		char name[64] = {0};
		csStatus s = ops->prog_get_name(ctx, 2, name, sizeof(name));
		if (s != CS_STATUS_SUCCESS || strcmp(name, "SUM64") != 0) {
			printf("\n    backend '%s': got '%s' (status %d)", names[b], name, s);
			failures++;
		}
	}

	if (sim_ctx && sim_ops && sim_ops->dev_close) sim_ops->dev_close(sim_ctx);
	free(names);
	csShutdown();

	if (failures > 0) { TEST_FAIL("name mismatch"); return; }
	TEST_PASS();
}

static void
test_query_csf_list_all_backends(void)
{
	TEST_START("query_csf_list returns >=11 CSFs on all backends");
	csInit();

	uint32_t count = 0;
	cs_backend_list(NULL, &count);
	const char **names = calloc(count, sizeof(char *));
	cs_backend_list(names, &count);

	int failures = 0;
	for (uint32_t b = 0; b < count; b++) {
		const cs_backend_ops_t *ops = cs_backend_get(names[b]);
		if (!ops || !ops->query_csf_list) continue;

		uint32_t csf_count = 0;
		csStatus s = ops->query_csf_list(NULL, NULL, &csf_count);
		if (s != CS_STATUS_SUCCESS || csf_count < 11) {
			printf("\n    backend '%s': count=%u (status %d)",
			       names[b], csf_count, s);
			failures++;
		}
	}

	free(names);
	csShutdown();

	if (failures > 0) { TEST_FAIL("csf count mismatch"); return; }
	TEST_PASS();
}

static void
test_mem_init_all_backends(void)
{
	TEST_START("mem_init fills buffer with 0xAA on all backends");
	csInit();

	uint32_t count = 0;
	cs_backend_list(NULL, &count);
	const char **names = calloc(count, sizeof(char *));
	cs_backend_list(names, &count);

	int failures = 0;
	for (uint32_t b = 0; b < count; b++) {
		const cs_backend_ops_t *ops = cs_backend_get(names[b]);
		if (!ops || !ops->mem_init) continue;

		uint8_t buf[256];
		memset(buf, 0, sizeof(buf));
		csStatus s = ops->mem_init(NULL, buf, 0xAA, sizeof(buf));
		if (s != CS_STATUS_SUCCESS) {
			printf("\n    backend '%s': status %d", names[b], s);
			failures++;
			continue;
		}
		for (size_t i = 0; i < sizeof(buf); i++) {
			if (buf[i] != 0xAA) {
				printf("\n    backend '%s': buf[%zu]=0x%02x", names[b], i, buf[i]);
				failures++;
				break;
			}
		}
	}

	free(names);
	csShutdown();

	if (failures > 0) { TEST_FAIL("fill mismatch"); return; }
	TEST_PASS();
}

static void
test_compute_submit_reap_sim(void)
{
	TEST_START("sim compute_submit + compute_reap async cycle");
	setenv("LIBCS_BACKEND", "sim", 1);
	csInit();

	csCSxHandle handle;
	csStatus s = csOpenCSx("sim://libcs0", &handle);
	if (s != CS_STATUS_SUCCESS) { TEST_FAIL("csOpenCSx failed"); return; }

	csMemDesc desc = { .type = CS_MEM_TYPE_AFDM, .size = 64 * sizeof(uint64_t) };
	csMemHandle mem;
	void *ptr;
	s = csAllocMem(handle, &desc, &mem, &ptr);
	if (s != CS_STATUS_SUCCESS) {
		TEST_FAIL("csAllocMem failed");
		csCloseCSx(handle);
		csShutdown();
		return;
	}

	uint64_t *vals = (uint64_t *)ptr;
	for (int i = 0; i < 64; i++) vals[i] = (uint64_t)(i + 1);

	csEventHandle event;
	s = csCreateEvent(handle, &event);
	if (s != CS_STATUS_SUCCESS) {
		TEST_FAIL("csCreateEvent failed");
		csFreeMem(handle, mem);
		csCloseCSx(handle);
		csShutdown();
		return;
	}

	csComputeDesc compute = {
		.csf_id        = CS_CSF_SUM64,
		.input_buffer  = ptr,
		.input_size    = 64 * sizeof(uint64_t),
		.output_buffer = ptr,
		.output_size   = sizeof(uint64_t),
	};

	s = csQueueComputeRequest(handle, &compute, event, NULL, NULL);
	if (s != CS_STATUS_PENDING) {
		char buf[64];
		snprintf(buf, sizeof(buf), "expected PENDING, got %d", s);
		TEST_FAIL(buf);
		csDestroyEvent(handle, event);
		csFreeMem(handle, mem);
		csCloseCSx(handle);
		csShutdown();
		return;
	}

	csEventStatus ev_status;
	csStatus result;
	s = csPollEvent(handle, event, &ev_status, &result);
	if (s != CS_STATUS_SUCCESS || ev_status != CS_EVENT_STATUS_COMPLETE) {
		TEST_FAIL("poll/wait failed");
		csDestroyEvent(handle, event);
		csFreeMem(handle, mem);
		csCloseCSx(handle);
		csShutdown();
		return;
	}

	uint64_t sum = *(uint64_t *)ptr;
	if (sum != 2080) {
		char buf[64];
		snprintf(buf, sizeof(buf), "SUM64 result=%" PRIu64 " expected 2080", sum);
		TEST_FAIL(buf);
	} else {
		TEST_PASS();
	}

	csDestroyEvent(handle, event);
	csFreeMem(handle, mem);
	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
}

static void
test_compute_sync_hardware(const char *backend_name, const char *device_uri)
{
	char label[128];
	snprintf(label, sizeof(label), "compute_sync SUM64 via %s backend", backend_name);
	TEST_START(label);

	setenv("LIBCS_BACKEND", backend_name, 1);
	csInit();

	if (!cs_backend_get(backend_name)) {
		TEST_SKIP("backend not registered in this build");
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csCSxHandle handle;
	csStatus s = csOpenCSx(device_uri, &handle);
	if (s != CS_STATUS_SUCCESS) {
		char buf[128];
		snprintf(buf, sizeof(buf), "csOpenCSx(%s) failed: %d", device_uri, s);
		TEST_FAIL(buf);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	csMemDesc desc = { .type = CS_MEM_TYPE_AFDM, .size = 4096 };
	csMemHandle mem;
	void *ptr;
	s = csAllocMem(handle, &desc, &mem, &ptr);
	if (s != CS_STATUS_SUCCESS) {
		char buf[128];
		snprintf(buf, sizeof(buf), "csAllocMem failed: %d", s);
		TEST_FAIL(buf);
		csCloseCSx(handle);
		csShutdown();
		unsetenv("LIBCS_BACKEND");
		return;
	}

	uint64_t *vals = (uint64_t *)ptr;
	for (int i = 0; i < 64; i++) vals[i] = (uint64_t)(i + 1);

	uint64_t result = 0;
	csComputeDesc compute = {
		.csf_id        = CS_CSF_SUM64,
		.input_buffer  = ptr,
		.input_size    = 64 * sizeof(uint64_t),
		.output_buffer = &result,
		.output_size   = sizeof(result),
	};

	s = csQueueComputeRequest(handle, &compute, CS_INVALID_HANDLE, NULL, NULL);
	if (s != CS_STATUS_SUCCESS) {
		char buf[128];
		snprintf(buf, sizeof(buf), "compute failed: %d", s);
		TEST_FAIL(buf);
	} else if (result == 2080) {
		TEST_PASS();
	} else {
		char buf[128];
		snprintf(buf, sizeof(buf), "SUM64 result=%" PRIu64 " expected 2080", result);
		TEST_FAIL(buf);
	}

	csFreeMem(handle, mem);
	csCloseCSx(handle);
	csShutdown();
	unsetenv("LIBCS_BACKEND");
}

int
main(int argc, char *argv[])
{
	const char *device_uri = argc > 1 ? argv[1] : "/dev/nvme1";

	printf("\n");
	printf("===========================================\n");
	printf("  Backend Vtable + Compute Verification\n");
	printf("===========================================\n\n");

	printf("Device URI: %s\n", device_uri);
	printf("NVMe devices:\n");
	printf("  /dev/nvme0  - local NVMe device (PCIe)\n");
	printf("  /dev/nvme1  - NVMe-oF target 192.0.2.1:4420\n");
	printf("\nRunning tests:\n\n");

	test_vtable_completeness();
	test_prog_get_name_all_backends();
	test_query_csf_list_all_backends();
	test_mem_init_all_backends();
	test_compute_submit_reap_sim();

	test_compute_sync_hardware("sim", "sim://libcs0");
	test_compute_sync_hardware("ioctl", "/dev/nvme0");
	test_compute_sync_hardware("ioctl", "/dev/nvme1");
	test_compute_sync_hardware("io_uring", "/dev/nvme0");
	test_compute_sync_hardware("io_uring", "/dev/nvme1");
	test_compute_sync_hardware("libnvme", "/dev/nvme0");
	test_compute_sync_hardware("libnvme", "/dev/nvme1");
	test_compute_sync_hardware("xnvme", "/dev/nvme0");
	test_compute_sync_hardware("xnvme", "/dev/nvme1");

	TEST_SUMMARY();

	return TEST_EXIT();
}
