/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_api_program.c
 * @brief CS API Program (CSF) Management Implementation
 */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_internal.h"
#include "cs_programs.h"

csStatus
csCSFDownload(csCSxHandle csx_handle, const void *csf_binary,
	      size_t size, csCSFId *csf_id)
{
	if (!csf_binary || size == 0 || !csf_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	*csf_id = 0;

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	/*
	 * Bug 1 fix: atomically find and claim a free program slot under
	 * ctx->lock. Slot 0 is reserved (see cs_device_alloc()) so the scan
	 * starts at 1. Without this, csGetCSFId() used to always report
	 * success, so this scan never found a "free" slot and every
	 * download silently collided on prog_index == 0.
	 */
	uint16_t limit = ctx->info.max_programs;
	if (limit > CS_MAX_PROGRAM_SLOTS) {
		limit = CS_MAX_PROGRAM_SLOTS;
	}

	uint16_t prog_index = 0;
	bool found = false;
	pthread_mutex_lock(&ctx->lock);
	for (uint16_t i = 1; i < limit; i++) {
		if (!atomic_load(&ctx->prog_slot_used[i])) {
			atomic_store(&ctx->prog_slot_used[i], true);
			prog_index = i;
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&ctx->lock);

	if (!found) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_LIMIT;
	}

	status = csLoadProgram(csx_handle, prog_index, csf_binary, size);
	if (status != CS_STATUS_SUCCESS) {
		atomic_store(&ctx->prog_slot_used[prog_index], false);
		cs_device_release(ctx);
		return status;
	}

	status = csActivateProgram(csx_handle, prog_index);
	if (status != CS_STATUS_SUCCESS) {
		atomic_store(&ctx->prog_slot_used[prog_index], false);
		cs_device_release(ctx);
		return status;
	}

	*csf_id = (csCSFId)prog_index;
	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csCSEEDownload(csCSxHandle csx_handle, const void *image,
	       size_t size, uint32_t *csee_id)
{
	if (!image || size == 0 || !csee_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	*csee_id = 0;

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	/* Bug 1 fix: see csCSFDownload() above — atomically find and claim a
	 * free program slot instead of relying on the old always-succeeds
	 * csGetCSFId() stub. */
	uint16_t limit = ctx->info.max_programs;
	if (limit > CS_MAX_PROGRAM_SLOTS) {
		limit = CS_MAX_PROGRAM_SLOTS;
	}

	uint16_t prog_index = 0;
	bool found = false;
	pthread_mutex_lock(&ctx->lock);
	for (uint16_t i = 1; i < limit; i++) {
		if (!atomic_load(&ctx->prog_slot_used[i])) {
			atomic_store(&ctx->prog_slot_used[i], true);
			prog_index = i;
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&ctx->lock);

	if (!found) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_LIMIT;
	}

	status = csLoadProgram(csx_handle, prog_index, image, size);
	if (status != CS_STATUS_SUCCESS) {
		atomic_store(&ctx->prog_slot_used[prog_index], false);
		cs_device_release(ctx);
		return status;
	}

	*csee_id = (uint32_t)prog_index;
	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csGroupComputeByIds(csCSxHandle csx_handle, const csCSFId *csf_ids,
		     uint32_t count, uint32_t *group_id)
{
	if (!csf_ids || count == 0 || count > CS_MAX_GROUP_CSFS || !group_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t dev_index = cs_handle_get_index(csx_handle);

	cs_group_ctx_t *g;
	status = cs_group_alloc_ctx(dev_index, &g, group_id);
	if (status != CS_STATUS_SUCCESS) {
		cs_device_release(ctx);
		return status;
	}

	g->csf_count = count;
	memcpy(g->csf_ids, csf_ids, count * sizeof(csCSFId));

	cs_group_release(g);
	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csUngroupComputeFromGroupId(csCSxHandle csx_handle, uint32_t group_id)
{
	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	status = cs_group_free_ctx(group_id);

	cs_device_release(ctx);
	return status;
}

csStatus
csQueryLibrarySupport(csLibrarySupport *support)
{
	if (!support) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	memset(support, 0, sizeof(*support));
	support->api_version = 0x0100;
	support->num_backends = g_state.backend_count;
	support->features = CS_FEATURE_ASYNC_COMPUTE | CS_FEATURE_BATCH |
			    CS_FEATURE_EVENTS;
	return CS_STATUS_SUCCESS;
}

/**
 * Program Management
 */
csStatus
csGetCSFId(csCSxHandle csx_handle, uint16_t prog_index, csCSFId *csf_id)
{
	if (!csf_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	/*
	 * Bug 1 fix: this used to unconditionally return SUCCESS for any
	 * prog_index, which made the free-slot scan in csCSFDownload() /
	 * csCSEEDownload() never find a free slot (it breaks on the first
	 * *failure*), so every download silently collided on prog_index 0.
	 * Report whether a program is actually loaded/activated at this
	 * slot instead. Slot 0 is always considered present (device
	 * built-in/default program; see cs_device_alloc()).
	 */
	csStatus result;
	if (prog_index >= CS_MAX_PROGRAM_SLOTS ||
	    !atomic_load(&ctx->prog_slot_used[prog_index])) {
		result = CS_STATUS_ERROR_NOT_FOUND;
	} else {
		/* CSF ID is simply the program index */
		*csf_id = prog_index;
		result = CS_STATUS_SUCCESS;
	}

	cs_device_release(ctx);
	return result;
}

csStatus
csGetCSFInfo(csCSxHandle csx_handle, csCSFId csf_id, csCSFInfo *info)
{
	if (!info) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	(void)ctx;

	memset(info, 0, sizeof(*info));
	snprintf(info->name, sizeof(info->name), "prog_%u", (unsigned)csf_id);
	info->version = 0;
	info->flags = 0;

	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csGetCSFIdByName(csCSxHandle csx_handle, const char *name, csCSFId *csf_id)
{
	if (!name || !csf_id) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	uint32_t tbl_count;
	const cs_csf_entry_t *tbl = cs_csf_table(&tbl_count);

	csStatus result = CS_STATUS_ERROR_NOT_FOUND;
	for (uint32_t i = 0; i < tbl_count; i++) {
		if (strcmp(name, tbl[i].name) == 0) {
			*csf_id = tbl[i].csf_id;
			result = CS_STATUS_SUCCESS;
			break;
		}
	}

	cs_device_release(ctx);
	return result;
}

csStatus
csQueryCSFList(csCSxHandle csx_handle, csCSFInfo *csfs, uint32_t *count)
{
	if (!count) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (ctx->backend && ctx->backend->query_csf_list) {
		status = ctx->backend->query_csf_list(ctx->backend_ctx, csfs, count);
		cs_device_release(ctx);
		return status;
	}

	uint32_t total;
	const cs_csf_entry_t *tbl = cs_csf_table(&total);

	if (!csfs) {
		*count = total;
		cs_device_release(ctx);
		return CS_STATUS_SUCCESS;
	}

	if (*count < total) {
		*count = total;
		cs_device_release(ctx);
		return CS_STATUS_ERROR_LIMIT;
	}

	for (uint32_t i = 0; i < total; i++) {
		memset(&csfs[i], 0, sizeof(csfs[i]));
		snprintf(csfs[i].name, sizeof(csfs[i].name), "%s", tbl[i].name);
		csfs[i].version = 1;
		csfs[i].flags = 0;
	}
	*count = total;

	cs_device_release(ctx);
	return CS_STATUS_SUCCESS;
}

csStatus
csLoadProgram(csCSxHandle csx_handle, uint16_t prog_index,
	      const void *program, size_t program_size)
{
	if (!program || program_size == 0) {
		return CS_STATUS_ERROR_INVALID_PARAM;
	}

	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!ctx->backend || !ctx->backend->prog_load) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	status = ctx->backend->prog_load(ctx->backend_ctx, prog_index,
					 program, program_size);
	cs_device_release(ctx);
	return status;
}

csStatus
csActivateProgram(csCSxHandle csx_handle, uint16_t prog_index)
{
	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!ctx->backend || !ctx->backend->prog_activate) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	status = ctx->backend->prog_activate(ctx->backend_ctx, prog_index);
	cs_device_release(ctx);
	return status;
}

csStatus
csDeactivateProgram(csCSxHandle csx_handle, uint16_t prog_index)
{
	cs_device_ctx_t *ctx;
	csStatus status = cs_device_lookup(csx_handle, &ctx);
	if (status != CS_STATUS_SUCCESS) {
		return status;
	}

	if (!ctx->backend || !ctx->backend->prog_deactivate) {
		cs_device_release(ctx);
		return CS_STATUS_ERROR_NOT_SUPPORTED;
	}

	status = ctx->backend->prog_deactivate(ctx->backend_ctx, prog_index);

	/*
	 * Bug 1 fix: the public API has no separate CSF/CSEE "unload" call,
	 * so csDeactivateProgram() is the point at which a program slot
	 * claimed by csCSFDownload()/csCSEEDownload() is released back to
	 * the free pool. Slot 0 is reserved and never toggled here.
	 */
	if (status == CS_STATUS_SUCCESS && prog_index != 0 &&
	    prog_index < CS_MAX_PROGRAM_SLOTS) {
		atomic_store(&ctx->prog_slot_used[prog_index], false);
	}

	cs_device_release(ctx);
	return status;
}
