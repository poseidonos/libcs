/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file test_cmd_oracle.c
 * @brief Differential oracle for the 6 CP/SLM NVMe command builders.
 *
 * This is commit 1 of a 10-commit sequence porting libcs off the xNVMe fork
 * onto direct upstream SPDK. Right now this oracle only self-checks against
 * hand-transcribed formulas (hardcoded golden command vectors, derived by
 * reading xnvme/lib/xnvme_cs.c and xnvme/lib/xnvme_slm.c as of this commit)
 * plus the live xNVMe opcode/CSI enum values. Commit 2 will wire the new
 * cs_nvme_cmds.c builders' `_prep` functions into this same golden-vector
 * table via memcmp, turning it into a true differential check against the
 * fork. The final commit in the 10-commit sequence deletes this oracle once
 * cs_nvme_cmds.c has its own permanent golden-vector unit test.
 *
 * Why hardcoded golden vectors instead of calling the real xNVMe builders:
 * xnvme_cmd_cp_load() et al. fill ctx->cmd and then immediately call
 * xnvme_cmd_pass()/xnvme_cmd_pass_admin(), which dereference ctx->dev (nsid
 * lookup, geometry) and submit a real NVMe command. There is no device-free
 * way to invoke them and capture just the dword-fill without a real open
 * device. So this test re-derives the dword-packing arithmetic by hand from
 * the fork source and cross-checks two *independently written* forms of
 * each expected command image:
 *   1. a "formula" image, built in this file by named sub-expressions that
 *      mirror the fork's local variables (sel, psize, mrs_id<<16|prog_index,
 *      etc.) — this is what commit 2 will literally replace with calls into
 *      cs_nvme_cmds.c;
 *   2. a "golden" image, a plain literal dword table transcribed by hand
 *      from the same formulas, stored next to each case's input parameters.
 * A memcmp between the two catches transcription/arithmetic mistakes in
 * either form. The one genuinely live regression check is the opcode/CSI
 * enum cross-check against <libxnvme.h> (which transitively pulls in
 * libxnvme_cs.h / libxnvme_slm.h for the enum values) — if the fork ever
 * renumbers an opcode, this test fails immediately, even before
 * cs_nvme_cmds.c exists.
 *
 * No device needed, no xNVMe device open. Build/run standalone:
 *   meson test -C build cmd_oracle_tests -v
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Umbrella header: libxnvme_cs.h / libxnvme_slm.h are not standalone
 * includable (they use XNVME_STATIC_ASSERT / stdint types defined by
 * headers libxnvme.h includes first in the right order). This pulls them
 * in transitively, purely for the opcode/CSI enum constants below — no
 * device, no linkage to anything we actually call. */
#include <libxnvme.h>

#include "test_util.h"

/* ───────────────────────── mismatch reporting ───────────────────────── */

static bool
dwords_match(const uint32_t *formula, const uint32_t *golden, size_t n, char *msg, size_t msg_len,
	     const char *what)
{
	for (size_t i = 0; i < n; i++) {
		if (formula[i] != golden[i]) {
			snprintf(msg, msg_len,
				 "%s dword[%zu]: formula=0x%08x golden=0x%08x", what, i,
				 formula[i], golden[i]);
			return false;
		}
	}
	return true;
}

/* ───────────────────────── opcode/CSI enum cross-check ───────────────────────── *
 *
 * Compile-time: fails to build if the fork renumbers any of these. These
 * mirror the literal opcode bytes baked into every golden vector below, so
 * a drift here means every golden vector's dword[0] is now stale too.
 */
_Static_assert(XNVME_OPC_CP_LOAD == 0x85, "XNVME_OPC_CP_LOAD drifted from golden vectors");
_Static_assert(XNVME_OPC_CP_ACTIVATE == 0x88, "XNVME_OPC_CP_ACTIVATE drifted from golden vectors");
_Static_assert(XNVME_OPC_CP_MEM_RANGE == 0x89,
	       "XNVME_OPC_CP_MEM_RANGE drifted from golden vectors");
