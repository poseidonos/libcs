/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_nvme_cmds.c
 * @brief CP/SLM command builders, transcribed from xnvme/lib/xnvme_cs.c and
 * xnvme/lib/xnvme_slm.c onto the libcs-native shim (cs_nvme.h).
 *
 * Commit 2 of the 10-commit xNVMe-fork -> direct-SPDK port (see
 * ~/.claude/plans/hidden-sprouting-turtle.md). Each of the 7 functions below
 * is split into:
 *
 *   - a pure, non-static "_prep" helper (cs_nvme_cmds_prep_*) that only
 *     fills a `struct cs_nvme_cmd` (and, for SLM copy, a descriptor buffer)
 *     from already-resolved inputs -- no device access, no I/O. These are
 *     what tests/test_cmd_oracle.c calls directly for the golden-vector
 *     differential check, and what cs_nvme_cmds.c's own public wrapper
 *     below it calls after resolving nsid/env/padding.
 *   - the public wrapper, matching the original xnvme_cmd_* signature
 *     exactly: validates, resolves the nsid fallback (nsid ? nsid :
 *     xnvme_dev_get_nsid(ctx->dev)), does any buffer padding/allocation,
 *     calls the _prep helper to fill ctx->cmd, then calls
 *     xnvme_cmd_pass()/xnvme_cmd_pass_admin().
 *
 * xnvme_dev_get_nsid(), xnvme_cmd_pass(), xnvme_cmd_pass_admin(),
 * xnvme_buf_alloc()/xnvme_buf_free() are declared in cs_nvme.h but not
 * implemented until commit 3 (cs_nvme_spdk.c). That's fine for this file --
 * it only needs them to link, not to run; the golden-vector test only calls
 * the _prep helpers, never the public wrappers. The public wrappers are
 * still written and compiled now (not deferred) because commit 5's backend
 * port needs them to already exist with final signatures, and because
 * leaving them for later would make the eventual port commit (5) mix
 * "write new code" with "swap one include line", defeating the whole
 * point of the shim-first approach (see cs_nvme.h's file header).
 *
 * All CDW/opcode/SEL semantics below are transcribed verbatim from the
 * fork source with file:line references preserved in comments, matching
 * tests/test_cmd_oracle.c's golden vectors (commit 1) and cross-checked
 * against them by this same oracle in commit 2.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cs_internal.h" /* OPC_CP_LOAD / OPC_CP_ACTIVATE / OPC_CP_MEM_RANGE / OPC_CP_EXECUTE / OPC_SLM_COPY */
#include "cs_nvme.h"

/* ───────────────────────── validate ───────────────────────── *
 * xnvme_cs.c:12 xnvme_cmd_cp_validate_dbuf()
 */
int
xnvme_cmd_cp_validate_dbuf(const void *buffer, size_t buf_len)
{
	if (!buffer || !buf_len) {
		return -EINVAL;
	}
	if (buf_len % sizeof(uint32_t)) {
		return -EINVAL;
	}
	if (buf_len > (UINT32_MAX - 4095u)) {
		return -EINVAL;
	}
	return 0;
}

/* ───────────────────────── 1. CP_LOAD ─────────────────────────
 * xnvme_cs.c:33 xnvme_cmd_cp_load(), ADMIN queue.
 */

void
cs_nvme_cmds_prep_cp_load(struct cs_nvme_cmd *cmd, uint32_t nsid, uint16_t prog_index,
			  uint32_t psize, uint32_t ptype)
{
	const uint32_t pit = 0; /* xnvme_cs.c:36 */
	const uint32_t sel = 0; /* xnvme_cs.c:37 */

	memset(cmd, 0, sizeof(*cmd));
	cmd->common.opcode = (uint8_t)OPC_CP_LOAD;
	cmd->common.nsid = nsid;
	cmd->cp.cdw10 = (pit << 25) | (sel << 24) | (ptype << 16) | prog_index;
	cmd->cp.cdw11 = psize;
	cmd->cp.cdw12 = 0;
	cmd->cp.cdw13 = 0;
	cmd->cp.cdw14 = psize;
	cmd->cp.cdw15 = 0;
}

int
xnvme_cmd_cp_load(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint16_t prog_index,
		  const void *buffer, size_t buf_len)
{
	const uint32_t lpg_align = 4096;
	uint32_t ptype = 0;
	uint32_t psize;
	void *padded = NULL;
	uint32_t resolved_nsid;
	int err;

	err = xnvme_cmd_cp_validate_dbuf(buffer, buf_len);
	if (err) {
		return err;
	}

	/* CPCS_XNVME_PTYPE: libcs inherits this env var from the fork, which
	 * read it internally -- keeping the same name preserves current
	 * behavior (see design plan, env-rename section: this one is NOT
	 * renamed to LIBCS_SPDK_*). */
	{
		const char *env = getenv("CPCS_XNVME_PTYPE");
		if (env && env[0] != '\0') {
			ptype = (uint32_t)strtoul(env, NULL, 0);
		}
	}

	psize = ((uint32_t)buf_len + lpg_align - 1) & ~(lpg_align - 1); /* xnvme_cs.c:55 */

	if (psize != (uint32_t)buf_len) {
		padded = xnvme_buf_alloc(ctx->dev, psize);
		if (!padded) {
			return -ENOMEM;
		}
		memset(padded, 0, psize);
		memcpy(padded, buffer, buf_len);
		buffer = padded;
	}

	resolved_nsid = nsid ? nsid : xnvme_dev_get_nsid(ctx->dev);
	cs_nvme_cmds_prep_cp_load(&ctx->cmd, resolved_nsid, prog_index, psize, ptype);

	err = xnvme_cmd_pass_admin(ctx, (void *)buffer, psize, NULL, 0);
	if (padded) {
		xnvme_buf_free(ctx->dev, padded);
	}
	return err;
}

/* ───────────────────────── 2. CP_ACTIVATE ─────────────────────────
 * xnvme_cs.c:85 xnvme_cmd_cp_activate(), ADMIN queue.
 * sel = action ? 0 : 1  (INVERTED: action=0/activate -> sel=1;
 *                        action=1/deactivate -> sel=0)
 */

void
cs_nvme_cmds_prep_cp_activate(struct cs_nvme_cmd *cmd, uint32_t nsid, uint16_t prog_index,
			      uint8_t action)
{
	const uint32_t sel = action ? 0u : 1u; /* xnvme_cs.c:91 */

	memset(cmd, 0, sizeof(*cmd));
	cmd->common.opcode = (uint8_t)OPC_CP_ACTIVATE;
	cmd->common.nsid = nsid;
	cmd->cp.cdw10 = (sel << 16) | prog_index;
	cmd->cp.cdw11 = 0;
	cmd->cp.cdw12 = 0;
	cmd->cp.cdw13 = 0;
	cmd->cp.cdw14 = 0;
	cmd->cp.cdw15 = 0;
}

int
xnvme_cmd_cp_activate(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint16_t prog_index,
		      uint8_t action)
{
	uint32_t resolved_nsid = nsid ? nsid : xnvme_dev_get_nsid(ctx->dev);

	cs_nvme_cmds_prep_cp_activate(&ctx->cmd, resolved_nsid, prog_index, action);

	/* cpcs_admin_program_activation() only reads CDW10 (PIND/SEL); it
	 * never touches a data buffer, matching the no-buffer precedent used
	 * by xnvme_cmd_cp_mem_range_delete() below. */
	return xnvme_cmd_pass_admin(ctx, NULL, 0, NULL, 0);
}

/* ───────────────────────── 3. CP_EXECUTE (buf) ─────────────────────────
 * xnvme_cs.c:108 xnvme_cmd_cp_execute_buf(), IO queue.
 * Writes cdw00_09[2..4] (= CDW02..CDW04), NOT cdw10+:
 *   CDW02 = (mrs_id<<16)|prog_index
 *   CDW03 = 0
 *   CDW04 = buf_len            <- the DLEN field; no kernel-passthru
 *                                  equivalent (motivates direct-SPDK port)
 */

void
cs_nvme_cmds_prep_cp_execute_buf(struct cs_nvme_cmd *cmd, uint32_t nsid, uint16_t prog_index,
				 uint16_t mrs_id, uint32_t buf_len)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->common.opcode = (uint8_t)OPC_CP_EXECUTE;
	cmd->common.nsid = nsid;
	cmd->cp.cdw00_09[2] = ((uint32_t)mrs_id << 16) | prog_index; /* CDW02, xnvme_cs.c:122 */
	cmd->cp.cdw00_09[3] = 0;				      /* CDW03, xnvme_cs.c:123 */
	cmd->cp.cdw00_09[4] = buf_len;				      /* CDW04, xnvme_cs.c:124 */
	cmd->cp.cdw10 = 0;
	cmd->cp.cdw11 = 0;
	cmd->cp.cdw12 = 0;
	cmd->cp.cdw13 = 0;
	cmd->cp.cdw14 = 0;
	cmd->cp.cdw15 = 0;
}

