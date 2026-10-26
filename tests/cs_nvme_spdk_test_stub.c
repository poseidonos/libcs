/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_nvme_spdk_test_stub.c
 * @brief TEMPORARY stub bodies for the cs_nvme.h transport functions that
 * src/backend/cs_nvme_cmds.c's public wrappers call but that are not
 * implemented until commit 3 (cs_nvme_spdk.c).
 *
 * This file exists ONLY so that tests/test_cmd_oracle.c (which links
 * cs_nvme_cmds.c in order to call its `_prep` helpers for the golden-vector
 * differential check) can produce a complete, linkable binary. The oracle
 * test never calls the public wrappers (xnvme_cmd_cp_load() et al.) that
 * reference these symbols -- it only calls the pure `_prep` helpers -- so
 * these bodies are never executed; they only need to satisfy the linker.
 *
 * Superseded by cs_nvme_spdk.c in commit 3. At that point this stub file
 * should be deleted and test_cmd_oracle's meson target relinked against the
 * real implementation (or, per the plan, the oracle itself is deleted once
 * cs_nvme_cmds.c has its own permanent golden-vector unit test).
 */

#include <errno.h>
#include <stddef.h>

#include "cs_nvme.h"

uint32_t
xnvme_dev_get_nsid(const struct cs_nvme_dev *dev)
{
	(void)dev;
	return 0;
}

int
xnvme_cmd_pass(struct cs_nvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *mbuf,
	       size_t mbuf_nbytes)
{
	(void)ctx;
	(void)dbuf;
	(void)dbuf_nbytes;
	(void)mbuf;
	(void)mbuf_nbytes;
	return -ENOSYS;
}

int
xnvme_cmd_pass_admin(struct cs_nvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *mbuf,
		     size_t mbuf_nbytes)
{
	(void)ctx;
	(void)dbuf;
	(void)dbuf_nbytes;
	(void)mbuf;
	(void)mbuf_nbytes;
	return -ENOSYS;
}

void *
xnvme_buf_alloc(const struct cs_nvme_dev *dev, size_t nbytes)
{
	(void)dev;
	(void)nbytes;
	return NULL;
}

void
xnvme_buf_free(const struct cs_nvme_dev *dev, void *buf)
{
	(void)dev;
	(void)buf;
}

const struct cs_nvme_geo *
xnvme_dev_get_geo(const struct cs_nvme_dev *dev)
{
	(void)dev;
	return NULL;
}
