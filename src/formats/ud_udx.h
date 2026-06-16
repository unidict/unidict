//
//  ud_udx.h
//  unidict
//
//  Created by kejinlu on 2026-05-22
//
#ifndef ud_udx_h
#define ud_udx_h

#include "unidict.h"
#include "udx_types.h"

unidict *ud_udx_open(const char *file_path, const unidict_open_options *options);

// ============================================================
// Raw data access (for composing backends)
// Returns UDX native value entries; caller frees with
// udx_db_value_entry_free().
// ============================================================

udx_db_value_entry *ud_udx_raw_lookup(unidict *dict, const char *key);
udx_db_value_entry *ud_udx_raw_fetch(unidict *dict, const unidict_entry *entry);
udx_db_value_entry *ud_udx_raw_resource_get(unidict *dict, const char *key);

// ============================================================
// Raw resource iterator
// ============================================================

typedef struct ud_udx_raw_res_iter ud_udx_raw_res_iter;

ud_udx_raw_res_iter *ud_udx_raw_res_iter_create(unidict *dict);
udx_db_value_entry *ud_udx_raw_res_iter_next(ud_udx_raw_res_iter *iter);
void ud_udx_raw_res_iter_free(ud_udx_raw_res_iter *iter);

#endif /* ud_udx_h */
