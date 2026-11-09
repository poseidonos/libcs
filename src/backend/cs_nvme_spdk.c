/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 * SPDX-FileCopyrightText: xNVMe Project contributors (SPDK backend reference)
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_nvme_spdk.c
 * @brief SPDK-backed transport implementation of cs_nvme.h.
 *
 * Commit 3 of the 10-commit xNVMe-fork -> direct-SPDK port (see
 * ~/.claude/plans/hidden-sprouting-turtle.md, section "5. 전송 구현 결정
 * 사항"). Implements every cs_nvme.h function that cs_nvme_cmds.c (commit 2)
 * calls but does not implement itself: device open/close + identify, sync
 * admin/IO submit, async queue lifecycle, DMA buffers, NVM read/write.
 *
 * Ground-truth reference: xnvme/lib/xnvme_be_spdk_{dev,tcp,sync,admin,async,
 * mem}.c (upstream xNVMe, BSD-3-Clause, https://github.com/xnvme/xnvme — this
 * file is a re-implementation against upstream SPDK, not a copy; API surface
 * was mirrored for call-site compatibility, with the internal design changed
 * as noted below). Three known bugs in that reference are deliberately NOT
 * reproduced here -- see the comments at ensure_spdk_env_init(),
 * xnvme_cmd_pass_admin(), and the cref_* cache below.
 *
 * NOT YET wired into the library: this file is compiled only by the
 * standalone compile-check meson target added alongside it in this commit,
 * and (once cs_nvme_cmds.c's test stub is retired) by test_cmd_oracle_exe.
 * lib_sources in meson.build is untouched -- that is commit 5/6's job.
 *
 * Platform note: this file targets upstream SPDK (Linux/FreeBSD only). It
 * was written and manually cross-checked against
 * ../../../spdk/include/spdk/{nvme.h,nvme_spec.h,env.h} on this development
 * machine (macOS/Darwin), which cannot build or link SPDK at all -- see the
 * commit message for the verification status of this file.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spdk/env.h>
#include <spdk/nvme.h>

#include "cs_internal.h" /* CS_LOG_ERROR/WARN/INFO/DEBUG, OPC_IDENTIFY */
#include "cs_nvme.h"

/* ───────────────────────── layout pins ─────────────────────────
 *
 * cs_nvme_cmd / cs_nvme_cpl are defined independently in cs_nvme.h (no SPDK
 * include there, per the design plan's rule), but every submit path in this
 * file reinterpret-casts them to struct spdk_nvme_cmd / struct
 * spdk_nvme_cpl. Pin the layout equivalence here, the one TU that includes
 * both headers, so any future change to either side is caught at compile
 * time rather than corrupting dwords 2-15 / cdw0-1 / status silently.
 */
_Static_assert(sizeof(struct cs_nvme_cmd) == 64, "cs_nvme_cmd must be 64 bytes (16 dwords)");
_Static_assert(sizeof(struct cs_nvme_cmd) == sizeof(struct spdk_nvme_cmd),
	       "cs_nvme_cmd must be layout-identical to struct spdk_nvme_cmd");
_Static_assert(sizeof(struct cs_nvme_cpl) == 16, "cs_nvme_cpl must be 16 bytes");
_Static_assert(sizeof(struct cs_nvme_cpl) == sizeof(struct spdk_nvme_cpl),
	       "cs_nvme_cpl must be layout-identical to struct spdk_nvme_cpl");
_Static_assert(offsetof(struct cs_nvme_cpl, status) == 14,
	       "cs_nvme_cpl.status must sit at byte offset 14 (upper half of dword 3)");
_Static_assert(offsetof(struct cs_nvme_cpl, status) == offsetof(struct spdk_nvme_cpl, status),
	       "cs_nvme_cpl.status offset must match struct spdk_nvme_cpl.status");

/* Inline-Execute chunking (backend_xnvme_compute.c) ships a fixed 32 MiB
 * buffer per CP_EXECUTE call. Checked against the controller's MDTS at
 * dev_open() time -- see the comment at the end of do_identify_and_mdts().
 */
#define CS_NVME_INLINE_EXECUTE_CHUNK_BYTES (32u * 1024u * 1024u)

/* ───────────────────────── one-time SPDK env init ─────────────────────────
 *
 * Design calls for a pthread_once-guarded single spdk_env_init() call. A
 * literal pthread_once_t can't be handed the per-call opts it needs (shm_id/
 * core_mask/main_core come from whichever cs_nvme_opts the first dev_open()
 * call used), so this uses a mutex-guarded "attempted" flag instead -- same
 * guarantee (spdk_env_init() runs at most once, process-wide, ever), just
 * parameterizable. This directly replaces TWO things in the fork: (a)
 * xnvme_be_spdk_dev.c's flag-guarded _spdk_env_init(), and (b)
 * xnvme_be_spdk_tcp.c:219-229's completely separate, UNGUARDED second
 * spdk_env_init() call on the fabrics path -- a real double-init bug if both
 * a PCIe and a TCP device are opened in the same process. There is exactly
 * one init path here, used by both.
 */
static pthread_mutex_t g_env_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_env_attempted = false;
static int g_env_init_rc = 0;

static int
ensure_spdk_env_init(const struct cs_nvme_opts *opts)
{
	struct spdk_env_opts env_opts;

	/* FIX (hardware-verified bug, see commit message): spdk_env_opts_init()
	 * reads opts->opts_size BEFORE it memsets the struct (it treats
	 * opts_size as a versioning field the caller must already have set),
	 * so an uninitialized opts_size here is live stack garbage, not a
	 * harmless default. Must be set before the call.
	 */
	env_opts.opts_size = sizeof(env_opts);

	pthread_mutex_lock(&g_env_lock);
	if (g_env_attempted) {
		pthread_mutex_unlock(&g_env_lock);
		return g_env_init_rc;
	}

	spdk_env_opts_init(&env_opts);
	env_opts.name = "libcs";
	if (opts && opts->shm_id) {
		env_opts.shm_id = (int)opts->shm_id;
	}
	if (opts && opts->core_mask) {
		env_opts.core_mask = opts->core_mask;
		env_opts.main_core = (int)opts->main_core;
	}

	g_env_init_rc = spdk_env_init(&env_opts);
	if (g_env_init_rc < 0) {
		CS_LOG_ERROR("spdk_env_init failed: %d", g_env_init_rc);
	} else {
		CS_LOG_INFO("spdk_env_init OK (name=libcs shm_id=%d core_mask=%s main_core=%d)",
			    env_opts.shm_id, env_opts.core_mask ? env_opts.core_mask : "(none)",
			    env_opts.main_core);
	}
	g_env_attempted = true; /* one attempt, ever -- success or failure */

	pthread_mutex_unlock(&g_env_lock);
	return g_env_init_rc;
}

/* ───────────────────────── env var helpers (LIBCS_SPDK_* / LIBCS_XNVME_*) ───────────────────────── */

static const char *
getenv_spdk_or_legacy(const char *new_name, const char *old_name, bool *warned)
{
	const char *v = getenv(new_name);

	if (v && v[0] != '\0') {
		return v;
	}
	v = getenv(old_name);
	if (v && v[0] != '\0') {
		if (warned && !*warned) {
			*warned = true;
			CS_LOG_WARN("%s is deprecated; use %s instead", old_name, new_name);
		}
		return v;
	}
	return NULL;
}

static int
getenv_spdk_or_legacy_u32(const char *new_name, const char *old_name, uint32_t *out, bool *warned)
{
	const char *v = getenv_spdk_or_legacy(new_name, old_name, warned);

	if (!v) {
		return 0;
	}
	*out = (uint32_t)strtoul(v, NULL, 0);
	return 1;
}

/* Opt-in sync-submit deadline (design plan risk #2 mitigation). Read once;
 * 0 (the default, env unset) means "no deadline" -- bit-identical to the
 * historical unlimited-spin behavior.
 */
static pthread_mutex_t g_timeout_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_command_timeout_ms = -1; /* -1 = not yet read */

static int
get_command_timeout_ms(void)
{
	pthread_mutex_lock(&g_timeout_lock);
	if (g_command_timeout_ms < 0) {
		static bool warned = false;
		uint32_t v = 0;

		if (getenv_spdk_or_legacy_u32("LIBCS_SPDK_COMMAND_TIMEOUT",
					      "LIBCS_XNVME_COMMAND_TIMEOUT", &v, &warned)) {
			g_command_timeout_ms = (int)v;
		} else {
			g_command_timeout_ms = 0;
		}
	}
	pthread_mutex_unlock(&g_timeout_lock);
	return g_command_timeout_ms;
}

struct cs_nvme_opts
xnvme_opts_default(void)
{
	struct cs_nvme_opts opts;
	const char *v;
	uint32_t u;

	memset(&opts, 0, sizeof(opts));

	opts.be = "spdk";
	opts.css.value = 0; /* 0 = "not given"; dev_open()'s probe_cb applies
			     * SPDK_NVME_CC_CSS_IOCS unconditionally per the
			     * design plan -- cs_nvme.h is SPDK-free, so this
			     * file does not hardcode the SPDK enum value here. */
	opts.css.given = 0;

	/* LIBCS_SPDK_BE / LIBCS_SPDK_ASYNC are now meaningless no-ops (SPDK is
	 * the only transport, and async always means "caller supplied a
	 * queue"); parse-but-ignore with a one-time log, per the task's env
	 * var handling instructions. */
	{
		static bool warned_be = false, warned_async = false;

		if (getenv_spdk_or_legacy("LIBCS_SPDK_BE", "LIBCS_XNVME_BE", NULL) && !warned_be) {
			warned_be = true;
			CS_LOG_WARN(
				"LIBCS_SPDK_BE/LIBCS_XNVME_BE is a no-op: SPDK is the only libcs transport now");
		}
		if (getenv_spdk_or_legacy("LIBCS_SPDK_ASYNC", "LIBCS_XNVME_ASYNC", NULL) &&
		    !warned_async) {
			warned_async = true;
			CS_LOG_WARN(
				"LIBCS_SPDK_ASYNC/LIBCS_XNVME_ASYNC is a no-op: async-ness is now determined by whether a queue is supplied");
		}
	}

	{
		static bool w_subnqn = false, w_hostnqn = false, w_adrfam = false, w_nsid = false,
			    w_main_core = false, w_admin_to = false, w_cmd_to = false,
			    w_poll_io = false, w_core_mask = false, w_shm_id = false;

		v = getenv_spdk_or_legacy("LIBCS_SPDK_SUBNQN", "LIBCS_XNVME_SUBNQN", &w_subnqn);
		if (v) {
			opts.subnqn = v;
		}

		v = getenv_spdk_or_legacy("LIBCS_SPDK_HOSTNQN", "LIBCS_XNVME_HOSTNQN", &w_hostnqn);
		if (v) {
			opts.hostnqn = v;
		}

		v = getenv_spdk_or_legacy("LIBCS_SPDK_ADRFAM", "LIBCS_XNVME_ADRFAM", &w_adrfam);
		if (v) {
			opts.adrfam = v;
		}

		if (getenv_spdk_or_legacy_u32("LIBCS_SPDK_NSID", "LIBCS_XNVME_NSID", &u, &w_nsid)) {
			opts.nsid = u;
		}

		if (getenv_spdk_or_legacy_u32("LIBCS_SPDK_MAIN_CORE", "LIBCS_XNVME_MAIN_CORE", &u,
					      &w_main_core)) {
			opts.main_core = u;
		}

		if (getenv_spdk_or_legacy_u32("LIBCS_SPDK_ADMIN_TIMEOUT", "LIBCS_XNVME_ADMIN_TIMEOUT",
					      &u, &w_admin_to)) {
			opts.admin_timeout = u;
		}

		if (getenv_spdk_or_legacy_u32("LIBCS_SPDK_COMMAND_TIMEOUT",
					      "LIBCS_XNVME_COMMAND_TIMEOUT", &u, &w_cmd_to)) {
			opts.command_timeout = u;
		}

		if (getenv_spdk_or_legacy_u32("LIBCS_SPDK_POLL_IO", "LIBCS_XNVME_POLL_IO", &u,
					      &w_poll_io)) {
			opts.poll_io = u ? 1 : 0;
		}

		v = getenv_spdk_or_legacy("LIBCS_SPDK_CORE_MASK", "LIBCS_XNVME_CORE_MASK",
					  &w_core_mask);
		if (v) {
			opts.core_mask = v;
		}

		if (getenv_spdk_or_legacy_u32("LIBCS_SPDK_SHM_ID", "LIBCS_XNVME_SHM_ID", &u,
					      &w_shm_id)) {
			opts.shm_id = u;
		}
	}

	/* NOTE (flagged per the task instructions, not an omission): three of
	 * the ~13 legacy vars -- LIBCS_XNVME_NVM_NSID, _SLM_NSID, _QD -- have
	 * no corresponding struct cs_nvme_opts field; they are
	 * backend_xnvme.c-local variables (nvm_nsid/slm_nsid/queue depth),
	 * not transport opts. They are deliberately left for commit 5's
	 * backend_xnvme.c port, which already reads them directly, to pick
	 * up the LIBCS_SPDK_* / legacy-fallback convention via this same
	 * getenv_spdk_or_legacy() helper (declared non-static would need a
	 * header; for now commit 5 can either duplicate the ~6 line helper or
	 * this file can expose it internally -- a call for that commit, not
	 * this one, since cs_nvme.h declares no such helper and backend_xnvme.c
	 * is explicitly out of scope here). */

	/* CPCS_XNVME_PTYPE keeps its exact name -- already handled directly
	 * inside cs_nvme_cmds.c's xnvme_cmd_cp_load(), not here. Not touched. */

	return opts;
}

/* ───────────────────────── controller ref cache ─────────────────────────
 *
 * URI-string-keyed, mutex-protected, dynamically-sized (singly-linked list,
 * grows on demand -- NOT the fork's fixed g_cref[100]/g_tcp_cref[100]
 * unlocked arrays). libcs opens the same URI multiple times concurrently
 * (main dev, SLM dev, per-worker SLM readback connections -- design plan
 * risk #1, "the one genuinely new piece of engineering here"), so every
 * operation on this list holds g_cref_lock for its entire duration.
 */
struct cref_entry {
	char *uri;
	struct spdk_nvme_ctrlr *ctrlr;
	int refcount;
	struct cref_entry *next;
};

static pthread_mutex_t g_cref_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cref_entry *g_cref_head = NULL;

/* Look up an existing controller for `uri`; on hit, refcount++ and return
 * it. On miss, return NULL (caller must probe and call cref_insert()).
 */
static struct spdk_nvme_ctrlr *
cref_lookup_and_ref(const char *uri)
{
	struct cref_entry *e;
	struct spdk_nvme_ctrlr *ctrlr = NULL;

	pthread_mutex_lock(&g_cref_lock);
	for (e = g_cref_head; e; e = e->next) {
		if (strcmp(e->uri, uri) == 0) {
			e->refcount++;
			ctrlr = e->ctrlr;
			break;
		}
	}
	pthread_mutex_unlock(&g_cref_lock);
	return ctrlr;
}

static int
cref_insert(const char *uri, struct spdk_nvme_ctrlr *ctrlr)
{
	struct cref_entry *e = calloc(1, sizeof(*e));

	if (!e) {
		return -ENOMEM;
	}
	e->uri = strdup(uri);
	if (!e->uri) {
		free(e);
		return -ENOMEM;
	}
	e->ctrlr = ctrlr;
	e->refcount = 1;

	pthread_mutex_lock(&g_cref_lock);
	e->next = g_cref_head;
	g_cref_head = e;
	pthread_mutex_unlock(&g_cref_lock);
	return 0;
}

/* refcount--; spdk_nvme_detach() at 0. No-op (logged) if ctrlr isn't found,
 * which should not happen in practice.
 */
static void
cref_deref(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cref_entry **pp;
	struct cref_entry *victim = NULL;

	pthread_mutex_lock(&g_cref_lock);
	for (pp = &g_cref_head; *pp; pp = &(*pp)->next) {
		if ((*pp)->ctrlr == ctrlr) {
			(*pp)->refcount--;
			if ((*pp)->refcount <= 0) {
				victim = *pp;
				*pp = victim->next;
				/* FIX (hardware-verified race, see commit message):
				 * spdk_nvme_detach() must run BEFORE g_cref_lock is
				 * released, not after. Dropping the lock first opens
				 * a window where a concurrent xnvme_dev_open() for
				 * this same URI finds no cache entry (we already
				 * unlinked it above) and kicks off a fresh
				 * spdk_nvme_probe() for the same target while this
				 * controller is still mid-teardown -- SPDK's nvme.c
				 * rejects that with "is being destructed" / EBUSY.
				 * g_cref_lock is already a single global lock
				 * guarding the whole cache (not per-entry), so
				 * holding it across detach makes "no cache entry
				 * for this URI" and "no in-flight detach for this
				 * URI" the same fact by construction: any opener
				 * blocks here until the detach is fully done, then
				 * correctly takes the fresh-probe path against a
				 * clean slate. This only costs unrelated-URI
				 * openers one detach's duration, which matches the
				 * sync-submit-under-mutex style this file already
				 * uses for dev->sync_lock in xnvme_cmd_pass().
				 */
				spdk_nvme_detach(victim->ctrlr);
			}
			break;
		}
	}
	pthread_mutex_unlock(&g_cref_lock);

	if (victim) {
		free(victim->uri);
		free(victim);
	}
}

/* ───────────────────────── struct cs_nvme_dev / cs_nvme_queue (concrete) ───────────────────────── */

struct cs_nvme_dev {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;

	struct spdk_nvme_qpair *sync_qpair;
	pthread_mutex_t sync_lock; /* serializes submit+spin on sync_qpair */

	struct cs_nvme_opts opts;
	struct cs_nvme_ident ident;
	struct cs_nvme_geo geo;

	struct cs_nvme_spec_idfy_ctrlr idfy_ctrlr;
	bool have_idfy_ctrlr;

	struct cs_nvme_spec_idfy_ns ns_css;
	bool have_ns_css;
};

struct cs_nvme_queue {
	struct cs_nvme_dev *dev;
	struct spdk_nvme_qpair *qpair;

	struct cs_nvme_cmd_ctx *pool; /* backing storage, `capacity` entries */
	struct cs_nvme_cmd_ctx **free_stack;
	uint32_t capacity;
	uint32_t free_top; /* number of valid entries in free_stack */
	uint32_t outstanding;
};

/* ───────────────────────── sync submit (IO + Admin) ───────────────────────── */

/* Local completion token passed as cb_arg to the raw submit calls. ctx has
 * no backend-reserved scratch field (cs_nvme.h is frozen), so we cannot
 * stash the "done" flag inside *ctx itself -- a stack-local struct plays
 * the role xNVMe's ctx->be_rsvd[] private field does.
 */
struct sync_completion {
	struct cs_nvme_cmd_ctx *ctx;
	volatile int done;
};

static void
sync_cmd_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct sync_completion *sc = cb_arg;

	sc->ctx->cpl = *(const struct cs_nvme_cpl *)cpl;
	sc->done = 1;
}

/* Async submit completion: write ctx->cpl, decrement queue->outstanding,
 * invoke the user's callback if one was registered via
 * xnvme_cmd_ctx_set_cb(). Mirrors xnvme_be_spdk_async.c's cmd_async_cb.
 */
static void
async_cmd_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct cs_nvme_cmd_ctx *ctx = cb_arg;
	struct cs_nvme_queue *queue = ctx->async.queue;

	ctx->cpl = *(const struct cs_nvme_cpl *)cpl;
	if (queue && queue->outstanding > 0) {
		queue->outstanding--;
	}
	if (ctx->async.cb) {
		ctx->async.cb(ctx, ctx->async.cb_arg);
	}
}