int
xnvme_cmd_cp_execute_buf(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint16_t prog_index,
			 uint16_t mrs_id, const void *buffer, size_t buf_len)
{
	uint32_t resolved_nsid;
	int err;

	err = xnvme_cmd_cp_validate_dbuf(buffer, buf_len);
	if (err) {
		return err;
	}

	resolved_nsid = nsid ? nsid : xnvme_dev_get_nsid(ctx->dev);
	cs_nvme_cmds_prep_cp_execute_buf(&ctx->cmd, resolved_nsid, prog_index, mrs_id,
					 (uint32_t)buf_len);

	return xnvme_cmd_pass(ctx, (void *)buffer, buf_len, NULL, 0);
}

/* ───────────────────────── 4. CP_MEM_RANGE (create) ─────────────────────────
 * xnvme_cs.c:136 xnvme_cmd_cp_mem_range(), ADMIN queue, sel=0 (CREATE).
 * CDW11 = numr = nr (1-based NUMR, not nr-1).
 */

void
cs_nvme_cmds_prep_cp_mem_range(struct cs_nvme_cmd *cmd, uint32_t nsid, uint32_t mrs_id,
			       uint32_t numr)
{
	const uint32_t sel = 0; /* xnvme_cs.c:139, CREATE */

	memset(cmd, 0, sizeof(*cmd));
	cmd->common.opcode = (uint8_t)OPC_CP_MEM_RANGE;
	cmd->common.nsid = nsid;
	cmd->cp.cdw10 = (mrs_id << 16) | sel;
	cmd->cp.cdw11 = numr;
	cmd->cp.cdw12 = 0;
	cmd->cp.cdw13 = 0;
	cmd->cp.cdw14 = 0;
	cmd->cp.cdw15 = 0;
}