_Static_assert(XNVME_OPC_CP_EXECUTE == 0x01, "XNVME_OPC_CP_EXECUTE drifted from golden vectors");
_Static_assert(XNVME_OPC_SLM_MEM_COPY == 0x01,
	       "XNVME_OPC_SLM_MEM_COPY drifted from golden vectors");
_Static_assert(XNVME_SLM_COPY_DESC_FMT2 == 0x2,
	       "XNVME_SLM_COPY_DESC_FMT2 drifted from golden vectors");
_Static_assert(XNVME_SLM_COPY_DESC_FMT3 == 0x3,
	       "XNVME_SLM_COPY_DESC_FMT3 drifted from golden vectors");
_Static_assert(XNVME_CSI_SLM == 0x03, "XNVME_CSI_SLM drifted (documentation-only, not in dwords)");
_Static_assert(XNVME_CSI_CP == 0x04, "XNVME_CSI_CP drifted (documentation-only, not in dwords)");

static void
test_opcode_enum_values(void)
{
	TEST_START("xNVMe opcode/CSI enum values match golden-vector literals");
	/* The _Static_assert block above already enforces this at compile
	 * time; this runtime check exists so a passing build also shows up
	 * as a counted PASS in the test summary. */
	bool ok = (XNVME_OPC_CP_LOAD == 0x85) && (XNVME_OPC_CP_ACTIVATE == 0x88) &&
		  (XNVME_OPC_CP_MEM_RANGE == 0x89) && (XNVME_OPC_CP_EXECUTE == 0x01) &&
		  (XNVME_OPC_SLM_MEM_COPY == 0x01) && (XNVME_SLM_COPY_DESC_FMT2 == 0x2) &&
		  (XNVME_SLM_COPY_DESC_FMT3 == 0x3) && (XNVME_CSI_SLM == 0x03) &&
		  (XNVME_CSI_CP == 0x04);
	if (!ok) {
		TEST_FAIL("xnvme fork opcode/CSI enum values no longer match this oracle");
		return;
	}
	TEST_PASS();
}

/* ───────────────────────── 1. CP_LOAD ─────────────────────────
 * xnvme/lib/xnvme_cs.c:33 xnvme_cmd_cp_load(), ADMIN queue.
 * CDW10 = (pit<<25)|(sel<<24)|(ptype<<16)|prog_index  (pit=sel=0 always;
 *         ptype from env CPCS_XNVME_PTYPE, golden vectors assume unset -> 0)
 * CDW11 = psize = round_up(buf_len, 4096)
 * CDW12 = 0, CDW13 = 0, CDW14 = psize, CDW15 = 0
 */

static void
build_cp_load(uint32_t nsid, uint16_t prog_index, size_t buf_len, uint32_t out[16])
{
	const uint32_t pit = 0;   /* xnvme_cs.c:36 */
	const uint32_t sel = 0;   /* xnvme_cs.c:37 */
	const uint32_t ptype = 0; /* xnvme_cs.c:38-44, CPCS_XNVME_PTYPE unset for this oracle */
	const uint32_t lpg_align = 4096;
	uint32_t psize = ((uint32_t)buf_len + lpg_align - 1) & ~(lpg_align - 1); /* xnvme_cs.c:55 */

	memset(out, 0, 16 * sizeof(uint32_t));
	out[0] = (uint32_t)XNVME_OPC_CP_LOAD; /* dword0 byte0 = opcode */
	out[1] = nsid;			       /* dword1 = common.nsid */
	out[10] = (pit << 25) | (sel << 24) | (ptype << 16) | prog_index; /* cdw10 */
	out[11] = psize;						    /* cdw11 */
	out[14] = psize;						    /* cdw14 */
}

typedef struct {
	const char *name;
	uint32_t nsid;
	uint16_t prog_index;
	size_t buf_len;
	uint32_t golden[16]; /* CDW00..CDW15 */
} cp_load_case;