/* Elapsed-ms helper for the opt-in deadline. */
static long
elapsed_ms_since(const struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

int
xnvme_cmd_pass(struct cs_nvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *mbuf,
	       size_t mbuf_nbytes)
{
	struct cs_nvme_dev *dev = ctx ? ctx->dev : NULL;
	int rc;

	(void)mbuf_nbytes; /* no separate length parameter on
			    * spdk_nvme_ctrlr_cmd_io_raw_with_md -- mbuf's
			    * extent is implied by the namespace's metadata
			    * size, same as xnvme_be_spdk_sync.c. None of
			    * cs_nvme_cmds.c's 7 builders pass a non-NULL
			    * mbuf here, so this is unexercised but kept
			    * signature-faithful. */

	if (!dev) {
		return -EINVAL;
	}

	if (ctx->async.queue) {
		/* Async path: submit on the caller-owned queue's qpair and
		 * return immediately. Completion (ctx->cpl, user callback,
		 * outstanding--) happens later via xnvme_queue_poke().
		 * outstanding++ BEFORE submit, --  on submit failure, per
		 * the design plan's async section. */
		struct cs_nvme_queue *queue = ctx->async.queue;

		queue->outstanding++;
		rc = spdk_nvme_ctrlr_cmd_io_raw_with_md(dev->ctrlr, queue->qpair,
							 (struct spdk_nvme_cmd *)&ctx->cmd, dbuf,
							 (uint32_t)dbuf_nbytes, mbuf, async_cmd_cb,
							 ctx);
		if (rc) {
			queue->outstanding--;
		}
		return rc;
	}

	/* Sync path: dev->sync_qpair, block until this specific command's
	 * completion lands.
	 */
	{
		struct sync_completion sc = {.ctx = ctx, .done = 0};
		int timeout_ms = get_command_timeout_ms();
		struct timespec start;

		if (!dev->sync_qpair) {
			return -EINVAL;
		}

		if (timeout_ms > 0) {
			clock_gettime(CLOCK_MONOTONIC, &start);
		}

		pthread_mutex_lock(&dev->sync_lock);

		rc = spdk_nvme_ctrlr_cmd_io_raw_with_md(dev->ctrlr, dev->sync_qpair,
							 (struct spdk_nvme_cmd *)&ctx->cmd, dbuf,
							 (uint32_t)dbuf_nbytes, mbuf, sync_cmd_cb,
							 &sc);
		if (rc) {
			pthread_mutex_unlock(&dev->sync_lock);
			return rc;
		}

		while (!sc.done) {
			int32_t pr = spdk_nvme_qpair_process_completions(dev->sync_qpair, 0);

			if (pr < 0) {
				pthread_mutex_unlock(&dev->sync_lock);
				return (int)pr;
			}
			if (timeout_ms > 0 && elapsed_ms_since(&start) >= timeout_ms) {
				CS_LOG_ERROR(
					"xnvme_cmd_pass timed out after %dms (LIBCS_SPDK_COMMAND_TIMEOUT)",
					timeout_ms);
				pthread_mutex_unlock(&dev->sync_lock);
				return -ETIMEDOUT;
			}
		}

		pthread_mutex_unlock(&dev->sync_lock);
	}

	return xnvme_cmd_ctx_cpl_status(ctx) ? -EIO : 0;
}

int
xnvme_cmd_pass_admin(struct cs_nvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *mbuf,
		     size_t mbuf_nbytes)
{
	struct cs_nvme_dev *dev = ctx ? ctx->dev : NULL;
	struct sync_completion sc = {.ctx = ctx, .done = 0};
	int timeout_ms;
	struct timespec start;
	int rc;

	(void)mbuf;	   /* spdk_nvme_ctrlr_cmd_admin_raw has no metadata-pointer
			   * parameter at all; none of the 7 CP/SLM builders in
			   * cs_nvme_cmds.c pass a non-NULL mbuf to the admin path. */
	(void)mbuf_nbytes;

	if (!dev) {
		return -EINVAL;
	}

	rc = spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, (struct spdk_nvme_cmd *)&ctx->cmd, dbuf,
					    (uint32_t)dbuf_nbytes, sync_cmd_cb, &sc);
	if (rc) {
		return rc;
	}

	timeout_ms = get_command_timeout_ms();
	if (timeout_ms > 0) {
		clock_gettime(CLOCK_MONOTONIC, &start);
	}

	while (!sc.done) {
		/* FIX vs xnvme_be_spdk_admin.c:41-62 ("xnvme_be_spdk_sync_cmd_admin"
		 * polls this in a while(!*completed) loop with NO check of
		 * the int32_t return for negative -- can spin forever if the
		 * controller is lost). Bail out on a negative return here. */
		int32_t pr = spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);

		if (pr < 0) {
			CS_LOG_ERROR("spdk_nvme_ctrlr_process_admin_completions failed: %d", pr);
			return (int)pr;
		}
		if (timeout_ms > 0 && elapsed_ms_since(&start) >= timeout_ms) {
			CS_LOG_ERROR(
				"xnvme_cmd_pass_admin timed out after %dms (LIBCS_SPDK_COMMAND_TIMEOUT)",
				timeout_ms);
			return -ETIMEDOUT;
		}
	}

	return xnvme_cmd_ctx_cpl_status(ctx) ? -EIO : 0;
}

