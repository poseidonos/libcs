/*
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * test_compute_from_nvm.c — verify csRunComputeFromNvm produces the same
 * SUM64 result as csRunComputeExCached (inline H2D path) on the same data.
 *
 * Build on .28:
 *   gcc -O2 -Wall -Ilibcs/include \
 *       libcs/tests/test_compute_from_nvm.c \
 *       -Llibcs/build -lcs -Lxnvme/build/lib -lxnvme \
 *       -lm -lpthread -o test_compute_from_nvm
 *
 * Run on .28 (target must be running on .24):
 *   LIBCS_XNVME_BE=spdk \
 *   LIBCS_XNVME_SUBNQN=nqn.2024-01.io.example:libcs \
 *   LIBCS_XNVME_NSID=2 \
 *   LIBCS_XNVME_SHM_ID=0 \
 *   LIBCS_XNVME_SLM_NSID=100 \
 *   LD_LIBRARY_PATH=libcs/build:xnvme/build/lib \
 *   ./test_compute_from_nvm --uri 192.0.2.1:4420 --len 1048576
 */

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cs_api.h"
#include "cs_helpers.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char *argv[])
{
    const char *uri = NULL;
    size_t data_len = 1048576; /* 1 MB default */
    uint32_t nvm_nsid = 1;
    uint32_t slm_nsid = 100;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--uri") && i + 1 < argc) {
            uri = argv[++i];
        } else if (!strcmp(argv[i], "--len") && i + 1 < argc) {
            data_len = (size_t)strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--nvm-nsid") && i + 1 < argc) {
            nvm_nsid = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--slm-nsid") && i + 1 < argc) {
            slm_nsid = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
    }

    if (!uri) {
        fprintf(stderr, "Usage: %s --uri <traddr:port> [--len <bytes>] "
                        "[--nvm-nsid <n>] [--slm-nsid <n>]\n", argv[0]);
        return 1;
    }

    uint32_t n_uint64 = (uint32_t)(data_len / sizeof(uint64_t));

    /* Generate input data: in_buf[i] = i+1 */
    uint64_t *in_buf = malloc(data_len);
    if (!in_buf) {
        fprintf(stderr, "malloc failed\n");
        return 2;
    }
    for (uint32_t i = 0; i < n_uint64; i++)
        in_buf[i] = (uint64_t)(i + 1);

    /* Expected SUM64 = sum(1..n) = n*(n+1)/2 */
    uint64_t expected = (uint64_t)n_uint64 * (n_uint64 + 1) / 2;

    printf("=== Test: SUM64 over %u uint64 (%zu bytes) ===\n", n_uint64, data_len);
    printf("  Expected sum = %" PRIu64 "\n", expected);
    printf("  nvm_nsid=%u  slm_nsid=%u\n\n", nvm_nsid, slm_nsid);

    /*
     * Path 1: csRunComputeExCached with LIBCS_MRS_STAGE_VIA_NVM=1
     *
     * This writes data to NVM (nsid=ctx->nsid, e.g. 1), then stages
     * NVM->SLM, then MRS execute.  The data remains in NVM after this
     * call, which is exactly what Path 2 needs.
     */
    setenv("LIBCS_MRS_STAGE_VIA_NVM", "1", 1);

    uint64_t out_inline = 0;
    csComputeResult cr_inline = {0};

    uint64_t t0 = now_ns();
    csStatus st1 = csRunComputeExCached(uri, CS_CSF_SUM64,
                                        in_buf, data_len,
                                        &out_inline, sizeof(out_inline),
                                        NULL, 0, &cr_inline);
    uint64_t t1 = now_ns();

    double inline_ms = (double)(t1 - t0) / 1e6;
    csLatencyBreakdown lb = csGetLastLatencyBreakdown();

    printf("=== Path 1: csRunComputeExCached (H2D via NVM staging) ===\n");
    printf("  status=%d  result=%" PRIu64 "  elapsed=%.3f ms\n",
           (int)st1, out_inline, inline_ms);
    printf("  h2d=%.3f  execute=%.3f  d2h=%.3f ms\n",
           lb.h2d_ms, lb.execute_ms, lb.d2h_ms);

    csRunComputeExCachedCleanup();

    /* ── Path 2: csRunComputeFromNvm (NVM→SLM staging + MRS, no H2D) ── */
    /*
     * Data was written to NVM by Path 1 (LIBCS_MRS_STAGE_VIA_NVM=1).
     * Now csRunComputeFromNvm reads from NVM, stages NVM->SLM (device-
     * internal copy), creates MRS, and executes — no host-to-device
     * data transfer.
     */
    uint64_t out_nvm = 0;
    csComputeResult cr_nvm = {0};

    t0 = now_ns();
    csStatus st2 = csRunComputeFromNvm(uri,
                                       nvm_nsid, 0,      /* nvm_nsid, nvm_slba */
                                       slm_nsid, 0,      /* slm_nsid, slm_byte_off */
                                       n_uint64,
                                       CS_CSF_SUM64,
                                       0, 0,              /* threshold, batch */
                                       &out_nvm, sizeof(out_nvm),
                                       &cr_nvm);
    t1 = now_ns();

    double nvm_ms = (double)(t1 - t0) / 1e6;

    printf("\n=== Path 2: csRunComputeFromNvm (NVM→SLM + MRS, no H2D) ===\n");
    printf("  status=%d  result=%" PRIu64 "  elapsed=%.3f ms\n",
           (int)st2, out_nvm, nvm_ms);

    /* ── Verify ── */
    printf("\n=== Verification ===\n");
    int rc = 1;

    if (st1 != CS_STATUS_SUCCESS) {
        fprintf(stderr, "  FAIL: Path 1 status=%d\n", (int)st1);
    } else if (st2 != CS_STATUS_SUCCESS) {
        fprintf(stderr, "  FAIL: Path 2 status=%d\n", (int)st2);
    } else if (out_inline != out_nvm) {
        fprintf(stderr, "  FAIL: results differ (inline=%" PRIu64 ", nvm=%" PRIu64 ")\n",
                out_inline, out_nvm);
    } else if (out_nvm != expected) {
        fprintf(stderr, "  FAIL: result mismatch (got=%" PRIu64 ", expected=%" PRIu64 ")\n",
                out_nvm, expected);
    } else {
        printf("  PASS: all results match (inline=%" PRIu64 ", nvm=%" PRIu64 ", expected=%" PRIu64 ")\n",
               out_inline, out_nvm, expected);
        rc = 0;
    }

    csRunComputeExCachedCleanup();
    free(in_buf);
    return rc;
}