int
xnvme_cmd_cp_mem_range(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint32_t mrs_id, uint32_t nr,
		       const void *buffer, size_t buf_len)
{
	uint32_t resolved_nsid;
	int err;

	err = xnvme_cmd_cp_validate_dbuf(buffer, buf_len);
	if (err) {
		return err;
	}

	if (!nr || nr > 128) {
		return -EINVAL;
	}

	resolved_nsid = nsid ? nsid : xnvme_dev_get_nsid(ctx->dev);
	/* NOTE: CPCS NUMR is 1-based (number of ranges), unlike standard NVMe
	 * zero-based encoding. Verify against SNIA CPCS spec if changing. */
	cs_nvme_cmds_prep_cp_mem_range(&ctx->cmd, resolved_nsid, mrs_id, nr);

	return xnvme_cmd_pass_admin(ctx, (void *)buffer, buf_len, NULL, 0);
}

/* ───────────────────────── 5. CP_MEM_RANGE (delete) ─────────────────────────
 * xnvme_cs.c:171 xnvme_cmd_cp_mem_range_delete(), ADMIN queue,
 * same opcode as create (0x89), sel=1 (DELETE). NUMR left 0.
 */

void
cs_nvme_cmds_prep_cp_mem_range_delete(struct cs_nvme_cmd *cmd, uint32_t nsid, uint16_t mrs_id)
{
	const uint32_t sel = 1; /* xnvme_cs.c:177, DELETE */

	memset(cmd, 0, sizeof(*cmd));
	cmd->common.opcode = (uint8_t)OPC_CP_MEM_RANGE;
	cmd->common.nsid = nsid;
	cmd->cp.cdw10 = ((uint32_t)mrs_id << 16) | sel;
	cmd->cp.cdw11 = 0;
	cmd->cp.cdw12 = 0;
	cmd->cp.cdw13 = 0;
	cmd->cp.cdw14 = 0;
	cmd->cp.cdw15 = 0;
}

int
xnvme_cmd_cp_mem_range_delete(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint16_t mrs_id)
{
	uint32_t resolved_nsid = nsid ? nsid : xnvme_dev_get_nsid(ctx->dev);

	/* SPDK_NVME_CPCS_MRS_OP_DELETE = 0x01. Delete takes no range data --
	 * NUMR is left 0, matching the target's cpcs_admin_mrs_management()
	 * dispatch, which only validates/consumes ranges on the CREATE path. */
	cs_nvme_cmds_prep_cp_mem_range_delete(&ctx->cmd, resolved_nsid, mrs_id);

	return xnvme_cmd_pass_admin(ctx, NULL, 0, NULL, 0);
}

/* ───────────────────────── 7. SLM_COPY_LBA ─────────────────────────
 * xnvme_slm.c:104 xnvme_cmd_slm_copy_lba(), IO queue.
 *
 * (xnvme_cmd_slm_copy()/FMT4 and xnvme_cmd_slm_read()/opcode 0x02 are
 * declared in the fork but never called by libcs -- not ported here, per
 * the design plan's §6 note.)
 */