/* ───────────────────────── admin identify helper (dev_open only) ───────────────────────── */

struct identify_completion {
	volatile int done;
	uint16_t sc;
	uint16_t sct;
};

static void
identify_cmd_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct identify_completion *ic = cb_arg;

	ic->sc = cpl->status.sc;
	ic->sct = cpl->status.sct;
	ic->done = 1;
}

/* Raw Admin Identify (CNS in cdw10 bits 0-7, CSI in cdw11 bits 24-31 per the
 * NVMe base spec's Identify command). Used only for the CP-specific
 * Identify Namespace (CNS=0x05, CSI=CS_NVME_CSI_CP) that vanilla SPDK does
 * not natively cache (it does not know about a non-standard CSI). The
 * ctrlr-wide Identify Controller data is NOT fetched this way -- SPDK
 * already performs and caches it during attach, retrievable via
 * spdk_nvme_ctrlr_get_data(), which do_identify_and_mdts() uses directly
 * instead of re-issuing a redundant Identify Controller command.
 */
static int
admin_identify_raw(struct spdk_nvme_ctrlr *ctrlr, uint8_t cns, uint32_t nsid, uint8_t csi,
		    void *buf, uint32_t buf_len)
{
	struct spdk_nvme_cmd cmd;
	struct identify_completion ic = {0};
	int rc;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = (uint8_t)OPC_IDENTIFY; /* 0x06 */
	cmd.nsid = nsid;
	cmd.cdw10 = cns;
	cmd.cdw11 = (uint32_t)csi << 24;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf, buf_len, identify_cmd_cb, &ic);
	if (rc) {
		return rc;
	}

	while (!ic.done) {
		int32_t pr = spdk_nvme_ctrlr_process_admin_completions(ctrlr);

		if (pr < 0) {
			return (int)pr;
		}
	}
	if (ic.sc || ic.sct) {
		return -EIO;
	}
	return 0;
}

