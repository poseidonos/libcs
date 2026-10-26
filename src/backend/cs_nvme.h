/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_nvme.h
 * @brief libcs-native NVMe shim: types + function declarations that mirror
 * the subset of the xNVMe library API actually used by backend_xnvme.c,
 * backend_xnvme_compute.c, backend_xnvme_slm.c, backend_xnvme_misc.c and
 * backend_xnvme_internal.h.
 *
 * This is commit 2 of the 10-commit sequence (see
 * ~/.claude/plans/hidden-sprouting-turtle.md) porting libcs off the xNVMe
 * fork onto direct upstream SPDK. Context for why this file exists and how
 * it will be used:
 *
 *   - Commit 3 (cs_nvme_spdk.c) implements the transport-shaped functions
 *     declared here (dev open/close, queue init/poke, buf alloc, cmd_pass,
 *     nvm read/write, ...) against upstream `spdk_nvme_*`.
 *   - Commit 5 ports backend_xnvme*.c onto this shim by swapping their
 *     `#include <libxnvme*.h>` lines for `#include "cs_nvme.h"`. Function
 *     NAMES and signatures here are therefore kept IDENTICAL to their xNVMe
 *     originals (same args, same order, same return type) so call sites in
 *     those ~3300 lines need zero changes. Only the later, purely mechanical
 *     commit 9 renames `xnvme_*` -> `cs_nvme_*` for the functions.
 *   - TYPE names, by contrast, use the `cs_nvme_*` prefix from the start
 *     (struct cs_nvme_dev, cs_nvme_cmd, cs_nvme_cpl, cs_nvme_opts,
 *     cs_nvme_cmd_ctx, ...) per the orchestrator's explicit direction for
 *     this commit. backend_xnvme_internal.h's own type usage (e.g.
 *     `struct xnvme_dev *dev;`) gets its pointer types swapped to match in
 *     commit 5 alongside the include-line swap; this does not create any
 *     call-site churn in the four big backend .c files, which never spell
 *     out `struct xnvme_dev` / `struct xnvme_opts` etc. as a type name
 *     themselves except through variables whose *declared* type lives in
 *     backend_xnvme_internal.h or as local var decls that commit 5 reviews
 *     by hand (see the plan's "raw-cdw hotspot" callouts).
 *
 * This header is internal-only: not installed, not part of the public SNIA
 * CS API surface. It is intentionally SPDK-free (opaque device/queue
 * handles only) even though nothing strictly requires that here -- keeping
 * SPDK/DPDK headers out of every translation unit that doesn't need them is
 * free compile-time-surface reduction, and it means this header, unlike
 * cs_nvme_spdk.c, can be included and compiled without SPDK installed at
 * all (exercised by this commit's standalone golden-vector test).
 *
 * NOT wired into the library build yet: lib_sources in meson.build is
 * unchanged by this commit. Only a new standalone test
 * (tests/test_cmd_oracle.c) compiles cs_nvme_cmds.c, which is the one
 * translation unit in this commit that implements against this header.
 */

#ifndef CS_NVME_H
#define CS_NVME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────── opaque handles ─────────────────────────
 *
 * Real definitions live in cs_nvme_spdk.c (commit 3), wrapping
 * `struct spdk_nvme_ctrlr`/`struct spdk_nvme_ns` (dev) and
 * `struct spdk_nvme_qpair` (queue) respectively, plus the locked
 * controller-ref-cache bookkeeping described in the design plan. Nothing
 * in this header or in cs_nvme_cmds.c dereferences these -- they are only
 * ever passed through by pointer.
 */
struct cs_nvme_dev;
struct cs_nvme_queue;

/* ───────────────────────── CP/SLM constants ─────────────────────────
 *
 * libcs-native CSI / opcode / descriptor-format constants, self-defined
 * (not pulled from any fork-only SPDK header) per the design plan's rule
 * that libcs must not depend on nvme_cpcs_spec.h / bdev_slm.h or the fork's
 * additions to nvme_spec.h. Values transcribed from
 * xnvme/include/libxnvme_cs.h and libxnvme_slm.h as of commit 1's oracle
 * (tests/test_cmd_oracle.c), which cross-checks these same literals via
 * _Static_assert against the live xNVMe enums while the xnvme submodule is
 * still present. cs_internal.h already defines the OPC_* subset needed by
 * the ioctl/io_uring/libnvme passthrough backends (OPC_CP_LOAD etc.) --
 * those are reused as-is by cs_nvme_cmds.c instead of being redefined here;
 * this header only adds the CSI and descriptor-format constants that
 * cs_internal.h does not have.
 */
#define CS_NVME_CSI_SLM 0x03u /**< Subsystem Local Memory Command Set */
#define CS_NVME_CSI_CP  0x04u /**< Computational Programs Command Set */

enum cs_nvme_slm_copy_desc_fmt {
	CS_NVME_SLM_COPY_DESC_FMT2 = 0x2, /**< Descriptor format 2 (LBA source) */
	CS_NVME_SLM_COPY_DESC_FMT3 = 0x3, /**< Descriptor format 3 (LBA source) */
	CS_NVME_SLM_COPY_DESC_FMT4 = 0x4, /**< Descriptor format 4 (includes source NSID) */
};

/* ───────────────────────── on-wire command (64B / 16 dwords) ─────────────────────────
 *
 * Matches struct spdk_nvme_cmd layout byte-for-byte (verified later via
 * _Static_assert in cs_nvme_spdk.c, commit 3). Raw dword access
 * (cmd->cdw[n]) is valid for n in 0..15, matching the two raw
 * `(uint32_t *)&cmd_ctx->cmd` casts in backend_xnvme_slm.c (SLM
 * MEMORY_READ/MEMORY_WRITE, opcodes 0x02/0x05) where dword index ==
 * CDW number.
 *
 * Two additional named union arms are provided, both views over the exact
 * same 64 bytes, matching the two access conventions actually used by the
 * backend_xnvme*.c call sites (grepped, not guessed):
 *   - `.common.*`  mirrors xnvme_spec_cmd_common: opcode/flags/cid (dword0),
 *     nsid (dword1), cdw02..cdw15 named individually. This is the view used
 *     by essentially every call site (`cmd.cmd.common.opcode = ...`,
 *     `cmd.cmd.common.cdw10 = ...`, etc.)
 *   - `.cp.*`      mirrors xnvme_spec_cp_cmd: cdw00_09[10] (index == CDW
 *     number, i.e. cdw00_09[2] is CDW02) followed by cdw10..cdw15. This is
 *     the convention documented in xnvme_cs.c's cp_execute_buf builder and
 *     used internally by cs_nvme_cmds.c's own _prep helpers below.
 */
struct cs_nvme_cmd {
	union {
		uint32_t cdw[16]; /* raw dword access, index == CDW number */

		struct {
			uint8_t opcode;
			uint8_t flags; /* fuse (bits0-1) | reserved | psdt (bits6-7) */
			uint16_t cid;
			uint32_t nsid;
			uint32_t cdw02;
			uint32_t cdw03;
			uint32_t cdw04;
			uint32_t cdw05;
			uint32_t cdw06;
			uint32_t cdw07;
			uint32_t cdw08;
			uint32_t cdw09;
			uint32_t cdw10;
			uint32_t cdw11;
			uint32_t cdw12;
			uint32_t cdw13;
			uint32_t cdw14;
			uint32_t cdw15;
		} common;

		struct {
			uint32_t cdw00_09[10]; /* dwords 0-9, index == CDW number */
			uint32_t cdw10;
			uint32_t cdw11;
			uint32_t cdw12;
			uint32_t cdw13;
			uint32_t cdw14;
			uint32_t cdw15;
		} cp;
	};
};

/* ───────────────────────── completion (16 bytes) ─────────────────────────
 *
 * Matches xnvme_spec_cpl's bitfield layout exactly (libxnvme_spec.h, struct
 * xnvme_spec_status): p(1)/sc(8)/sct(3)/rsvd2(2)/m(1)/dnr(1) = 16 bits.
 * `result`/`cdw0` are read at ~20 call sites across the backend
 * (cpl.cdw0 for 32-bit results, cpl.result for 64-bit combined cdw0/cdw1
 * results e.g. MULTI_AGG64 sums, RLE compressed sizes) and `status.sc` /
 * `status.sct` at ~23 more for error mapping -- both forms must produce
 * identical values post-port, which this layout guarantees since it's the
 * same union/bitfield shape xNVMe uses.
 */
struct cs_nvme_cpl {
	union {
		struct {
			uint32_t cdw0; /* command-specific */
			uint32_t cdw1; /* xNVMe calls this `rsvd1`; combined with cdw0 it
					* carries 64-bit results (e.g. RLE compressed size,
					* MULTI_AGG64 sum) via the `result` alias below. */
		};
		uint64_t result; /* combined result of cdw0 and cdw1 */
	};

	uint16_t sqhd; /* submission queue head pointer */
	uint16_t sqid; /* submission queue identifier */
	uint16_t cid;  /* command identifier */

	struct {
		uint16_t p     : 1; /* phase tag */
		uint16_t sc    : 8; /* status code */
		uint16_t sct   : 3; /* status code type */
		uint16_t rsvd2 : 2;
		uint16_t m     : 1; /* more */
		uint16_t dnr   : 1; /* do not retry */
	} status;
};

/* ───────────────────────── device-open options ─────────────────────────
 *
 * Mirrors every field of struct xnvme_opts (libxnvme_opts.h) that
 * backend_xnvme.c / backend_xnvme_internal.h either read or copy wholesale
 * (`ctx->opts = xnvme_opts_default()`, `struct xnvme_opts slm_opts =
 * ctx->opts`). Field NAMES are unchanged from xNVMe's so a future
 * `struct xnvme_opts` -> `struct cs_nvme_opts` type-name swap in
 * backend_xnvme_internal.h (commit 5) is the only touch point; the field
 * accessors (`.be`, `.async`, `.subnqn`, ...) throughout the four backend
 * .c files stay byte-for-byte identical.
 */
struct cs_nvme_opts_css {
	uint32_t value;
	uint32_t given;
};

struct cs_nvme_opts {
	const char *be;    /* backend/system interface to use */
	const char *dev;   /* device manager/enumerator */
	const char *mem;   /* memory allocator to use for buffers */
	const char *sync;  /* synchronous command-interface */
	const char *async; /* asynchronous command-interface */
	const char *admin; /* administrative command-interface */

	uint32_t nsid; /* namespace identifier */

	uint8_t rdonly;   /* OS open: for read only */
	uint8_t wronly;   /* OS open: for write only */
	uint8_t rdwr;     /* OS open: for read and write */
	uint8_t create;   /* OS open: create if it does not exist */
	uint8_t truncate; /* OS open: truncate content */
	uint8_t direct;   /* OS open: bypass OS-managed caching */

	uint32_t create_mode; /* OS file creation-mode */

	uint8_t poll_io;          /* io_uring: enable io-polling */
	uint8_t poll_sq;          /* io_uring: enable sqthread-polling */
	uint8_t register_files;   /* io_uring: enable file-registrations */
	uint8_t register_buffers; /* io_uring: enable buffer-registration */

	struct cs_nvme_opts_css css; /* SPDK controller-setup: command-set-selection */
	uint32_t use_cmb_sqs;        /* SPDK controller-setup: use controller-memory-buffer for sq */

	uint32_t shm_id;        /* SPDK multi-processing: shared-memory-id */
	uint32_t main_core;     /* SPDK multi-processing: main-core */
	const char *core_mask;  /* SPDK multi-processing: core-mask */

	const char *adrfam;  /* SPDK fabrics: address-family, IPv4/IPv6 */
	const char *subnqn;  /* SPDK fabrics: subsystem NQN */
	const char *hostnqn; /* SPDK fabrics: host NQN */

	uint32_t admin_timeout;         /* SPDK fabrics: admin command timeout */
	uint32_t command_timeout;       /* SPDK fabrics: io command timeout */
	uint32_t spdk_fabrics;          /* assigned by backend if SPDK uses fabrics */
	uint32_t keep_alive_timeout_ms; /* SPDK fabrics: keep alive timeout */
};

/**
 * Returns an initialized option-struct with default values.
 *
 * Implemented in cs_nvme_spdk.c (commit 3).
 */
struct cs_nvme_opts
xnvme_opts_default(void);

/* ───────────────────────── command context ───────────────────────── */

struct cs_nvme_cmd_ctx;

/**
 * Signature of function used with command queues for async callback upon
 * command-completion. Mirrors xnvme_queue_cb (libxnvme_queue.h).
 */
typedef void (*cs_nvme_queue_cb)(struct cs_nvme_cmd_ctx *ctx, void *opaque);

struct cs_nvme_cmd_ctx {
	struct cs_nvme_cmd cmd; /* command to be processed */
	struct cs_nvme_cpl cpl; /* completion result from processing */
	struct cs_nvme_dev *dev; /* device associated with the command */

	struct {
		struct cs_nvme_queue *queue; /* queue used for command processing */
		cs_nvme_queue_cb cb;         /* user defined callback function */
		void *cb_arg;                /* user defined callback function arguments */
	} async;

	uint32_t opts; /* command-option flags (e.g. async), set by helpers */
};

/**
 * Retrieve a command-context for issuing synchronous commands to the given
 * device. Implemented in cs_nvme_spdk.c (commit 3).
 */
struct cs_nvme_cmd_ctx
xnvme_cmd_ctx_from_dev(struct cs_nvme_dev *dev);

/**
 * Assign a callback-function and argument to be used with the given
 * command-context. Trivial enough to implement directly here, matching
 * xNVMe's own `static inline` (libxnvme_adm.h) -- no transport dependency,
 * so no need to defer to cs_nvme_spdk.c.
 */
static inline void
xnvme_cmd_ctx_set_cb(struct cs_nvme_cmd_ctx *ctx, cs_nvme_queue_cb cb, void *cb_arg)
{
	ctx->async.cb = cb;
	ctx->async.cb_arg = cb_arg;
}

/**
 * Encapsulates completion-error checking. Matches xNVMe's own `static
 * inline` predicate (libxnvme_adm.h) exactly -- `cpl.status.sc ||
 * cpl.status.sct` -- so the ~23 call sites across the backend that rely on
 * this check for error mapping see byte-identical behavior post-port.
 */
static inline int
xnvme_cmd_ctx_cpl_status(struct cs_nvme_cmd_ctx *ctx)
{
	return ctx->cpl.status.sc || ctx->cpl.status.sct;
}

/**
 * Pass an NVMe IO command through to the device via the given cmd_ctx.
 * Implemented in cs_nvme_spdk.c (commit 3): sync submit via
 * spdk_nvme_ctrlr_cmd_io_raw_with_md + spdk_nvme_qpair_process_completions
 * spin, per the design plan.
 */
int
xnvme_cmd_pass(struct cs_nvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *mbuf,
	       size_t mbuf_nbytes);

/**
 * Pass an NVMe Admin command through to the device via the given cmd_ctx.
 * Implemented in cs_nvme_spdk.c (commit 3): spdk_nvme_ctrlr_cmd_admin_raw +
 * spdk_nvme_ctrlr_process_admin_completions, with the int32_t negative-return
 * bail-out that xnvme_be_spdk_admin.c omits (design plan risk #2).
 */
int
xnvme_cmd_pass_admin(struct cs_nvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *mbuf,
		     size_t mbuf_nbytes);

/* ───────────────────────── device lifecycle / identify ───────────────────────── */

/**
 * Representation of device identifiers once decoded from text
 * representation. Mirrors the fields of struct xnvme_ident (libxnvme_ident.h)
 * actually read by the backend (`ident->uri`).
 */
struct cs_nvme_ident {
	char subnqn[256];
	char uri[384];
	uint32_t dtype;
	uint32_t nsid;
	uint8_t csi;
};

/**
 * Device "geometry". Mirrors struct xnvme_geo (libxnvme_geo.h) fields read
 * by the backend (`geo->lba_nbytes`, `geo->mdts_nbytes`).
 */
struct cs_nvme_geo {
	uint32_t type;

	uint32_t npugrp;
	uint32_t npunit;

	uint32_t nzone;
	uint64_t nsect;
	uint32_t nbytes;
	uint32_t nbytes_oob;

	uint64_t tbytes;
	uint64_t ssw;

	uint32_t mdts_nbytes; /* maximum-data-transfer-size, in bytes */

	uint32_t lba_nbytes; /* size of an LBA in bytes */
	uint8_t lba_extended;

	uint8_t pi_type;
	uint8_t pi_loc;
	uint8_t pi_format;
};

/**
 * NVMe Identify Controller data, trimmed to the fields the backend actually
 * reads (`ctrlr->vid`, `ctrlr->mn`) from struct xnvme_spec_idfy_ctrlr
 * (libxnvme_spec.h). The real 4096-byte Identify Controller structure has
 * many more fields; cs_nvme_spdk.c (commit 3) is free to extend this struct
 * as more fields become necessary, same as the fallback-preserving
 * identify-on-open design in the plan.
 */
struct cs_nvme_spec_idfy_ctrlr {
	uint16_t vid;   /* PCI Vendor ID */
	uint16_t ssvid; /* PCI Subsystem Vendor ID */
	int8_t sn[20];  /* Serial Number */
	int8_t mn[40];  /* Model Number */
	uint8_t fr[8];  /* Firmware Revision */
};

/**
 * NVMe Identify Namespace data, command-set-specific view (CNS=0x05 with
 * CSI=CS_NVME_CSI_CP). Opaque to the generic (non-CP) identify path --
 * xnvme_dev_get_ns_css() returns this as `const struct
 * cs_nvme_spec_idfy_ns *`, which the backend then reinterprets as `const
 * struct cs_nvme_spec_cp_idfy_ns *` when the CP command set is active
 * (mirrors backend_xnvme.c:357-358's xnvme_spec_cp_idfy_ns cast).
 */
struct cs_nvme_spec_idfy_ns {
	uint8_t rsvd[4096];
};

/**
 * Identify Computational Namespace Data Structure (simplified), matching
 * struct xnvme_spec_cp_idfy_ns (libxnvme_cs.h) field-for-field so the
 * backend's `(const struct xnvme_spec_cp_idfy_ns *)ns_css` reinterpret-cast
 * pattern keeps reading the same offsets (maxact at offset 0, maxpb at
 * offset 8) after the port.
 */
struct __attribute__((packed)) cs_nvme_spec_cp_idfy_ns {
	uint16_t maxact;   /* maximum activated programs */
	uint16_t maxmemrs; /* maximum memory range sets */
	uint16_t mrsg;     /* memory range size granularity */
	uint8_t maxmemr;   /* maximum number of ranges in a memory range set */
	uint8_t rsvd7;
	uint64_t maxpb; /* maximum bytes for all downloaded programs (MiB) */
	uint8_t lpg;    /* load program granularity */
	uint8_t rsvd17[4096 - 17];
};

/**
 * Creates a device handle based on the given device-uri. Implemented in
 * cs_nvme_spdk.c (commit 3): URI -> trid classification, locked
 * controller-ref cache, spdk_nvme_probe, Admin Identify for ctrlr/CP
 * namespace caching. See the design plan's "전송 구현 결정 사항" section.
 */
struct cs_nvme_dev *
xnvme_dev_open(const char *dev_uri, struct cs_nvme_opts *opts);

/**
 * Destroy the given device handle. Implemented in cs_nvme_spdk.c (commit 3):
 * refcounted controller-cache release, spdk_nvme_detach at refcount 0.
 */
void
xnvme_dev_close(struct cs_nvme_dev *dev);

/**
 * Returns the NVMe namespace identifier associated with the given dev.
 */
uint32_t
xnvme_dev_get_nsid(const struct cs_nvme_dev *dev);

/**
 * Returns the geometry of the given device.
 */
const struct cs_nvme_geo *
xnvme_dev_get_geo(const struct cs_nvme_dev *dev);

/**
 * Returns the representation of the device identifier, decoded from the
 * open URI.
 */
const struct cs_nvme_ident *
xnvme_dev_get_ident(const struct cs_nvme_dev *dev);

/**
 * Returns the NVMe identify controller structure associated with the given
 * device.
 */
const struct cs_nvme_spec_idfy_ctrlr *
xnvme_dev_get_ctrlr(const struct cs_nvme_dev *dev);

/**
 * Returns the NVMe identify namespace structure specific to the command set
 * and namespace associated with the given device (CNS=0x05, CSI from the
 * device's own command-set).
 */
const struct cs_nvme_spec_idfy_ns *
xnvme_dev_get_ns_css(const struct cs_nvme_dev *dev);

/* ───────────────────────── queues (async) ───────────────────────── */

/**
 * Queue initialization options. Mirrors enum xnvme_queue_opts
 * (libxnvme_queue.h); not currently passed a non-zero value by the backend,
 * kept for signature parity.
 */
enum cs_nvme_queue_opts {
	CS_NVME_QUEUE_IOPOLL = 0x1,
	CS_NVME_QUEUE_SQPOLL = 0x1 << 1,
};

/**
 * Allocate a command queue for asynchronous command submission and
 * completion. Implemented in cs_nvme_spdk.c (commit 3): maps to
 * spdk_nvme_ctrlr_alloc_io_qpair + a preallocated cs_nvme_cmd_ctx freelist
 * sized to `capacity`, per the design plan's async section.
 */
int
xnvme_queue_init(struct cs_nvme_dev *dev, uint16_t capacity, int opts,
		  struct cs_nvme_queue **queue);

/**
 * Tear down the given queue.
 */
int
xnvme_queue_term(struct cs_nvme_queue *queue);

/**
 * Process completions of commands on the given queue. Set `max` to limit
 * the number of completions; 0 means no max.
 */
int
xnvme_queue_poke(struct cs_nvme_queue *queue, uint32_t max);

/**
 * Process outstanding commands on the given queue until it is empty.
 */
int
xnvme_queue_drain(struct cs_nvme_queue *queue);

/**
 * Get the number of outstanding commands on the given queue.
 */
uint32_t
xnvme_queue_get_outstanding(struct cs_nvme_queue *queue);

/**
 * Retrieve a command-context from the given queue for async command
 * execution. Not thread-safe (matches xNVMe's own documented contract).
 */
struct cs_nvme_cmd_ctx *
xnvme_queue_get_cmd_ctx(struct cs_nvme_queue *queue);

/**
 * Hand back a command-context previously retrieved via
 * xnvme_queue_get_cmd_ctx(). Not thread-safe.
 */
int
xnvme_queue_put_cmd_ctx(struct cs_nvme_queue *queue, struct cs_nvme_cmd_ctx *ctx);

/* ───────────────────────── buffers ───────────────────────── */

/**
 * Allocate a buffer for IO with the given device, DMA-capable if required
 * by the backend. Implemented in cs_nvme_spdk.c (commit 3) via
 * spdk_dma_malloc.
 */
void *
xnvme_buf_alloc(const struct cs_nvme_dev *dev, size_t nbytes);

/**
 * Free a buffer allocated with xnvme_buf_alloc().
 */
void
xnvme_buf_free(const struct cs_nvme_dev *dev, void *buf);

/**
 * Write zeroes to the first `nbytes` of `buf`. Returns the first argument,
 * matching xNVMe's own signature (libxnvme_buf.h) exactly -- no transport
 * dependency, implemented directly in cs_nvme_spdk.c or inline later; kept
 * as a declaration here since it is also reasonable to implement in
 * cs_nvme_spdk.c alongside the other buffer management.
 */
void *
xnvme_buf_clear(void *buf, size_t nbytes);

/**
 * Retrieve the physical address of the given buffer. Implemented in
 * cs_nvme_spdk.c (commit 3) via spdk_vtophys, with the SPDK_VTOPHYS_ERROR
 * check the design plan calls for.
 */
int
xnvme_buf_vtophys(const struct cs_nvme_dev *dev, void *buf, uint64_t *phys);

/* ───────────────────────── NVM read/write (SLM staging path) ───────────────────────── */

/**
 * Submit, and optionally wait for completion of, an NVMe Read.
 */
int
xnvme_nvm_read(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint64_t slba, uint16_t nlb,
	       void *dbuf, void *mbuf);

/**
 * Submit, and optionally wait for completion of, an NVMe Write.
 */
int
xnvme_nvm_write(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint64_t slba, uint16_t nlb,
		const void *dbuf, const void *mbuf);

/* ───────────────────────── CP/SLM command builders ─────────────────────────
 *
 * The 7 functions below are the ones this commit (commit 2) actually
 * implements, in cs_nvme_cmds.c. Declared here with the same names/
 * signatures as their xNVMe originals (xnvme/include/libxnvme_cs.h,
 * libxnvme_slm.h) so commit 5's backend port is a pure #include swap for
 * these call sites too.
 */

/**
 * Validate a CP command data buffer: non-NULL, dword-aligned, at least one
 * dword, and within the maximum size allowed by the CPCS wire protocol.
 *
 * @return 0 on success, -EINVAL on validation failure.
 */
int
xnvme_cmd_cp_validate_dbuf(const void *buffer, size_t buf_len);

/**
 * Submit a Computational Program Load command (opcode 0x85, ADMIN).
 */
int
xnvme_cmd_cp_load(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint16_t prog_index,
		  const void *buffer, size_t buf_len);

/**
 * Submit a Computational Program Activation command (opcode 0x88, ADMIN).
 */
int
xnvme_cmd_cp_activate(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint16_t prog_index,
		      uint8_t action);

/**
 * Submit a Computational Program Execute command with a payload buffer
 * (opcode 0x01, IO queue).
 */
int
xnvme_cmd_cp_execute_buf(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint16_t prog_index,
			 uint16_t mrs_id, const void *buffer, size_t buf_len);

/**
 * Submit a Computational Program Memory Range Set create command (opcode
 * 0x89, ADMIN, sel=0).
 */
int
xnvme_cmd_cp_mem_range(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint32_t mrs_id, uint32_t nr,
		       const void *buffer, size_t buf_len);

/**
 * Submit a Computational Program Memory Range Set delete command (opcode
 * 0x89, ADMIN, sel=1), releasing a previously-created MRS back to the
 * device's MRS pool.
 */
int
xnvme_cmd_cp_mem_range_delete(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint16_t mrs_id);

/**
 * SLM Memory Copy Descriptor (Format 2/3), matching struct
 * xnvme_spec_slm_copy_desc_fmt23 (libxnvme_slm.h) field-for-field and byte
 * layout: 32 bytes, packed, little-endian on the wire (the LE packing is
 * done explicitly in cs_nvme_cmds.c, not relied upon from host byte order).
 */
struct __attribute__((packed)) cs_nvme_spec_slm_copy_desc_fmt23 {
	uint32_t snsid;
	uint32_t rsvd0;
	uint64_t slba;
	uint16_t nlb;
	uint16_t rsvd1;
	uint32_t rsvd2;
	uint32_t eilbrt;
	uint16_t elbat;
	uint16_t elbatm;
};

/**
 * Submit an SLM Memory Copy using descriptor format 2/3 (LBA source);
 * opcode 0x01 on the SLM command set, IO queue. Sync-only (the 32-byte
 * out-of-band descriptor is freed right after xnvme_cmd_pass() returns, so
 * there is no async variant).
 */
int
xnvme_cmd_slm_copy_lba(struct cs_nvme_cmd_ctx *ctx, uint32_t dst_nsid, uint64_t dst_addr,
		       uint32_t src_nsid, uint64_t slba, uint16_t nlb, uint64_t nbytes,
		       enum cs_nvme_slm_copy_desc_fmt desc_fmt);

#ifdef __cplusplus
}
#endif

#endif /* CS_NVME_H */
