/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file test_xnvme_logic.c
 * @brief Unit tests for pure-logic functions exposed from libcs internals:
 *        - xnvme_uri_is_local_direct(): URI transport classification
 *        - cs_infer_bytes_produced(): output size inference per CSF type
 *
 * No device required. Pure logic tests.
 *
 * Build: gcc -o test_xnvme_logic test_xnvme_logic.c \
 *            -Iinclude -Isrc -Lbuild -lcs -lpthread -lm
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cs_api.h"
#include "cs_helpers.h"
#include "cs_internal.h"
#include "test_util.h"

#define ASSERT_EQ(expected, actual, msg) \
	do { \
		if ((expected) != (actual)) { \
			char buf[128]; \
			snprintf(buf, sizeof(buf), "%s (expected %zu, got %zu)", \
				 msg, (size_t)(expected), (size_t)(actual)); \
			TEST_FAIL(buf); \
			return; \
		} \
	} while (0)

#define ASSERT_TRUE(cond, msg) \
	do { \
		if (!(cond)) { \
			TEST_FAIL(msg); \
			return; \
		} \
	} while (0)

/* ── xnvme_uri_is_local_direct tests ──────────────────── */

static void
test_uri_null(void)
{
	TEST_START("xnvme_uri_is_local_direct: NULL -> false");
	ASSERT_EQ(false, xnvme_uri_is_local_direct(NULL), "NULL should be false");
	TEST_PASS();
}

static void
test_uri_empty(void)
{
	TEST_START("xnvme_uri_is_local_direct: empty string -> false");
	ASSERT_EQ(false, xnvme_uri_is_local_direct(""), "empty should be false");
	TEST_PASS();
}

static void
test_uri_block_device(void)
{
	TEST_START("xnvme_uri_is_local_direct: /dev/nvme0n1 -> true");
	ASSERT_EQ(true, xnvme_uri_is_local_direct("/dev/nvme0n1"), "block path should be true");
	TEST_PASS();
}

static void
test_uri_pci(void)
{
	TEST_START("xnvme_uri_is_local_direct: pci:0000:01:00.0 -> true");
	ASSERT_EQ(true, xnvme_uri_is_local_direct("pci:0000:01:00.0"), "pci: should be true");
	TEST_PASS();
}

static void
test_uri_pcie(void)
{
	TEST_START("xnvme_uri_is_local_direct: pcie:0000:01:00.0 -> true");
	ASSERT_EQ(true, xnvme_uri_is_local_direct("pcie:0000:01:00.0"), "pcie: should be true");
	TEST_PASS();
}

static void
test_uri_fabric_hostport(void)
{
	TEST_START("xnvme_uri_is_local_direct: 192.0.2.1:4420 -> false");
	ASSERT_EQ(false, xnvme_uri_is_local_direct("192.0.2.1:4420"), "host:port should be false");
	TEST_PASS();
}

static void
test_uri_fabric_localhost(void)
{
	TEST_START("xnvme_uri_is_local_direct: 127.0.0.1:4420 -> false");
	ASSERT_EQ(false, xnvme_uri_is_local_direct("127.0.0.1:4420"), "localhost:port should be false");
	TEST_PASS();
}

static void
test_uri_scheme(void)
{
	TEST_START("xnvme_uri_is_local_direct: rdma://host:4420 -> false");
	ASSERT_EQ(false, xnvme_uri_is_local_direct("rdma://host:4420"), "scheme URI should be false");
	TEST_PASS();
}

/* ── cs_infer_bytes_produced: fixed-output CSFs ───────── */

static void
test_infer_sum64_normal(void)
{
	TEST_START("cs_infer_bytes_produced: SUM64, out=8 -> 8");
	uint64_t out = 0;
	ASSERT_EQ(sizeof(uint64_t),
		   cs_infer_bytes_produced(CS_CSF_SUM64, NULL, 0, &out, sizeof(out)),
		   "SUM64 should produce 8 bytes");
	TEST_PASS();
}

static void
test_infer_sum64_small(void)
{
	TEST_START("cs_infer_bytes_produced: SUM64, out=4 -> 0");
	uint32_t out = 0;
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(CS_CSF_SUM64, NULL, 0, &out, sizeof(out)),
		   "SUM64 with 4-byte output should produce 0");
	TEST_PASS();
}