/* Identify-on-open (design plan item 9) + MDTS check (item 10). On ANY
 * failure of the CP-specific Identify Namespace, dev->have_ns_css stays
 * false and xnvme_dev_get_ns_css() returns NULL -- preserving the EXISTING
 * caller-side 16/256 fallback already present in backend_xnvme.c. This
 * function must never itself fabricate those defaults; that is the
 * caller's job, unchanged.
 */
static void
do_identify_and_mdts(struct cs_nvme_dev *dev, uint32_t csi)
{
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(dev->ctrlr);

	if (cdata) {
		dev->idfy_ctrlr.vid = cdata->vid;
		dev->idfy_ctrlr.ssvid = cdata->ssvid;
		memcpy(dev->idfy_ctrlr.sn, cdata->sn, sizeof(dev->idfy_ctrlr.sn));
		memcpy(dev->idfy_ctrlr.mn, cdata->mn, sizeof(dev->idfy_ctrlr.mn));
		memcpy(dev->idfy_ctrlr.fr, cdata->fr, sizeof(dev->idfy_ctrlr.fr));
		dev->have_idfy_ctrlr = true;
	} else {
		CS_LOG_WARN("spdk_nvme_ctrlr_get_data() returned NULL; vid/mn unavailable");
		dev->have_idfy_ctrlr = false;
	}

	if (csi == CS_NVME_CSI_CP) {
		int rc = admin_identify_raw(dev->ctrlr, 0x05, dev->opts.nsid, CS_NVME_CSI_CP,
					    &dev->ns_css, sizeof(dev->ns_css));
		if (rc) {
			CS_LOG_WARN(
				"CP Identify Namespace (CNS=0x05 CSI=0x4) failed: %d -- caller falls back to hardcoded maxact/maxpb defaults",
				rc);
			dev->have_ns_css = false;
		} else {
			dev->have_ns_css = true;
		}
	} else {
		dev->have_ns_css = false;
	}

	/* MDTS check (design plan risk #4): warn loudly if the controller's
	 * maximum transfer size is smaller than the 32 MiB fixed chunk size
	 * backend_xnvme_compute.c ships inline per CP_EXECUTE call. Never
	 * silently clamp -- that would corrupt the Execute protocol's DLEN
	 * framing, not just underperform. */
	dev->geo.mdts_nbytes = spdk_nvme_ctrlr_get_max_xfer_size(dev->ctrlr);
	if (dev->geo.mdts_nbytes != 0 && dev->geo.mdts_nbytes < CS_NVME_INLINE_EXECUTE_CHUNK_BYTES) {
		CS_LOG_WARN(
			"controller MDTS (%u bytes) is smaller than libcs's fixed 32 MiB inline Execute chunk size -- CP_EXECUTE will fail or be rejected by the device; this is NOT silently clamped",
			dev->geo.mdts_nbytes);
	}
}