static const cp_load_case cp_load_cases[] = {
	{
		.name = "cp_load: nsid=1 prog_index=0 buf_len=4096 (exact page)",
		.nsid = 1,
		.prog_index = 0,
		.buf_len = 4096,
		.golden = {
			/* CDW00 */ 0x85, /* CDW01 (nsid) */ 1,
			/* CDW02..09 */ 0, 0, 0, 0, 0, 0, 0, 0,
			/* CDW10 */ 0x00000000, /* CDW11 (psize) */ 0x1000,
			/* CDW12 */ 0, /* CDW13 */ 0,
			/* CDW14 (psize) */ 0x1000, /* CDW15 */ 0,
		},
	},
	{
		.name = "cp_load: nsid=1 prog_index=3 buf_len=5000 (rounds up to 8192)",
		.nsid = 1,
		.prog_index = 3,
		.buf_len = 5000,
		.golden = {
			/* CDW00 */ 0x85, /* CDW01 (nsid) */ 1,
			/* CDW02..09 */ 0, 0, 0, 0, 0, 0, 0, 0,
			/* CDW10 (prog_index=3) */ 0x00000003, /* CDW11 (psize) */ 0x2000,
			/* CDW12 */ 0, /* CDW13 */ 0,
			/* CDW14 (psize) */ 0x2000, /* CDW15 */ 0,
		},
	},
};

/* ───────────────────────── 2. CP_ACTIVATE ─────────────────────────
 * xnvme/lib/xnvme_cs.c:85 xnvme_cmd_cp_activate(), ADMIN queue.
 * sel = action ? 0 : 1  (INVERTED: action=0/activate -> sel=1;
 *                        action=1/deactivate -> sel=0)
 * CDW10 = (sel<<16)|prog_index. CDW11..15 = 0.
 */

static void
build_cp_activate(uint32_t nsid, uint16_t prog_index, uint8_t action, uint32_t out[16])
{
	const uint32_t sel = action ? 0u : 1u; /* xnvme_cs.c:91 */

	memset(out, 0, 16 * sizeof(uint32_t));
	out[0] = (uint32_t)XNVME_OPC_CP_ACTIVATE;
	out[1] = nsid;
	out[10] = (sel << 16) | prog_index;
}

typedef struct {
	const char *name;
	uint32_t nsid;
	uint16_t prog_index;
	uint8_t action;
	uint32_t golden[16];
} cp_activate_case;

static const cp_activate_case cp_activate_cases[] = {
	{
		.name = "cp_activate: action=0 (activate) -> sel=1",
		.nsid = 1,
		.prog_index = 2,
		.action = 0,
		.golden = {
			/* CDW00 */ 0x88, /* CDW01 (nsid) */ 1,
			/* CDW02..09 */ 0, 0, 0, 0, 0, 0, 0, 0,
			/* CDW10 = (sel=1<<16)|prog_index=2 */ 0x00010002,
			/* CDW11..15 */ 0, 0, 0, 0, 0,
		},
	},
	{
		.name = "cp_activate: action=1 (deactivate) -> sel=0",
		.nsid = 1,
		.prog_index = 5,
		.action = 1,
		.golden = {
			/* CDW00 */ 0x88, /* CDW01 (nsid) */ 1,
			/* CDW02..09 */ 0, 0, 0, 0, 0, 0, 0, 0,
			/* CDW10 = (sel=0<<16)|prog_index=5 */ 0x00000005,
			/* CDW11..15 */ 0, 0, 0, 0, 0,
		},
	},
};

/* ───────────────────────── 3. CP_EXECUTE (buf) ─────────────────────────
 * xnvme/lib/xnvme_cs.c:110 xnvme_cmd_cp_execute_buf(), IO queue.
 * Writes cdw00_09[2..4] (= CDW02..CDW04), NOT cdw10+:
 *   CDW02 = (mrs_id<<16)|prog_index
 *   CDW03 = 0
 *   CDW04 = buf_len            <- the DLEN field; no kernel-passthru
 *                                  equivalent (motivates direct-SPDK port)
 * CDW10..15 = 0.
 */

static void
build_cp_execute_buf(uint32_t nsid, uint16_t prog_index, uint16_t mrs_id, size_t buf_len,
		      uint32_t out[16])
{
	memset(out, 0, 16 * sizeof(uint32_t));
	out[0] = (uint32_t)XNVME_OPC_CP_EXECUTE;
	out[1] = nsid;
	out[2] = ((uint32_t)mrs_id << 16) | prog_index; /* xnvme_cs.c:122 */
	out[3] = 0;				 /* xnvme_cs.c:123 */
	out[4] = (uint32_t)buf_len;		 /* xnvme_cs.c:124 */
}

