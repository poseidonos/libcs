/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_programs.h
 * @brief Single source of truth for CPCS program index constants and tables
 */

#ifndef CS_PROGRAMS_H
#define CS_PROGRAMS_H

#include <stddef.h>
#include <stdint.h>
#include "cs_api.h"

/* CPCS program indices (PIND) — wire protocol values matching SPDK target */
#define LIBCS_PIND_MEMCPY             0u
#define LIBCS_PIND_MEMFILL            1u
#define LIBCS_PIND_SUM64              2u
#define LIBCS_PIND_MAX64              3u
#define LIBCS_PIND_MIN64              4u
/*
 * FILTER_AGG / FILTERED_TOPK_EXACT take an MR-id based request struct
 * (the device-side evaluation request header), not an inline input buffer, so
 * they are NOT reachable through the csRunCompute helpers or csComputeDesc.
 * Declared here so cs_prog_name_from_pind() can name them; actual wiring needs
 * the MR-execute path.
 */
#define LIBCS_PIND_FILTER_AGG         5u
#define LIBCS_PIND_FILTERED_TOPK_EXACT 6u
#define LIBCS_PIND_DOT_PRODUCT        13u
#define LIBCS_PIND_FILTER_GT          14u
#define LIBCS_PIND_MEMCPY_INLINE      15u
#define LIBCS_PIND_RLE_COMPRESS       16u
#define LIBCS_PIND_MULTI_AGG64        17u
#define LIBCS_PIND_L2_DISTANCE_SQ     18u
#define LIBCS_PIND_COSINE_SIMILARITY  19u

static inline uint16_t
cs_prog_index_from_csf(csCSFId csf_id)
{
	switch (csf_id) {
	case CS_CSF_DOT_PRODUCT:       return LIBCS_PIND_DOT_PRODUCT;
	case CS_CSF_FILTER_GT:         return LIBCS_PIND_FILTER_GT;
	case CS_CSF_MEMCPY:            return LIBCS_PIND_MEMCPY;
	case CS_CSF_MEMFILL:           return LIBCS_PIND_MEMFILL;
	case CS_CSF_MEMCPY_INLINE:     return LIBCS_PIND_MEMCPY_INLINE;
	case CS_CSF_FILTER_AGG:        return LIBCS_PIND_FILTER_AGG;
	case CS_CSF_FILTERED_TOPK_EXACT: return LIBCS_PIND_FILTERED_TOPK_EXACT;
	case CS_CSF_RLE_COMPRESS:      return LIBCS_PIND_RLE_COMPRESS;
	case CS_CSF_MULTI_AGG64:       return LIBCS_PIND_MULTI_AGG64;
	case CS_CSF_L2_DISTANCE_SQ:    return LIBCS_PIND_L2_DISTANCE_SQ;
	case CS_CSF_COSINE_SIMILARITY: return LIBCS_PIND_COSINE_SIMILARITY;
	default:                       return (uint16_t)csf_id;
	}
}

static inline const char *
cs_prog_name_from_pind(uint16_t pind)
{
	switch (pind) {
	case LIBCS_PIND_MEMCPY:            return "MEMCPY";
	case LIBCS_PIND_MEMFILL:           return "MEMFILL";
	case LIBCS_PIND_SUM64:             return "SUM64";
	case LIBCS_PIND_MAX64:             return "MAX64";
	case LIBCS_PIND_MIN64:             return "MIN64";
	case LIBCS_PIND_FILTER_AGG:        return "FILTER_AGG";
	case LIBCS_PIND_FILTERED_TOPK_EXACT: return "FILTERED_TOPK_EXACT";
	case LIBCS_PIND_DOT_PRODUCT:       return "DOT_PRODUCT";
	case LIBCS_PIND_FILTER_GT:         return "FILTER_GT";
	case LIBCS_PIND_MEMCPY_INLINE:     return "MEMCPY_INLINE";
	case LIBCS_PIND_RLE_COMPRESS:      return "RLE_COMPRESS";
	case LIBCS_PIND_MULTI_AGG64:       return "MULTI_AGG64";
	case LIBCS_PIND_L2_DISTANCE_SQ:    return "L2_DISTANCE_SQ";
	case LIBCS_PIND_COSINE_SIMILARITY: return "COSINE_SIMILARITY";
	default:                          return NULL;
	}
}

typedef struct {
	const char *name;
	csCSFId     csf_id;
	uint16_t    pind;
} cs_csf_entry_t;

static inline const cs_csf_entry_t *
cs_csf_table(uint32_t *count)
{
	static const cs_csf_entry_t table[] = {
		{"SUM64",             CS_CSF_SUM64,             LIBCS_PIND_SUM64},
		{"MAX64",             CS_CSF_MAX64,             LIBCS_PIND_MAX64},
		{"MIN64",             CS_CSF_MIN64,             LIBCS_PIND_MIN64},
		{"DOT_PRODUCT",       CS_CSF_DOT_PRODUCT,       LIBCS_PIND_DOT_PRODUCT},
		{"FILTER_GT",         CS_CSF_FILTER_GT,         LIBCS_PIND_FILTER_GT},
		{"MEMCPY",            CS_CSF_MEMCPY,            LIBCS_PIND_MEMCPY},
		{"MEMFILL",           CS_CSF_MEMFILL,           LIBCS_PIND_MEMFILL},
		{"MEMCPY_INLINE",     CS_CSF_MEMCPY_INLINE,     LIBCS_PIND_MEMCPY_INLINE},
		{"FILTER_AGG",        CS_CSF_FILTER_AGG,        LIBCS_PIND_FILTER_AGG},
		{"FILTERED_TOPK_EXACT", CS_CSF_FILTERED_TOPK_EXACT, LIBCS_PIND_FILTERED_TOPK_EXACT},
		{"RLE_COMPRESS",      CS_CSF_RLE_COMPRESS,      LIBCS_PIND_RLE_COMPRESS},
		{"MULTI_AGG64",       CS_CSF_MULTI_AGG64,       LIBCS_PIND_MULTI_AGG64},
		{"L2_DISTANCE_SQ",    CS_CSF_L2_DISTANCE_SQ,    LIBCS_PIND_L2_DISTANCE_SQ},
		{"COSINE_SIMILARITY", CS_CSF_COSINE_SIMILARITY, LIBCS_PIND_COSINE_SIMILARITY},
	};
	*count = sizeof(table) / sizeof(table[0]);
	return table;
}

#endif /* CS_PROGRAMS_H */