/* ───────────────────────── URI -> trid classification ───────────────────────── */

/* Mirrors cs_helpers.c's xnvme_uri_is_local_direct(): true for "pci:"/"pcie:"
 * prefixes.
 */
static bool
uri_is_local_direct(const char *uri)
{
	return strncmp(uri, "pci:", 4) == 0 || strncmp(uri, "pcie:", 5) == 0;
}

static bool
uri_is_rejected_dev_node(const char *uri)
{
	return uri[0] == '/';
}

/* Build a spdk_nvme_transport_id_parse()-compatible string and parse it.
 * Both the local/PCIe and fabric/TCP paths resolve to this, then both use
 * synchronous spdk_nvme_probe() (design plan: "로컬/원격 모두 동기
 * spdk_nvme_probe") -- no spdk_nvme_connect_async() anywhere in this file.
 */
static int
build_trid(const char *uri, const struct cs_nvme_opts *opts, struct spdk_nvme_transport_id *trid,
	   bool *out_is_fabrics)
{
	char buf[512];

	memset(trid, 0, sizeof(*trid));

	if (uri_is_local_direct(uri)) {
		const char *addr = uri + (strncmp(uri, "pcie:", 5) == 0 ? 5 : 4);

		snprintf(buf, sizeof(buf), "trtype:PCIe traddr:%s", addr);
		*out_is_fabrics = false;
	} else {
		char host[256];
		int port = 0;
		const char *adrfam = (opts && opts->adrfam) ? opts->adrfam : "IPv4";
		const char *subnqn =
			(opts && opts->subnqn) ? opts->subnqn : SPDK_NVMF_DISCOVERY_NQN;

		if (sscanf(uri, "%255[^:]:%d", host, &port) != 2) {
			return -EINVAL;
		}
		snprintf(buf, sizeof(buf), "trtype:TCP adrfam:%s traddr:%s trsvcid:%d subnqn:%s",
			 adrfam, host, port, subnqn);
		*out_is_fabrics = true;
	}

	return spdk_nvme_transport_id_parse(trid, buf);
}