typedef struct {
	const char *name;
	uint32_t nsid;
	uint16_t prog_index;
	uint16_t mrs_id;
	size_t buf_len;
	uint32_t golden[16];
} cp_execute_case;

static const cp_execute_case cp_execute_cases[] = {
	{
		.name = "cp_execute_buf: prog_index=1 mrs_id=0 buf_len=4096",
		.nsid = 1,
		.prog_index = 1,
		.mrs_id = 0,
		.buf_len = 4096,
		.golden = {
			/* CDW00 */ 0x01, /* CDW01 (nsid) */ 1,
			/* CDW02 = (mrs_id=0<<16)|prog_index=1 */ 0x00000001,
			/* CDW03 */ 0, /* CDW04 (buf_len) */ 0x1000,
			/* CDW05..09 */ 0, 0, 0, 0, 0,
			/* CDW10..15 */ 0, 0, 0, 0, 0, 0,
		},
	},
	{
		.name = "cp_execute_buf: prog_index=7 mrs_id=3 buf_len=1MiB",
		.nsid = 1,
		.prog_index = 7,
		.mrs_id = 3,
		.buf_len = 1048576,
		.golden = {
			/* CDW00 */ 0x01, /* CDW01 (nsid) */ 1,
			/* CDW02 = (mrs_id=3<<16)|prog_index=7 */ 0x00030007,
			/* CDW03 */ 0, /* CDW04 (buf_len) */ 0x100000,
			/* CDW05..09 */ 0, 0, 0, 0, 0,
			/* CDW10..15 */ 0, 0, 0, 0, 0, 0,
		},
	},
};

/* ───────────────────────── 4. CP_MEM_RANGE (create) ─────────────────────────
 * xnvme/lib/xnvme_cs.c:136 xnvme_cmd_cp_mem_range(), ADMIN queue, sel=0 (CREATE).
 * CDW10 = (mrs_id<<16)|sel. CDW11 = numr = nr (1-based NUMR, not nr-1).
 * CDW12..15 = 0.
 */

static void
build_cp_mem_range(uint32_t nsid, uint32_t mrs_id, uint32_t nr, uint32_t out[16])
{
	const uint32_t sel = 0; /* xnvme_cs.c:139, CREATE */
	uint32_t numr = nr;	 /* xnvme_cs.c:155, 1-based */

	memset(out, 0, 16 * sizeof(uint32_t));
	out[0] = (uint32_t)XNVME_OPC_CP_MEM_RANGE;
	out[1] = nsid;
	out[10] = (mrs_id << 16) | sel;
	out[11] = numr;
}

typedef struct {
	const char *name;
	uint32_t nsid;
	uint32_t mrs_id;
	uint32_t nr;
	uint32_t golden[16];
} cp_mem_range_case;

static const cp_mem_range_case cp_mem_range_cases[] = {
	{
		.name = "cp_mem_range: mrs_id=0 nr=1",
		.nsid = 1,
		.mrs_id = 0,
		.nr = 1,
		.golden = {
			/* CDW00 */ 0x89, /* CDW01 (nsid) */ 1,
			/* CDW02..09 */ 0, 0, 0, 0, 0, 0, 0, 0,
			/* CDW10 = (mrs_id=0<<16)|sel=0 */ 0x00000000,
			/* CDW11 (numr) */ 1,
			/* CDW12..15 */ 0, 0, 0, 0,
		},
	},
	{
		.name = "cp_mem_range: mrs_id=5 nr=64",
		.nsid = 1,
		.mrs_id = 5,
		.nr = 64,
		.golden = {
			/* CDW00 */ 0x89, /* CDW01 (nsid) */ 1,
			/* CDW02..09 */ 0, 0, 0, 0, 0, 0, 0, 0,
			/* CDW10 = (mrs_id=5<<16)|sel=0 */ 0x00050000,
			/* CDW11 (numr) */ 64,
			/* CDW12..15 */ 0, 0, 0, 0,
		},
	},
};