static void
test_infer_sum64_oversize(void)
{
	TEST_START("cs_infer_bytes_produced: SUM64, out=4096 -> 8");
	uint8_t out[4096];
	ASSERT_EQ(sizeof(uint64_t),
		   cs_infer_bytes_produced(CS_CSF_SUM64, NULL, 0, out, sizeof(out)),
		   "SUM64 should still produce 8 bytes regardless of buffer size");
	TEST_PASS();
}

static void
test_infer_max64(void)
{
	TEST_START("cs_infer_bytes_produced: MAX64, out=8 -> 8");
	uint64_t out = 0;
	ASSERT_EQ(sizeof(uint64_t),
		   cs_infer_bytes_produced(CS_CSF_MAX64, NULL, 0, &out, sizeof(out)),
		   "MAX64 should produce 8 bytes");
	TEST_PASS();
}

static void
test_infer_min64(void)
{
	TEST_START("cs_infer_bytes_produced: MIN64, out=8 -> 8");
	uint64_t out = 0;
	ASSERT_EQ(sizeof(uint64_t),
		   cs_infer_bytes_produced(CS_CSF_MIN64, NULL, 0, &out, sizeof(out)),
		   "MIN64 should produce 8 bytes");
	TEST_PASS();
}

static void
test_infer_dot_product(void)
{
	TEST_START("cs_infer_bytes_produced: DOT_PRODUCT, out=4 -> 4");
	float out = 0;
	ASSERT_EQ(sizeof(float),
		   cs_infer_bytes_produced(CS_CSF_DOT_PRODUCT, NULL, 0, &out, sizeof(out)),
		   "DOT_PRODUCT should produce 4 bytes");
	TEST_PASS();
}

static void
test_infer_l2_distance(void)
{
	TEST_START("cs_infer_bytes_produced: L2_DISTANCE_SQ, out=4 -> 4");
	float out = 0;
	ASSERT_EQ(sizeof(float),
		   cs_infer_bytes_produced(CS_CSF_L2_DISTANCE_SQ, NULL, 0, &out, sizeof(out)),
		   "L2_DISTANCE_SQ should produce 4 bytes");
	TEST_PASS();
}

static void
test_infer_cosine(void)
{
	TEST_START("cs_infer_bytes_produced: COSINE_SIMILARITY, out=4 -> 4");
	float out = 0;
	ASSERT_EQ(sizeof(float),
		   cs_infer_bytes_produced(CS_CSF_COSINE_SIMILARITY, NULL, 0, &out, sizeof(out)),
		   "COSINE_SIMILARITY should produce 4 bytes");
	TEST_PASS();
}

static void
test_infer_multi_agg64(void)
{
	TEST_START("cs_infer_bytes_produced: MULTI_AGG64, out=40 -> 40");
	csMultiAgg64Result out = {0};
	ASSERT_EQ(sizeof(csMultiAgg64Result),
		   cs_infer_bytes_produced(CS_CSF_MULTI_AGG64, NULL, 0, &out, sizeof(out)),
		   "MULTI_AGG64 should produce sizeof(csMultiAgg64Result) bytes");
	TEST_PASS();
}

static void
test_infer_multi_agg64_small(void)
{
	TEST_START("cs_infer_bytes_produced: MULTI_AGG64, out=4 -> 0");
	uint32_t out = 0;
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(CS_CSF_MULTI_AGG64, NULL, 0, &out, sizeof(out)),
		   "MULTI_AGG64 with tiny output should produce 0");
	TEST_PASS();
}

/* ── cs_infer_bytes_produced: MEMCPY ──────────────────── */

static void
test_infer_memcpy_input_smaller(void)
{
	TEST_START("cs_infer_bytes_produced: MEMCPY, in=100 out=200 -> 100");
	ASSERT_EQ(100,
		   cs_infer_bytes_produced(CS_CSF_MEMCPY, NULL, 100, NULL, 200),
		   "MEMCPY should produce min(input, output)");
	TEST_PASS();
}