/* ───────────────────────── probe/attach callbacks ───────────────────────── */

struct probe_state {
	const struct cs_nvme_opts *opts;
	bool is_fabrics;
	struct spdk_nvme_ctrlr *attached; /* set by attach_cb */
};

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *copts)
{
	struct probe_state *ps = cb_ctx;
	const struct cs_nvme_opts *opts = ps->opts;

	(void)trid;

	copts->command_set = SPDK_NVME_CC_CSS_IOCS;

	/* TCP header/data digest forced OFF -- matches
	 * xnvme_be_spdk_tcp.c:91-94's stated performance rationale. */
	copts->header_digest = false;
	copts->data_digest = false;

	if (opts && opts->keep_alive_timeout_ms) {
		copts->keep_alive_timeout_ms = opts->keep_alive_timeout_ms;
	}
	if (opts && opts->hostnqn) {
		snprintf(copts->hostnqn, sizeof(copts->hostnqn), "%s", opts->hostnqn);
	}

	return true; /* attach to every probed controller -- trid already
		      * pins us to exactly the one we asked for. */
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *copts)
{
	struct probe_state *ps = cb_ctx;

	(void)trid;
	(void)copts;
	ps->attached = ctrlr;
}

/* ───────────────────────── dev open/close ───────────────────────── */

struct cs_nvme_dev *
xnvme_dev_open(const char *dev_uri, struct cs_nvme_opts *opts)
{
	struct cs_nvme_dev *dev = NULL;
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	bool is_fabrics = false;
	bool inserted = false;
	uint32_t csi;
	int rc;

	if (!dev_uri || !opts) {
		return NULL;
	}

	/* DECISION 1 (design plan, accepted regression): /dev/nvmeXnY kernel
	 * block-device URIs are rejected outright. libcs is SPDK-only now;
	 * there is no kernel-passthru fallback (it cannot set Execute's
	 * CDW04/DLEN -- struct nvme_passthru_cmd has no cdw4 field -- and has
	 * no SLM/MRS support at all). Point the caller at the supported
	 * syntax and the project's setup script instead of failing opaquely. */
	if (uri_is_rejected_dev_node(dev_uri)) {
		CS_LOG_ERROR(
			"'%s' looks like a kernel block-device path; libcs's SPDK backend requires "
			"'pci:<BDF>' (local) or 'pcie:<BDF>' (local, alias) or 'host:port' (NVMe-oF "
			"fabrics) URIs instead. Run ./setup.sh to bind the device to vfio-pci / set "
			"up hugepages, then retry with e.g. 'pci:0000:01:00.0'.",
			dev_uri);
		return NULL;
	}

	if (!opts->nsid) {
		CS_LOG_ERROR("xnvme_dev_open: opts->nsid is required (0 given) for '%s'", dev_uri);
		return NULL;
	}

	rc = ensure_spdk_env_init(opts);
	if (rc < 0) {
		return NULL;
	}

	if (build_trid(dev_uri, opts, &trid, &is_fabrics) != 0) {
		CS_LOG_ERROR("xnvme_dev_open: failed to parse '%s' into a transport id", dev_uri);
		return NULL;
	}

	ctrlr = cref_lookup_and_ref(dev_uri);
	if (!ctrlr) {
		struct probe_state ps = {.opts = opts, .is_fabrics = is_fabrics, .attached = NULL};

		rc = spdk_nvme_probe(&trid, &ps, probe_cb, attach_cb, NULL);
		if (rc != 0 || !ps.attached) {
			CS_LOG_ERROR("xnvme_dev_open: spdk_nvme_probe('%s') failed: rc=%d attached=%p",
				     dev_uri, rc, (void *)ps.attached);
			return NULL;
		}
		ctrlr = ps.attached;

		rc = cref_insert(dev_uri, ctrlr);
		if (rc) {
			spdk_nvme_detach(ctrlr);
			return NULL;
		}
		inserted = true;
	}

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, opts->nsid);
	if (!ns) {
		CS_LOG_ERROR("xnvme_dev_open: nsid %u not found on '%s'", opts->nsid, dev_uri);
		goto fail_release_ctrlr;
	}

	csi = (uint32_t)spdk_nvme_ns_get_csi(ns);

	/* ───── namespace activation exception (design plan item 4) ─────
	 *
	 * DO NOT gate on spdk_nvme_ns_is_active() when csi is SLM (0x3) or CP
	 * (0x4): those namespaces are never reported active by the controller
	 * (this is a property of the CPCS target's namespace management, not
	 * a bug), mirroring xnvme_be_spdk_dev.c's own exception at its
	 * attach-time namespace gate. If a future maintainer is tempted to
	 * "fix" this by adding an is_active() check for CP/SLM, they will
	 * break every compute/SLM device open. Standard NVM namespaces (any
	 * other csi) still get the normal activation check.
	 */
	if (csi != CS_NVME_CSI_SLM && csi != CS_NVME_CSI_CP) {
		if (!spdk_nvme_ns_is_active(ns)) {
			CS_LOG_ERROR("xnvme_dev_open: nsid %u on '%s' is not active", opts->nsid,
				     dev_uri);
			goto fail_release_ctrlr;
		}
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		goto fail_release_ctrlr;
	}

	dev->ctrlr = ctrlr;
	dev->ns = ns;
	dev->opts = *opts;
	pthread_mutex_init(&dev->sync_lock, NULL);

	dev->sync_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (!dev->sync_qpair) {
		CS_LOG_ERROR("xnvme_dev_open: spdk_nvme_ctrlr_alloc_io_qpair (sync) failed for '%s'",
			     dev_uri);
		pthread_mutex_destroy(&dev->sync_lock);
		free(dev);
		dev = NULL;
		goto fail_release_ctrlr;
	}

	memset(&dev->ident, 0, sizeof(dev->ident));
	snprintf(dev->ident.uri, sizeof(dev->ident.uri), "%s", dev_uri);
	if (opts->subnqn) {
		snprintf(dev->ident.subnqn, sizeof(dev->ident.subnqn), "%s", opts->subnqn);
	}
	dev->ident.nsid = opts->nsid;
	dev->ident.csi = (uint8_t)csi;

	memset(&dev->geo, 0, sizeof(dev->geo));
	dev->geo.lba_nbytes = spdk_nvme_ns_get_sector_size(ns);

	do_identify_and_mdts(dev, csi); /* also sets dev->geo.mdts_nbytes */

	return dev;