/* ───────────────────────── 5. CP_MEM_RANGE (delete) ─────────────────────────
 * xnvme/lib/xnvme_cs.c:171 xnvme_cmd_cp_mem_range_delete(), ADMIN queue,
 * same opcode as create (0x89), sel=1 (DELETE). NUMR left 0 -- the delete
 * path takes no range data. CDW11..15 = 0.
 */

static void
build_cp_mem_range_delete(uint32_t nsid, uint16_t mrs_id, uint32_t out[16])
{
	const uint32_t sel = 1; /* xnvme_cs.c:177, DELETE */

	memset(out, 0, 16 * sizeof(uint32_t));
	out[0] = (uint32_t)XNVME_OPC_CP_MEM_RANGE;
	out[1] = nsid;
	out[10] = ((uint32_t)mrs_id << 16) | sel;
}

typedef struct {
	const char *name;
	uint32_t nsid;
	uint16_t mrs_id;
	uint32_t golden[16];
} cp_mem_range_delete_case;

static const cp_mem_range_delete_case cp_mem_range_delete_cases[] = {
	{
		.name = "cp_mem_range_delete: mrs_id=0",
		.nsid = 1,
		.mrs_id = 0,
		.golden = {
			/* CDW00 */ 0x89, /* CDW01 (nsid) */ 1,
			/* CDW02..09 */ 0, 0, 0, 0, 0, 0, 0, 0,
			/* CDW10 = (mrs_id=0<<16)|sel=1 */ 0x00000001,
			/* CDW11..15 */ 0, 0, 0, 0, 0,
		},
	},
	{
		.name = "cp_mem_range_delete: mrs_id=5",
		.nsid = 1,
		.mrs_id = 5,
		.golden = {
			/* CDW00 */ 0x89, /* CDW01 (nsid) */ 1,
			/* CDW02..09 */ 0, 0, 0, 0, 0, 0, 0, 0,
			/* CDW10 = (mrs_id=5<<16)|sel=1 */ 0x00050001,
			/* CDW11..15 */ 0, 0, 0, 0, 0,
		},
	},
};

/* ───────────────────────── 7. SLM_COPY_LBA ─────────────────────────
 * xnvme/lib/xnvme_slm.c:104 xnvme_cmd_slm_copy_lba(), IO queue.
 * common.nsid = dst_nsid.
 * CDW02 = (u32)nbytes, CDW03 = (u32)(nbytes>>32)
 * CDW10 = (u32)dst_addr, CDW11 = (u32)(dst_addr>>32)
 * CDW12 = (desc_fmt<<8)|0 (NDESC-1 = 0, i.e. 1 descriptor). CDW13..15 = 0.
 *
 * Plus a 32-byte out-of-band descriptor
 * (struct xnvme_spec_slm_copy_desc_fmt23, libxnvme_slm.h:52-63, packed):
 *   off0  snsid  u32 LE = src_nsid
 *   off4  rsvd0  u32 = 0
 *   off8  slba   u64 LE
 *   off16 nlb    u16 LE | off18 rsvd1 u16 = 0   (combined dword)
 *   off20 rsvd2  u32 = 0
 *   off24 eilbrt u32 = 0
 *   off28 elbat  u16 = 0 | off30 elbatm u16 = 0 (combined dword)
 *
 * (xnvme_cmd_slm_copy / FMT4 and xnvme_cmd_slm_read / opcode 0x02 are
 * declared in the fork but never called by libcs -- not included here.)
 *
 * desc_fmt must be FMT2 or FMT3; both cases below pick src_nsid != dst_nsid
 * (and != 0) so xnvme_cmd_slm_copy_lba's NLB/geo cross-check (xnvme_slm.c:139)
 * is skipped -- that check needs a live ctx->dev we don't have here, and is
 * irrelevant to the dword-packing arithmetic this oracle verifies.
 */

