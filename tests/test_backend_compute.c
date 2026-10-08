/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file test_backend_compute.c
 * @brief Open a device with a specific backend, run SUM64 compute, report result.
 *
 * Usage:
 *   test_backend_compute --backend <name> --uri <uri> [options]
 *
 * Options:
 *   --backend <sim|xnvme|ioctl|io_uring|libnvme>
 *   --uri <device_uri>
 *   --nsid <libcs_nsid>       (default 2)
 *   --nvm-nsid <nvm_nsid>    (default 1)
 *   --slm-nsid <slm_nsid>    (default 0=disabled)
 *   --subnqn <nqn>           (for xnvme+spdk NVMe-oF)
 *   --count <n_elements>     (default 64, must be <= 4096/sizeof(uint64))
 *   --async                  (use submit+reap instead of sync execute)
 *
 * Exit: 0=pass, 1=fail
 */

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cs_api.h"

#define MAX_ELEMS 512  /* 4KB / sizeof(uint64) */

static void usage(const char *prog)
{
	fprintf(stderr,
	        "Usage: %s --backend <name> --uri <uri> [options]\n"
	        "\n"
	        "  --backend <sim|xnvme|ioctl|io_uring|libnvme>\n"
	        "  --uri <device_uri>\n"
	        "  --nsid <libcs_nsid>       (default 2)\n"
	        "  --nvm-nsid <nvm_nsid>    (default 1)\n"
	        "  --slm-nsid <slm_nsid>    (default 0=disabled)\n"
	        "  --subnqn <nqn>           (for xnvme+spdk NVMe-oF)\n"
	        "  --count <n_elements>     (default 64)\n"
	        "  --async                  (use submit+reap)\n",
	        prog);
}

