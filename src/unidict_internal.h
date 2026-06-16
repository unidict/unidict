//
//  unidict_internal.h
//  unidict
//
//  Created by kejinlu on 2026-01-01
//
#ifndef unidict_internal_h
#define unidict_internal_h

#include "unidict.h"
#include "uobject.h"

// ============================================================
// VTable (interface definition)
// ============================================================
typedef struct unidict_ops {
    // Lifecycle (may be NULL, meaning no preload needed)
    unidict_status (*prepare)(unidict *dict);

    // Info (may be NULL)
    unidict_status (*info_get)(unidict *dict, unidict_info **out_info);
    // Original dictionary file is always included; external index file only when activated
    unidict_status (*file_infos_get)(unidict *dict, unidict_file_info_array **out_infos);

    // Index (may be NULL)
    unidict_status (*index_activate)(unidict *dict, unidict_index_type index_type);
    unidict_status (*index_external_make)(unidict *dict, unidict_index_external_make_cb callback, void *user_data);
    unidict_status (*index_external_delete)(unidict *dict);

    // Article (required: lookup)
    unidict_status (*lookup)(unidict *dict, const char *key, unidict_article_array **out_articles);
    unidict_status (*entry_lookup)(unidict *dict, const char *key, unidict_entry_array **out_entries);
    unidict_status (*suggest)(unidict *dict, const char *prefix, size_t limit, unidict_entry_array **out_entries);
    unidict_status (*fetch)(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
    unidict_entry_iter *(*entry_iter_create)(unidict *dict);
    unidict_status (*entry_iter_next)(unidict_entry_iter *iter, unidict_entry **out_entry);
    void (*entry_iter_free)(unidict_entry_iter *iter);

    // Resource (may be NULL)
    unidict_status (*resource_get)(unidict *dict, const char *key, unidict_resource **out_res);
    unidict_resource_iter *(*resource_iter_create)(unidict *dict, unidict_resource_iter_mode mode);
    unidict_status (*resource_iter_next)(unidict_resource_iter *iter, unidict_resource **out_res);
    void (*resource_iter_free)(unidict_resource_iter *iter);

    // Feature pages (may be NULL)
    unidict_status (*feature_pages_list)(unidict *dict, unidict_feature_page_array **out_pages);
    unidict_status (*feature_page_read)(unidict *dict, const char *key, char **out_html);

} unidict_ops;

// ============================================================
// unidict base struct (full definition)
// ============================================================
struct unidict {
    uobject obj;                   // Must be first member (uobject base)
    const struct unidict_ops *ops; // Virtual function table
    unidict_format format; // Dictionary format identifier

    // Index state
    bool has_builtin_index;          // Has built-in index
    bool has_external_index;         // Has external index
    unidict_index_type active_index; // Actually active index (set by index_activate)

    bool prepared; // Preload completed
};

// ============================================================
// Iterator base structs (full definition)
// ============================================================
struct unidict_entry_iter {
    unidict *dict;
    unidict_entry current;
};

struct unidict_resource_iter {
    unidict *dict;
    unidict_resource current;
};

// Internal helper to create a file list from path strings
unidict_file_info_array *unidict_file_infos_from_paths(const char **paths, int count);

// Detect whether an external UDX index file exists for the given source path
bool unidict_detect_external_index(const char *source_path);

#endif /* unidict_internal_h */