static void
build_slm_copy_lba(uint32_t dst_nsid, uint64_t dst_addr, uint32_t src_nsid, uint64_t slba,
		    uint16_t nlb, uint64_t nbytes, uint32_t desc_fmt, uint32_t out_cmd[16],
		    uint32_t out_desc[8])
{
	memset(out_cmd, 0, 16 * sizeof(uint32_t));
	out_cmd[0] = (uint32_t)XNVME_OPC_SLM_MEM_COPY;
	out_cmd[1] = dst_nsid;
	out_cmd[2] = (uint32_t)nbytes;		   /* cdw02, xnvme_slm.c:164 */
	out_cmd[3] = (uint32_t)(nbytes >> 32);	   /* cdw03, xnvme_slm.c:165 */
	out_cmd[10] = (uint32_t)dst_addr;	   /* cdw10, xnvme_slm.c:166 */
	out_cmd[11] = (uint32_t)(dst_addr >> 32); /* cdw11, xnvme_slm.c:167 */
	out_cmd[12] = (desc_fmt << 8) | 0u;	   /* cdw12, xnvme_slm.c:170 */

	memset(out_desc, 0, 8 * sizeof(uint32_t));
	out_desc[0] = src_nsid;		     /* off0 snsid */
	out_desc[1] = 0;		     /* off4 rsvd0 */
	out_desc[2] = (uint32_t)slba;	     /* off8 slba low */
	out_desc[3] = (uint32_t)(slba >> 32); /* off12 slba high */
	out_desc[4] = (uint32_t)nlb;	     /* off16 nlb | off18 rsvd1<<16 */
	out_desc[5] = 0;		     /* off20 rsvd2 */
	out_desc[6] = 0;		     /* off24 eilbrt */
	out_desc[7] = 0;		     /* off28 elbat | off30 elbatm<<16 */
}

typedef struct {
	const char *name;
	uint32_t dst_nsid;
	uint64_t dst_addr;
	uint32_t src_nsid;
	uint64_t slba;
	uint16_t nlb;
	uint64_t nbytes;
	uint32_t desc_fmt;
	uint32_t golden_cmd[16];
	uint32_t golden_desc[8];
} slm_copy_lba_case;

static const slm_copy_lba_case slm_copy_lba_cases[] = {
	{
		.name = "slm_copy_lba: FMT2, low-32 addresses",
		.dst_nsid = 1,
		.dst_addr = 0x1000,
		.src_nsid = 2,
		.slba = 0x100,
		.nlb = 7,
		.nbytes = 4096,
		.desc_fmt = 0x2, /* XNVME_SLM_COPY_DESC_FMT2 */
		.golden_cmd = {
			/* CDW00 */ 0x01, /* CDW01 (dst_nsid) */ 1,
			/* CDW02 (nbytes lo) */ 0x1000, /* CDW03 (nbytes hi) */ 0,
			/* CDW04..09 */ 0, 0, 0, 0, 0, 0,
			/* CDW10 (dst_addr lo) */ 0x1000, /* CDW11 (dst_addr hi) */ 0,
			/* CDW12 = (fmt=2<<8)|0 */ 0x200,
			/* CDW13..15 */ 0, 0, 0,
		},
		.golden_desc = {
			/* snsid */ 2, /* rsvd0 */ 0,
			/* slba lo */ 0x100, /* slba hi */ 0,
			/* nlb|rsvd1 */ 7,
			/* rsvd2 */ 0, /* eilbrt */ 0, /* elbat|elbatm */ 0,
		},
	},
	{
		.name = "slm_copy_lba: FMT3, high-32 addresses + max nlb",
		.dst_nsid = 5,
		.dst_addr = 0x100000008ULL,
		.src_nsid = 9,
		.slba = 0x100000003ULL,
		.nlb = 0xFFFF,
		.nbytes = 0x200000000ULL,
		.desc_fmt = 0x3, /* XNVME_SLM_COPY_DESC_FMT3 */
		.golden_cmd = {
			/* CDW00 */ 0x01, /* CDW01 (dst_nsid) */ 5,
			/* CDW02 (nbytes lo) */ 0, /* CDW03 (nbytes hi) */ 2,
			/* CDW04..09 */ 0, 0, 0, 0, 0, 0,
			/* CDW10 (dst_addr lo) */ 8, /* CDW11 (dst_addr hi) */ 1,
			/* CDW12 = (fmt=3<<8)|0 */ 0x300,
			/* CDW13..15 */ 0, 0, 0,
		},
		.golden_desc = {
			/* snsid */ 9, /* rsvd0 */ 0,
			/* slba lo */ 3, /* slba hi */ 1,
			/* nlb|rsvd1 */ 0xFFFF,
			/* rsvd2 */ 0, /* eilbrt */ 0, /* elbat|elbatm */ 0,
		},
	},
};

