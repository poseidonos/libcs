/*
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libxnvme.h>

#define LIBCS_NSID  2
#define NVM_NSID   1
#define SLM_NSID   100
#define PIND_SUM64 2

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s --uri <uri> --subnqn <nqn> [options]\n"
		"  --be <spdk|linux> (default spdk)\n  --count <n> (default 64)\n", prog);
}

int main(int argc, char *argv[])
{
	const char *uri = NULL, *subnqn = NULL, *be = "spdk";
	uint32_t count = 64;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--uri") && i+1 < argc) uri = argv[++i];
		else if (!strcmp(argv[i], "--subnqn") && i+1 < argc) subnqn = argv[++i];
		else if (!strcmp(argv[i], "--be") && i+1 < argc) be = argv[++i];
		else if (!strcmp(argv[i], "--count") && i+1 < argc) count = (uint32_t)strtoul(argv[++i], NULL, 0);
		else { usage(argv[0]); return 1; }
	}
	if (!uri || !subnqn) { usage(argv[0]); return 1; }
	if (count > 512) { fprintf(stderr, "count too large\n"); return 1; }

	size_t data_bytes = count * sizeof(uint64_t);
	uint64_t expected = 0;
	for (uint32_t i = 0; i < count; i++) expected += (uint64_t)(i + 1);
	printf("[uri=%s subnqn=%s be=%s count=%u data=%zu expected=%lu]\n", uri, subnqn, be, count, data_bytes, expected);

	struct xnvme_opts opts;
	memset(&opts, 0, sizeof(opts));
	opts.nsid = NVM_NSID;
	opts.be = be;
	opts.subnqn = subnqn;
	opts.adrfam = "IPv4";

	struct xnvme_dev *dev = xnvme_dev_open(uri, &opts);
	if (!dev) { fprintf(stderr, "FAIL: xnvme_dev_open: %s\n", strerror(errno)); return 1; }
	printf("  dev_open: OK (nsid=%d)\n", xnvme_dev_get_nsid(dev));

	struct xnvme_opts slm_opts = opts;
	slm_opts.nsid = SLM_NSID;
	struct xnvme_dev *slm_dev = xnvme_dev_open(uri, &slm_opts);
	if (!slm_dev) fprintf(stderr, "WARN: SLM dev open: %s\n", strerror(errno));
	else printf("  SLM dev_open: OK (nsid=%d)\n", xnvme_dev_get_nsid(slm_dev));

	uint64_t *data_buf = (uint64_t *)xnvme_buf_alloc(dev, data_bytes);
	if (!data_buf) { fprintf(stderr, "FAIL: buf_alloc: %s\n", strerror(errno)); goto out; }
	for (uint32_t i = 0; i < count; i++) data_buf[i] = (uint64_t)(i + 1);
	printf("  data_buf: OK (%zu bytes)\n", data_bytes);

	/* Step 1: NVM write */
	printf("\n--- Step 1: NVM write (nsid=%d) ---\n", NVM_NSID);
	{
		struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(dev);
		uint32_t nblocks = (uint32_t)(data_bytes / 512);
		if (nblocks == 0) nblocks = 1;
		int rc = xnvme_nvm_write(&cmd, NVM_NSID, 0, (uint16_t)(nblocks - 1), data_buf, NULL);
		if (rc || xnvme_cmd_ctx_cpl_status(&cmd))
			fprintf(stderr, "  NVM write: FAIL rc=%d sc=%u sct=%u\n", rc, cmd.cpl.status.sc, cmd.cpl.status.sct);
		else
			printf("  NVM write: OK (%u blocks)\n", nblocks);
	}

	/* Step 2: SLM copy */
	if (slm_dev) {
		printf("\n--- Step 2: SLM copy (nsid=%d -> %d) ---\n", NVM_NSID, SLM_NSID);
		struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(slm_dev);
		uint32_t nblocks = (uint32_t)(data_bytes / 512);
		if (nblocks == 0) nblocks = 1;
		size_t chunk = (size_t)nblocks * 512;
		int rc = xnvme_cmd_slm_copy_lba(&cmd, SLM_NSID, 0, NVM_NSID, 0,
			(uint16_t)(nblocks - 1), chunk, XNVME_SLM_COPY_DESC_FMT2);
		if (rc || xnvme_cmd_ctx_cpl_status(&cmd))
			fprintf(stderr, "  SLM copy: FAIL rc=%d sc=%u sct=%u\n", rc, cmd.cpl.status.sc, cmd.cpl.status.sct);
		else
			printf("  SLM copy: OK (%u blocks)\n", nblocks);
	}

	/* Step 3: Create MRS */
	uint16_t rsid = 0;
	{
		printf("\n--- Step 3: MRS create (CP Mem Range, nsid=%d) ---\n", LIBCS_NSID);
		uint8_t *dma_mrs = xnvme_buf_alloc(dev, 32);
		if (!dma_mrs) { fprintf(stderr, "  MRS buf: FAIL\n"); }
		else {
			memset(dma_mrs, 0, 32);
			((uint32_t *)dma_mrs)[0] = SLM_NSID;
			((uint32_t *)dma_mrs)[1] = (uint32_t)data_bytes;
			((uint64_t *)(dma_mrs + 8))[0] = 0;
			struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(dev);
			int rc = xnvme_cmd_cp_mem_range(&cmd, LIBCS_NSID, 0, 1, dma_mrs, 32);
			if (rc || xnvme_cmd_ctx_cpl_status(&cmd))
				fprintf(stderr, "  MRS create: FAIL rc=%d sc=%u sct=%u\n", rc, cmd.cpl.status.sc, cmd.cpl.status.sct);
			else {
				rsid = (uint16_t)(cmd.cpl.cdw0 ? cmd.cpl.cdw0 : 1);
				printf("  MRS create: OK (rsid=%u)\n", rsid);
			}
			xnvme_buf_free(dev, dma_mrs);
		}
	}

	/* Step 4: Execute SUM64 via MRS */
	if (rsid > 0) {
		printf("\n--- Step 4: CP Execute SUM64 (MRS rsid=%u) ---\n", rsid);
		size_t desc_size = 24;
		uint8_t *desc = xnvme_buf_alloc(dev, desc_size);
		if (desc) {
			memset(desc, 0, desc_size);
			((uint64_t *)desc)[0] = 1;            /* mr_id = 1 */
			((uint64_t *)desc)[1] = 0;            /* offset = 0 */
			((uint64_t *)desc)[2] = data_bytes;   /* length */
			struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(dev);
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			int rc = xnvme_cmd_cp_execute_buf(&cmd, LIBCS_NSID, PIND_SUM64, rsid, desc, desc_size);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
			if (rc || xnvme_cmd_ctx_cpl_status(&cmd))
				fprintf(stderr, "  MRS execute: FAIL rc=%d sc=%u sct=%u result=0x%lx (%.3f ms)\n",
					rc, cmd.cpl.status.sc, cmd.cpl.status.sct, (unsigned long)cmd.cpl.result, ms);
			else {
				uint64_t result = (uint64_t)cmd.cpl.result;
				printf("  MRS execute: OK result=%lu expected=%lu (%.3f ms) -> %s\n",
					result, expected, ms, result == expected ? "PASS" : "FAIL");
			}
			xnvme_buf_free(dev, desc);
		}
	}

	/* Step 5: Raw inline (what xnvme backend currently does) */
	printf("\n--- Step 5: Raw inline (rsid=0, raw data) ---\n");
	{
		struct xnvme_cmd_ctx cmd = xnvme_cmd_ctx_from_dev(dev);
		int rc = xnvme_cmd_cp_execute_buf(&cmd, LIBCS_NSID, PIND_SUM64, 0, data_buf, data_bytes);
		if (rc || xnvme_cmd_ctx_cpl_status(&cmd))
			fprintf(stderr, "  Raw inline: FAIL rc=%d sc=%u sct=%u\n", rc, cmd.cpl.status.sc, cmd.cpl.status.sct);
		else {
			uint64_t result = (uint64_t)cmd.cpl.result;
			printf("  Raw inline: OK result=%lu expected=%lu -> %s\n",
				result, expected, result == expected ? "PASS" : "FAIL");
		}
	}

	xnvme_buf_free(dev, data_buf);
out:
	xnvme_dev_close(dev);
	if (slm_dev) xnvme_dev_close(slm_dev);
	return 0;
}