fail_release_ctrlr:
	if (inserted) {
		cref_deref(ctrlr);
	} else {
		/* We reused an existing cache entry (refcount++ already
		 * happened in cref_lookup_and_ref) but failed afterward --
		 * release that same ref. */
		cref_deref(ctrlr);
	}
	return NULL;
}

void
xnvme_dev_close(struct cs_nvme_dev *dev)
{
	if (!dev) {
		return;
	}

	if (dev->sync_qpair) {
		spdk_nvme_ctrlr_free_io_qpair(dev->sync_qpair);
	}
	pthread_mutex_destroy(&dev->sync_lock);

	cref_deref(dev->ctrlr);

	free(dev);
}

uint32_t
xnvme_dev_get_nsid(const struct cs_nvme_dev *dev)
{
	return dev ? dev->ident.nsid : 0;
}

const struct cs_nvme_geo *
xnvme_dev_get_geo(const struct cs_nvme_dev *dev)
{
	return dev ? &dev->geo : NULL;
}

const struct cs_nvme_ident *
xnvme_dev_get_ident(const struct cs_nvme_dev *dev)
{
	return dev ? &dev->ident : NULL;
}

const struct cs_nvme_spec_idfy_ctrlr *
xnvme_dev_get_ctrlr(const struct cs_nvme_dev *dev)
{
	if (!dev || !dev->have_idfy_ctrlr) {
		return NULL;
	}
	return &dev->idfy_ctrlr;
}

const struct cs_nvme_spec_idfy_ns *
xnvme_dev_get_ns_css(const struct cs_nvme_dev *dev)
{
	if (!dev || !dev->have_ns_css) {
		return NULL; /* caller (backend_xnvme.c) falls back to
			      * hardcoded maxact=16/maxpb=256 defaults -- this
			      * is EXISTING behavior at the caller level, not
			      * a regression introduced here. */
	}
	return &dev->ns_css;
}

struct cs_nvme_cmd_ctx
xnvme_cmd_ctx_from_dev(struct cs_nvme_dev *dev)
{
	struct cs_nvme_cmd_ctx ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.dev = dev;
	return ctx;
}

/* ───────────────────────── queues (async) ───────────────────────── */