/* ───────────────────────── test drivers ───────────────────────── */

#define RUN_SIMPLE_CASES(cases, build_call)                                                  \
	do {                                                                                   \
		for (size_t i = 0; i < sizeof(cases) / sizeof((cases)[0]); i++) {             \
			uint32_t formula[16];                                                 \
			char msg[160];                                                        \
			TEST_START((cases)[i].name);                                         \
			build_call;                                                           \
			if (!dwords_match(formula, (cases)[i].golden, 16, msg, sizeof(msg),  \
					   "cmd")) {                                           \
				TEST_FAIL(msg);                                               \
				continue;                                                     \
			}                                                                     \
			TEST_PASS();                                                          \
		}                                                                              \
	} while (0)

static void
test_cp_load(void)
{
	RUN_SIMPLE_CASES(cp_load_cases,
			  build_cp_load(cp_load_cases[i].nsid, cp_load_cases[i].prog_index,
					 cp_load_cases[i].buf_len, formula));
}

static void
test_cp_activate(void)
{
	RUN_SIMPLE_CASES(cp_activate_cases,
			  build_cp_activate(cp_activate_cases[i].nsid,
					     cp_activate_cases[i].prog_index,
					     cp_activate_cases[i].action, formula));
}

static void
test_cp_execute_buf(void)
{
	RUN_SIMPLE_CASES(cp_execute_cases,
			  build_cp_execute_buf(cp_execute_cases[i].nsid,
						cp_execute_cases[i].prog_index,
						cp_execute_cases[i].mrs_id,
						cp_execute_cases[i].buf_len, formula));
}

static void
test_cp_mem_range(void)
{
	RUN_SIMPLE_CASES(cp_mem_range_cases,
			  build_cp_mem_range(cp_mem_range_cases[i].nsid,
					      cp_mem_range_cases[i].mrs_id,
					      cp_mem_range_cases[i].nr, formula));
}

static void
test_cp_mem_range_delete(void)
{
	RUN_SIMPLE_CASES(cp_mem_range_delete_cases,
			  build_cp_mem_range_delete(cp_mem_range_delete_cases[i].nsid,
						     cp_mem_range_delete_cases[i].mrs_id, formula));
}

static void
test_slm_copy_lba(void)
{
	for (size_t i = 0; i < sizeof(slm_copy_lba_cases) / sizeof(slm_copy_lba_cases[0]); i++) {
		const slm_copy_lba_case *c = &slm_copy_lba_cases[i];
		uint32_t formula_cmd[16];
		uint32_t formula_desc[8];
		char msg[160];

		TEST_START(c->name);
		build_slm_copy_lba(c->dst_nsid, c->dst_addr, c->src_nsid, c->slba, c->nlb,
				   c->nbytes, c->desc_fmt, formula_cmd, formula_desc);

		if (!dwords_match(formula_cmd, c->golden_cmd, 16, msg, sizeof(msg), "cmd")) {
			TEST_FAIL(msg);
			continue;
		}
		if (!dwords_match(formula_desc, c->golden_desc, 8, msg, sizeof(msg), "desc")) {
			TEST_FAIL(msg);
			continue;
		}
		TEST_PASS();
	}
}

int
main(void)
{
	printf("=== CP/SLM command-builder differential oracle ===\n");
	printf("(commit 1/10 of the xNVMe-fork -> direct-SPDK port; see file header)\n\n");

	test_opcode_enum_values();
	test_cp_load();
	test_cp_activate();
	test_cp_execute_buf();
	test_cp_mem_range();
	test_cp_mem_range_delete();
	test_slm_copy_lba();

	TEST_SUMMARY();
	return TEST_EXIT();
}