void
cs_nvme_cmds_prep_slm_copy_lba(struct cs_nvme_cmd *cmd, uint32_t dst_nsid, uint64_t dst_addr,
			       uint64_t nbytes, uint32_t desc_fmt)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->common.opcode = (uint8_t)OPC_SLM_COPY;
	cmd->common.nsid = dst_nsid;
	cmd->common.cdw02 = (uint32_t)nbytes;		  /* xnvme_slm.c:164 */
	cmd->common.cdw03 = (uint32_t)(nbytes >> 32);	  /* xnvme_slm.c:165 */
	cmd->common.cdw10 = (uint32_t)dst_addr;	  /* xnvme_slm.c:166 */
	cmd->common.cdw11 = (uint32_t)(dst_addr >> 32);  /* xnvme_slm.c:167 */
	cmd->common.cdw12 = (desc_fmt << 8) | 0u;	  /* xnvme_slm.c:170 */
	cmd->common.cdw13 = 0;
	cmd->common.cdw14 = 0;
	cmd->common.cdw15 = 0;
}

void
cs_nvme_cmds_prep_slm_copy_lba_desc(struct cs_nvme_spec_slm_copy_desc_fmt23 *desc,
				    uint32_t src_nsid, uint64_t slba, uint16_t nlb)
{
	/* Explicit little-endian field packing (not a host-order struct
	 * assignment) -- matches xnvme_to_le32()/xnvme_to_le64()/xnvme_to_le16()
	 * in xnvme_slm.c:177-179. Every deployment target for libcs (x86_64,
	 * aarch64) is little-endian, so this reduces to the same bit pattern
	 * as a plain assignment, but is written explicitly to document the
	 * on-wire contract rather than rely on it implicitly. */
	memset(desc, 0, sizeof(*desc));
	desc->snsid = src_nsid;
	desc->rsvd0 = 0;
	desc->slba = slba;
	desc->nlb = nlb;
	desc->rsvd1 = 0;
	desc->rsvd2 = 0;
	desc->eilbrt = 0;
	desc->elbat = 0;
	desc->elbatm = 0;
}

int
xnvme_cmd_slm_copy_lba(struct cs_nvme_cmd_ctx *ctx, uint32_t dst_nsid, uint64_t dst_addr,
		       uint32_t src_nsid, uint64_t slba, uint16_t nlb, uint64_t nbytes,
		       enum cs_nvme_slm_copy_desc_fmt desc_fmt)
{
	struct cs_nvme_spec_slm_copy_desc_fmt23 *desc = NULL;
	const struct cs_nvme_geo *geo = NULL;
	uint64_t expected_nbytes = 0;
	uint32_t resolved_nsid;
	int err;

	if (!nbytes) {
		return -EINVAL;
	}
	if (nbytes & 0x3) {
		return -EINVAL;
	}
	if (dst_addr & 0x3) {
		return -EINVAL;
	}
	if (desc_fmt != CS_NVME_SLM_COPY_DESC_FMT2 && desc_fmt != CS_NVME_SLM_COPY_DESC_FMT3) {
		return -EINVAL;
	}

	/* For LBA-based descriptors, validate command length against NLB and
	 * namespace LBA size. Skip when source and destination namespaces
	 * differ -- NLB refers to the source namespace's block count, whose
	 * LBA size is not known to the local device; the target validates the
	 * descriptor against the source namespace geometry in that case. */
	if (src_nsid == 0 || src_nsid == dst_nsid) {
		geo = xnvme_dev_get_geo(ctx->dev);
		if (geo && geo->lba_nbytes) {
			expected_nbytes = ((uint64_t)nlb + 1) * geo->lba_nbytes;
			if (expected_nbytes != nbytes) {
				return -EINVAL;
			}
		}
	}

	desc = xnvme_buf_alloc(ctx->dev, sizeof(*desc));
	if (!desc) {
		return -errno;
	}

	cs_nvme_cmds_prep_slm_copy_lba_desc(desc, src_nsid, slba, nlb);

	resolved_nsid = dst_nsid ? dst_nsid : xnvme_dev_get_nsid(ctx->dev);
	cs_nvme_cmds_prep_slm_copy_lba(&ctx->cmd, resolved_nsid, dst_addr, nbytes, (uint32_t)desc_fmt);

	err = xnvme_cmd_pass(ctx, desc, sizeof(*desc), NULL, 0);

	/* Safe on sync (xnvme_cmd_pass blocks until completion). On async,
	 * xnvme_cmd_pass returns immediately and the transport still
	 * references desc -- freeing here would be a use-after-free. Callers
	 * that need async must manage descriptor lifetime themselves. */
	xnvme_buf_free(ctx->dev, desc);

	return err;
}