int main(int argc, char *argv[])
{
	const char *backend = NULL;
	const char *uri = NULL;
	uint32_t libcs_nsid = 2;
	uint32_t nvm_nsid = 1;
	uint32_t slm_nsid = 0;
	const char *subnqn = NULL;
	uint32_t count = 64;
	int use_async = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
			backend = argv[++i];
		} else if (strcmp(argv[i], "--uri") == 0 && i + 1 < argc) {
			uri = argv[++i];
		} else if (strcmp(argv[i], "--nsid") == 0 && i + 1 < argc) {
			libcs_nsid = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--nvm-nsid") == 0 && i + 1 < argc) {
			nvm_nsid = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--slm-nsid") == 0 && i + 1 < argc) {
			slm_nsid = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--subnqn") == 0 && i + 1 < argc) {
			subnqn = argv[++i];
		} else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
			count = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--async") == 0) {
			use_async = 1;
		} else {
			fprintf(stderr, "Unknown arg: %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	if (!backend || !uri) {
		usage(argv[0]);
		return 1;
	}

	if (count > MAX_ELEMS) {
		fprintf(stderr, "count %u exceeds max %d\n", count, MAX_ELEMS);
		return 1;
	}

	/* Set env vars before csInit() */
	setenv("LIBCS_BACKEND", backend, 1);

	char buf[32];
	snprintf(buf, sizeof(buf), "%u", libcs_nsid);
	setenv("LIBCS_XNVME_NSID", buf, 1);
	snprintf(buf, sizeof(buf), "%u", nvm_nsid);
	setenv("LIBCS_XNVME_NVM_NSID", buf, 1);
	if (slm_nsid > 0) {
		snprintf(buf, sizeof(buf), "%u", slm_nsid);
		setenv("LIBCS_XNVME_SLM_NSID", buf, 1);
	}
	if (subnqn) {
		setenv("LIBCS_XNVME_SUBNQN", subnqn, 1);
		setenv("LIBCS_XNVME_ADRFAM", "IPv4", 1);
	}

	/* For xnvme SPDK backend, use spdk; for kernel devices, use linux */
	if (strcmp(backend, "xnvme") == 0) {
		if (strstr(uri, ":") && !strstr(uri, "/dev/")) {
			setenv("LIBCS_XNVME_BE", "spdk", 1);
		} else {
			setenv("LIBCS_XNVME_BE", "linux", 1);
		}
	}

	printf("[backend=%s uri=%s nsid=%u nvm_nsid=%u slm_nsid=%u count=%u async=%d]\n",
	       backend, uri, libcs_nsid, nvm_nsid, slm_nsid, count, use_async);

	csStatus s = csInit();
	if (s != CS_STATUS_SUCCESS) {
		fprintf(stderr, "FAIL: csInit: %d\n", s);
		return 1;
	}

	csCSxHandle handle;
	s = csOpenCSx(uri, &handle);
	if (s != CS_STATUS_SUCCESS) {
		fprintf(stderr, "FAIL: csOpenCSx(%s): %d\n", uri, s);
		csShutdown();
		return 1;
	}
	printf("  csOpenCSx: OK\n");

	csMemDesc desc = {
		.type = CS_MEM_TYPE_AFDM,
		.size = MAX_ELEMS * sizeof(uint64_t),
	};
	csMemHandle mem;
	void *ptr;
	s = csAllocMem(handle, &desc, &mem, &ptr);
	if (s != CS_STATUS_SUCCESS) {
		fprintf(stderr, "FAIL: csAllocMem: %d\n", s);
		csCloseCSx(handle);
		csShutdown();
		return 1;
	}
	printf("  csAllocMem: OK (%u bytes)\n", MAX_ELEMS * (uint32_t)sizeof(uint64_t));

	uint64_t *vals = (uint64_t *)ptr;
	uint64_t expected = 0;
	for (uint32_t i = 0; i < count; i++) {
		vals[i] = (uint64_t)(i + 1);
		expected += vals[i];
	}

	uint64_t result = 0;
	csComputeDesc compute = {
		.csf_id        = CS_CSF_SUM64,
		.input_buffer  = ptr,
		.input_size    = count * sizeof(uint64_t),
		.output_buffer = &result,
		.output_size   = sizeof(result),
	};

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (use_async) {
		csEventHandle event;
		s = csCreateEvent(handle, &event);
		if (s != CS_STATUS_SUCCESS) {
			fprintf(stderr, "FAIL: csCreateEvent: %d\n", s);
			csFreeMem(handle, mem);
			csCloseCSx(handle);
			csShutdown();
			return 1;
		}

		s = csQueueComputeRequest(handle, &compute, event, NULL, NULL);
		if (s != CS_STATUS_PENDING) {
			fprintf(stderr, "FAIL: csQueueComputeRequest: %d (expected PENDING)\n", s);
			csDestroyEvent(handle, event);
			csFreeMem(handle, mem);
			csCloseCSx(handle);
			csShutdown();
			return 1;
		}

		csEventStatus ev_status;
		csStatus compute_result;
		s = csPollEvent(handle, event, &ev_status, &compute_result);
		if (s != CS_STATUS_SUCCESS) {
			fprintf(stderr, "FAIL: csPollEvent: %d\n", s);
			csDestroyEvent(handle, event);
			csFreeMem(handle, mem);
			csCloseCSx(handle);
			csShutdown();
			return 1;
		}
		if (compute_result != CS_STATUS_SUCCESS) {
			fprintf(stderr, "FAIL: compute_result: %d\n", compute_result);
			csDestroyEvent(handle, event);
			csFreeMem(handle, mem);
			csCloseCSx(handle);
			csShutdown();
			return 1;
		}
		csDestroyEvent(handle, event);
	} else {
		s = csQueueComputeRequest(handle, &compute, CS_INVALID_HANDLE, NULL, NULL);
		if (s != CS_STATUS_SUCCESS) {
			fprintf(stderr, "FAIL: csQueueComputeRequest: %d\n", s);
			csFreeMem(handle, mem);
			csCloseCSx(handle);
			csShutdown();
			return 1;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	double elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
			    (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

	printf("  csQueueComputeRequest(SUM64): OK (%.3f ms)\n", elapsed_ms);

	if (result == expected) {
		printf("  RESULT: SUM64 = %" PRIu64 " (expected %" PRIu64 ") -> PASS\n", result, expected);
	} else {
		printf("  RESULT: SUM64 = %" PRIu64 " (expected %" PRIu64 ") -> FAIL\n", result, expected);
	}

	csFreeMem(handle, mem);
	csCloseCSx(handle);
	csShutdown();

	return (result == expected) ? 0 : 1;
}
