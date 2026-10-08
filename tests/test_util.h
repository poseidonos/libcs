/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file test_util.h
 * @brief Shared test-count fixture for libcs unit tests
 *
 * Standard counter variables and TEST_* result macros shared by
 * test_cs_api.c, test_backend_vtable.c, test_filter_gt.c and
 * test_xnvme_logic.c. Each test file is a standalone binary, so
 * the definitions here are static.
 *
 * Assertion macros with per-file formats remain in the files that
 * use them; this header only owns the pass/fail bookkeeping.
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

/* Test result counters */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) \
	printf("  %-55s ", name); \
	fflush(stdout)

#define TEST_PASS() \
	do { \
		printf("[PASS]\n"); \
		tests_passed++; \
	} while (0)

#define TEST_FAIL(msg) \
	do { \
		printf("[FAIL] %s\n", msg); \
		tests_failed++; \
	} while (0)

#define TEST_SKIP(msg) \
	do { \
		printf("[SKIP] %s\n", msg); \
	} while (0)

#define TEST_SUMMARY() \
	do { \
		printf("\n"); \
		printf("===========================================\n"); \
		printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed); \
		printf("===========================================\n\n"); \
	} while (0)

#define TEST_EXIT() (tests_failed > 0 ? 1 : 0)

#endif /* TEST_UTIL_H */
