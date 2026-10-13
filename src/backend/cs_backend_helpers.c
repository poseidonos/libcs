/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_backend_helpers.c
 * @brief Portable vtable ops shared by the NVMe passthrough backends
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cs_internal.h"
#include "cs_programs.h"

csStatus
cs_backend_mem_init(struct cs_backend_ctx *ctx, void *ptr,
		    uint8_t fill_value, size_t size)
{
	(void)ctx;
	if (!ptr) return CS_STATUS_ERROR_INVALID_PARAM;
	memset(ptr, fill_value, size);
	return CS_STATUS_SUCCESS;
}

csStatus
cs_backend_prog_get_name(struct cs_backend_ctx *ctx, uint16_t prog_index,
			 char *name, size_t name_size)
{
	(void)ctx;
	if (!name || name_size == 0) return CS_STATUS_ERROR_INVALID_PARAM;
	const char *prog_name = cs_prog_name_from_pind(prog_index);
	if (!prog_name) return CS_STATUS_ERROR_NOT_FOUND;
	snprintf(name, name_size, "%s", prog_name);
	return CS_STATUS_SUCCESS;
}

csStatus
cs_backend_query_csf_list(struct cs_backend_ctx *ctx, csCSFInfo *csfs, uint32_t *count)
{
	(void)ctx;
	if (!count) return CS_STATUS_ERROR_INVALID_PARAM;
	uint32_t total;
	const cs_csf_entry_t *tbl = cs_csf_table(&total);
	if (!csfs) { *count = total; return CS_STATUS_SUCCESS; }
	if (*count < total) { *count = total; return CS_STATUS_ERROR_LIMIT; }
	for (uint32_t i = 0; i < total; i++) {
		memset(&csfs[i], 0, sizeof(csfs[i]));
		snprintf(csfs[i].name, sizeof(csfs[i].name), "%s", tbl[i].name);
		csfs[i].version = 1;
	}
	*count = total;
	return CS_STATUS_SUCCESS;
}

csStatus
cs_backend_abort_request(struct cs_backend_ctx *ctx, volatile int *completion)
{
	(void)ctx;
	if (!completion) return CS_STATUS_ERROR_INVALID_PARAM;
	if (*completion == 0) { *completion = -1; return CS_STATUS_SUCCESS; }
	return CS_STATUS_ERROR_BUSY;
}

void
cs_backend_ctrl_path_from_uri(const char *uri, char *out, size_t out_sz,
			      const char *scheme)
{
	/* Strip optional "<scheme>:" prefix */
	size_t scheme_len = strlen(scheme);
	if (strncmp(uri, scheme, scheme_len) == 0) uri += scheme_len;

	strncpy(out, uri, out_sz - 1);
	out[out_sz - 1] = '\0';

	/* Strip "n<digits>" suffix if present */
	char *p = strstr(out, "n");
	while (p) {
		char *q = p + 1;
		bool all_digits = (*q != '\0');
		while (*q) { if (*q < '0' || *q > '9') { all_digits = false; break; } q++; }
		if (all_digits && q > p + 1) { *p = '\0'; break; }
		p = strstr(p + 1, "n");
	}
}