static void
test_infer_memcpy_output_smaller(void)
{
	TEST_START("cs_infer_bytes_produced: MEMCPY, in=200 out=100 -> 100");
	ASSERT_EQ(100,
		   cs_infer_bytes_produced(CS_CSF_MEMCPY, NULL, 200, NULL, 100),
		   "MEMCPY should produce min(input, output)");
	TEST_PASS();
}

static void
test_infer_memcpy_equal(void)
{
	TEST_START("cs_infer_bytes_produced: MEMCPY, in=128 out=128 -> 128");
	ASSERT_EQ(128,
		   cs_infer_bytes_produced(CS_CSF_MEMCPY, NULL, 128, NULL, 128),
		   "MEMCPY should produce min(input, output)");
	TEST_PASS();
}

static void
test_infer_memcpy_zero_input(void)
{
	TEST_START("cs_infer_bytes_produced: MEMCPY, in=0 out=128 -> 0");
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(CS_CSF_MEMCPY, NULL, 0, NULL, 128),
		   "MEMCPY with zero input should produce 0");
	TEST_PASS();
}

/* ── cs_infer_bytes_produced: FILTER_GT ──────────────── */

static void
test_infer_filter_gt_all_valid(void)
{
	TEST_START("cs_infer_bytes_produced: FILTER_GT, 3 valid floats -> 12");
	float out[] = {1.0f, 2.0f, 3.0f};
	ASSERT_EQ(3 * sizeof(float),
		   cs_infer_bytes_produced(CS_CSF_FILTER_GT, NULL, 0, out, sizeof(out)),
		   "3 valid floats should produce 12 bytes");
	TEST_PASS();
}

static void
test_infer_filter_gt_with_nan(void)
{
	TEST_START("cs_infer_bytes_produced: FILTER_GT, NaN at index 2 -> 8");
	float out[] = {1.0f, 2.0f, NAN, 4.0f};
	ASSERT_EQ(2 * sizeof(float),
		   cs_infer_bytes_produced(CS_CSF_FILTER_GT, NULL, 0, out, sizeof(out)),
		   "NaN at index 2 should stop, producing 8 bytes");
	TEST_PASS();
}

static void
test_infer_filter_gt_all_nan(void)
{
	TEST_START("cs_infer_bytes_produced: FILTER_GT, all NaN -> 0");
	float out[] = {NAN, NAN, NAN};
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(CS_CSF_FILTER_GT, NULL, 0, out, sizeof(out)),
		   "all NaN should produce 0 bytes");
	TEST_PASS();
}

static void
test_infer_filter_gt_null_output(void)
{
	TEST_START("cs_infer_bytes_produced: FILTER_GT, NULL output -> 0");
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(CS_CSF_FILTER_GT, NULL, 0, NULL, 100),
		   "NULL output should produce 0");
	TEST_PASS();
}

static void
test_infer_filter_gt_odd_size(void)
{
	TEST_START("cs_infer_bytes_produced: FILTER_GT, odd output size -> 0");
	float out[] = {1.0f, 2.0f, 3.0f};
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(CS_CSF_FILTER_GT, NULL, 0, out, 11),
		   "odd output size should produce 0");
	TEST_PASS();
}

/* ── cs_infer_bytes_produced: RLE_COMPRESS ───────────── */

static void
test_infer_rle_normal(void)
{
	TEST_START("cs_infer_bytes_produced: RLE, 3 pairs -> 6");
	uint8_t out[] = {1, 5, 2, 3, 4, 1, 0, 0};
	ASSERT_EQ(6,
		   cs_infer_bytes_produced(CS_CSF_RLE_COMPRESS, NULL, 0, out, sizeof(out)),
		   "3 pairs before zero-terminator should produce 6 bytes");
	TEST_PASS();
}

static void
test_infer_rle_immediate_term(void)
{
	TEST_START("cs_infer_bytes_produced: RLE, immediate zero -> 0");
	uint8_t out[] = {0, 0, 1, 2};
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(CS_CSF_RLE_COMPRESS, NULL, 0, out, sizeof(out)),
		   "immediate zero should produce 0 bytes");
	TEST_PASS();
}