int
xnvme_queue_init(struct cs_nvme_dev *dev, uint16_t capacity, int opts, struct cs_nvme_queue **queue)
{
	struct cs_nvme_queue *q;
	struct spdk_nvme_io_qpair_opts qopts;
	uint32_t i;

	(void)opts; /* CS_NVME_QUEUE_IOPOLL/SQPOLL: not currently requested with
		     * a non-zero value by the backend (kept for signature
		     * parity, same as cs_nvme.h's own comment on this enum). */

	if (!dev || !queue || capacity == 0) {
		return -EINVAL;
	}

	q = calloc(1, sizeof(*q));
	if (!q) {
		return -ENOMEM;
	}

	q->pool = calloc(capacity, sizeof(*q->pool));
	q->free_stack = calloc(capacity, sizeof(*q->free_stack));
	if (!q->pool || !q->free_stack) {
		free(q->pool);
		free(q->free_stack);
		free(q);
		return -ENOMEM;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(dev->ctrlr, &qopts, sizeof(qopts));
	if (qopts.io_queue_size < capacity) {
		qopts.io_queue_size = capacity;
	}
	qopts.io_queue_requests = qopts.io_queue_size * 2;

	q->qpair = spdk_nvme_ctrlr_alloc_io_qpair(dev->ctrlr, &qopts, sizeof(qopts));
	if (!q->qpair) {
		free(q->pool);
		free(q->free_stack);
		free(q);
		return -ENOMEM;
	}

	q->dev = dev;
	q->capacity = capacity;
	q->outstanding = 0;

	for (i = 0; i < capacity; i++) {
		q->pool[i] = xnvme_cmd_ctx_from_dev(dev);
		q->pool[i].async.queue = q;
		q->free_stack[i] = &q->pool[i];
	}
	q->free_top = capacity;

	*queue = q;
	return 0;
}

int
xnvme_queue_term(struct cs_nvme_queue *queue)
{
	if (!queue) {
		return -EINVAL;
	}
	if (queue->qpair) {
		spdk_nvme_ctrlr_free_io_qpair(queue->qpair);
	}
	free(queue->pool);
	free(queue->free_stack);
	free(queue);
	return 0;
}

int
xnvme_queue_poke(struct cs_nvme_queue *queue, uint32_t max)
{
	int32_t rc;

	if (!queue) {
		return -EINVAL;
	}
	rc = spdk_nvme_qpair_process_completions(queue->qpair, max);
	return (int)rc; /* negative on error; otherwise number of completions reaped */
}

int
xnvme_queue_drain(struct cs_nvme_queue *queue)
{
	if (!queue) {
		return -EINVAL;
	}
	while (queue->outstanding > 0) {
		int rc = xnvme_queue_poke(queue, 0);

		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

uint32_t
xnvme_queue_get_outstanding(struct cs_nvme_queue *queue)
{
	return queue ? queue->outstanding : 0;
}

/* Not thread-safe (matches xNVMe's documented contract, cs_nvme.h). */
struct cs_nvme_cmd_ctx *
xnvme_queue_get_cmd_ctx(struct cs_nvme_queue *queue)
{
	struct cs_nvme_cmd_ctx *ctx;

	if (!queue || queue->free_top == 0) {
		return NULL;
	}
	ctx = queue->free_stack[--queue->free_top];
	/* Reset to a clean cmd/cpl but keep dev + async.queue intact. */
	memset(&ctx->cmd, 0, sizeof(ctx->cmd));
	memset(&ctx->cpl, 0, sizeof(ctx->cpl));
	ctx->async.cb = NULL;
	ctx->async.cb_arg = NULL;
	ctx->opts = 0;
	return ctx;
}

int
xnvme_queue_put_cmd_ctx(struct cs_nvme_queue *queue, struct cs_nvme_cmd_ctx *ctx)
{
	if (!queue || !ctx) {
		return -EINVAL;
	}
	if (queue->free_top >= queue->capacity) {
		return -ENOSPC; /* double-put, shouldn't happen */
	}
	queue->free_stack[queue->free_top++] = ctx;
	return 0;
}

/* ───────────────────────── buffers ───────────────────────── */

void *
xnvme_buf_alloc(const struct cs_nvme_dev *dev, size_t nbytes)
{
	size_t align = (dev && dev->geo.lba_nbytes) ? dev->geo.lba_nbytes : 4096;

	return spdk_dma_malloc(nbytes, align, NULL);
}

void
xnvme_buf_free(const struct cs_nvme_dev *dev, void *buf)
{
	(void)dev;
	spdk_dma_free(buf);
}

void *
xnvme_buf_clear(void *buf, size_t nbytes)
{
	if (buf) {
		memset(buf, 0, nbytes);
	}
	return buf;
}

int
xnvme_buf_vtophys(const struct cs_nvme_dev *dev, void *buf, uint64_t *phys)
{
	uint64_t size = 0;
	uint64_t addr;

	(void)dev;
	if (!buf || !phys) {
		return -EINVAL;
	}

	addr = spdk_vtophys(buf, &size);
	if (addr == SPDK_VTOPHYS_ERROR) {
		return -EFAULT;
	}
	*phys = addr;
	return 0;
}

/* ───────────────────────── NVM read/write ─────────────────────────
 *
 * Dual-mode, same as xnvme_cmd_pass(): if ctx->async.queue is set (ctx came
 * from xnvme_queue_get_cmd_ctx()), submit non-blocking on the queue's qpair
 * and return immediately; otherwise (ctx came from xnvme_cmd_ctx_from_dev())
 * block on dev->sync_qpair until this command completes. Both call sites
 * exist in backend_xnvme_slm.c (SLM staging/readback), grepped and
 * confirmed in this commit's investigation.
 */
static void
build_nvm_rw_cmd(struct cs_nvme_cmd *cmd, uint8_t opc, uint32_t nsid, uint64_t slba, uint16_t nlb)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->common.opcode = opc;
	cmd->common.nsid = nsid;
	cmd->common.cdw10 = (uint32_t)slba;
	cmd->common.cdw11 = (uint32_t)(slba >> 32);
	cmd->common.cdw12 = nlb; /* NLB is zero-based per NVMe spec; callers
				  * already pass (nlb-1) where relevant, same
				  * convention as the fork's generic nvm cmd
				  * builder -- not re-derived here. */
}

static int
nvm_rw_submit(struct cs_nvme_cmd_ctx *ctx, uint8_t opc, uint32_t nsid, uint64_t slba, uint16_t nlb,
	      void *dbuf, void *mbuf)
{
	if (!ctx || !ctx->dev) {
		return -EINVAL;
	}
	build_nvm_rw_cmd(&ctx->cmd, opc, nsid, slba, nlb);
	return xnvme_cmd_pass(ctx, dbuf, (size_t)(nlb + 1) * ctx->dev->geo.lba_nbytes, mbuf, 0);
}

int
xnvme_nvm_read(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint64_t slba, uint16_t nlb, void *dbuf,
	       void *mbuf)
{
	return nvm_rw_submit(ctx, (uint8_t)OPC_NVM_READ, nsid, slba, nlb, dbuf, mbuf);
}

int
xnvme_nvm_write(struct cs_nvme_cmd_ctx *ctx, uint32_t nsid, uint64_t slba, uint16_t nlb,
		const void *dbuf, const void *mbuf)
{
	return nvm_rw_submit(ctx, (uint8_t)OPC_NVM_WRITE, nsid, slba, nlb, (void *)dbuf,
			     (void *)mbuf);
}