static void
test_infer_rle_no_term(void)
{
	TEST_START("cs_infer_bytes_produced: RLE, no terminator -> full size");
	uint8_t out[] = {1, 2, 3, 4, 5, 6};
	ASSERT_EQ(6,
		   cs_infer_bytes_produced(CS_CSF_RLE_COMPRESS, NULL, 0, out, sizeof(out)),
		   "no terminator should produce full buffer size");
	TEST_PASS();
}

static void
test_infer_rle_null_output(void)
{
	TEST_START("cs_infer_bytes_produced: RLE, NULL output -> 0");
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(CS_CSF_RLE_COMPRESS, NULL, 0, NULL, 100),
		   "NULL output should produce 0");
	TEST_PASS();
}

static void
test_infer_rle_odd_size(void)
{
	TEST_START("cs_infer_bytes_produced: RLE, odd output size -> 0");
	uint8_t out[] = {1, 2, 3};
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(CS_CSF_RLE_COMPRESS, NULL, 0, out, 3),
		   "odd output size should produce 0");
	TEST_PASS();
}

/* ── cs_infer_bytes_produced: default (unknown CSF) ──── */

static void
test_infer_default(void)
{
	TEST_START("cs_infer_bytes_produced: unknown CSF, out=256 -> 256");
	uint8_t out[256];
	ASSERT_EQ(256,
		   cs_infer_bytes_produced(0xFF, NULL, 0, out, sizeof(out)),
		   "unknown CSF should return output_size");
	TEST_PASS();
}

static void
test_infer_default_zero(void)
{
	TEST_START("cs_infer_bytes_produced: unknown CSF, out=0 -> 0");
	ASSERT_EQ(0,
		   cs_infer_bytes_produced(0xFF, NULL, 0, NULL, 0),
		   "unknown CSF with zero output should return 0");
	TEST_PASS();
}

/* ── Main ─────────────────────────────────────────────── */

int
main(void)
{
	printf("\n");
	printf("===========================================\n");
	printf("  xnvme_uri_is_local_direct + cs_infer_bytes_produced\n");
	printf("  Unit Tests\n");
	printf("===========================================\n\n");

	printf("Running tests:\n\n");

	/* xnvme_uri_is_local_direct */
	printf("--- xnvme_uri_is_local_direct ---\n\n");
	test_uri_null();
	test_uri_empty();
	test_uri_block_device();
	test_uri_pci();
	test_uri_pcie();
	test_uri_fabric_hostport();
	test_uri_fabric_localhost();
	test_uri_scheme();

	/* cs_infer_bytes_produced: fixed-output */
	printf("\n--- cs_infer_bytes_produced: fixed-output CSFs ---\n\n");
	test_infer_sum64_normal();
	test_infer_sum64_small();
	test_infer_sum64_oversize();
	test_infer_max64();
	test_infer_min64();
	test_infer_dot_product();
	test_infer_l2_distance();
	test_infer_cosine();
	test_infer_multi_agg64();
	test_infer_multi_agg64_small();

	/* cs_infer_bytes_produced: MEMCPY */
	printf("\n--- cs_infer_bytes_produced: MEMCPY ---\n\n");
	test_infer_memcpy_input_smaller();
	test_infer_memcpy_output_smaller();
	test_infer_memcpy_equal();
	test_infer_memcpy_zero_input();

	/* cs_infer_bytes_produced: FILTER_GT */
	printf("\n--- cs_infer_bytes_produced: FILTER_GT ---\n\n");
	test_infer_filter_gt_all_valid();
	test_infer_filter_gt_with_nan();
	test_infer_filter_gt_all_nan();
	test_infer_filter_gt_null_output();
	test_infer_filter_gt_odd_size();

	/* cs_infer_bytes_produced: RLE_COMPRESS */
	printf("\n--- cs_infer_bytes_produced: RLE_COMPRESS ---\n\n");
	test_infer_rle_normal();
	test_infer_rle_immediate_term();
	test_infer_rle_no_term();
	test_infer_rle_null_output();
	test_infer_rle_odd_size();

	/* cs_infer_bytes_produced: default */
	printf("\n--- cs_infer_bytes_produced: default ---\n\n");
	test_infer_default();
	test_infer_default_zero();

	TEST_SUMMARY();

	return TEST_EXIT();
}
