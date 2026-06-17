//
//  ud_dictd.c
//  unidict
//
//  Created by kejinlu on 2026-01-12
//
#include "ud_dictd.h"
#include "unidict_internal.h"
#include "unidict_log.h"
#include "ud_udx.h"
#include "udx_writer.h"
#include "sd_dictd.h"
#include "sd_dictfile_index.h"
#include "sd_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

// ============================================================
// Index object (stores offset/size for fast query)
// ============================================================

typedef struct {
    uobject obj;
    uint32_t offset;
    uint32_t size;
} ud_dictd_index;

static void ud_dictd_index_release(uobject *obj) {
    ud_dictd_index *idx = uobject_cast(obj, ud_dictd_index, obj);
    free(idx);
}

static const uobject_type ud_dictd_index_type = {
    .name = "ud_dictd_index",
    .size = sizeof(ud_dictd_index),
    .release = ud_dictd_index_release,
};

// ============================================================
// Private struct definition
// ============================================================

typedef struct ud_dictd ud_dictd;

// One 00-database-* metadata entry (headword + definition text).
typedef struct {
    char *word;        // headword, e.g. "00-database-short"
    char *definition;  // definition text (may be NULL if not fetched)
} dictd_meta_entry;

struct ud_dictd {
    unidict base;
    char *index_path;  // stored .index path for deriving .udx path

    sd_dictd *dictd_dict;
    unidict *udx_dict; // UDX external index

    // Cached metadata from 00database* entries
    char *db_title;          // 00-database-short definition
    char *db_description;    // 00-database-info definition (+ appended url)
    bool utf8_mode;          // 00databaseutf8 present
    uint32_t db_special_count; // number of 00database* entries at head of index

    // All 00-database-* entries (for the "meta" feature page)
    dictd_meta_entry *meta_entries;
    size_t meta_count;
};

// ============================================================
// VTable
// ============================================================

static void ud_dictd_release(uobject *obj);

static unidict_status ud_dictd_info_get(unidict *dict, unidict_info **out_info);
static unidict_status ud_dictd_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status ud_dictd_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status ud_dictd_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status ud_dictd_suggest(unidict *dict, const char *prefix, size_t limit,
                                       unidict_entry_array **out_entries);
static unidict_status ud_dictd_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_status ud_dictd_index_activate(unidict *dict, unidict_index_type index_type);
static unidict_status ud_dictd_index_external_delete(unidict *dict);
static unidict_status ud_dictd_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                   void *user_data);
static unidict_entry_iter *ud_dictd_entry_iter_create(unidict *dict);
static unidict_status ud_dictd_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void ud_dictd_entry_iter_free(unidict_entry_iter *iter);
static unidict_status ud_dictd_feature_pages_list(unidict *dict, unidict_feature_page_array **out_pages);
static unidict_status ud_dictd_feature_page_read(unidict *dict, const char *key, char **out_html);

static const unidict_ops dictd_ops = {
    .prepare = NULL,
    .info_get = ud_dictd_info_get,
    .file_infos_get = ud_dictd_file_infos_get,
    .index_activate = ud_dictd_index_activate,
    .index_external_make = ud_dictd_index_external_make,
    .index_external_delete = ud_dictd_index_external_delete,
    .lookup = ud_dictd_lookup,
    .entry_lookup = ud_dictd_entry_lookup,
    .suggest = ud_dictd_suggest,
    .fetch = ud_dictd_fetch,
    .entry_iter_create = ud_dictd_entry_iter_create,
    .entry_iter_next = ud_dictd_entry_iter_next,
    .entry_iter_free = ud_dictd_entry_iter_free,
    .resource_get = NULL,
    .resource_iter_create = NULL,
    .resource_iter_next = NULL,
    .resource_iter_free = NULL,
    .feature_pages_list = ud_dictd_feature_pages_list,
    .feature_page_read = ud_dictd_feature_page_read,
};

static const uobject_type ud_dictd_type = {
    .name = "ud_dictd",
    .size = sizeof(ud_dictd),
    .release = ud_dictd_release,
};

// ============================================================
// Release
// ============================================================

static void ud_dictd_release(uobject *obj) {
    if (!obj) return;
    ud_dictd *dictd = uobject_cast(obj, ud_dictd, base.obj);

    if (dictd->udx_dict) {
        unidict_close(dictd->udx_dict);
        dictd->udx_dict = NULL;
    }

    if (dictd->dictd_dict) {
        sd_dictd_close(dictd->dictd_dict);
        dictd->dictd_dict = NULL;
    }

    free(dictd->index_path);
    free(dictd->db_title);
    free(dictd->db_description);
    for (size_t i = 0; i < dictd->meta_count; i++) {
        free(dictd->meta_entries[i].word);
        free(dictd->meta_entries[i].definition);
    }
    free(dictd->meta_entries);
    free(dictd);
}

// ============================================================
// Helpers
// ============================================================

static char *ud_dictd_get_udx_path(const char *index_path) {
    const char *ext = strrchr(index_path, '.');
    if (!ext) return NULL;
    size_t base_len = ext - index_path;
    char *udx_path = malloc(base_len + 5);
    if (!udx_path) return NULL;
    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, index_path);
    return udx_path;
}

static bool is_db_special_entry(const char *word) {
    return word && word[0] == '0' && word[1] == '0' &&
           (strncmp(word, "00-database-", 12) == 0 || strncmp(word, "00database", 10) == 0);
}

static void load_db_metadata(ud_dictd *dictd) {
    if (!dictd->dictd_dict) return;
    const sd_dictfile_index *idx = sd_dictd_get_index(dictd->dictd_dict);
    if (!idx) return;

    uint32_t total = sd_dictfile_index_get_count(idx);
    char *url = NULL;

    // First pass: count leading 00database* entries (they sit at the head).
    uint32_t meta_total = 0;
    for (uint32_t i = 0; i < total && i < 64; i++) {
        const sd_dictfile_index_entry *entry = sd_dictfile_index_get_entry((sd_dictfile_index *)idx, i);
        if (!entry || !entry->word || !is_db_special_entry(entry->word)) break;
        meta_total = i + 1;
    }
    dictd->db_special_count = meta_total;

    if (meta_total == 0) return;

    dictd->meta_entries = calloc(meta_total, sizeof(dictd_meta_entry));
    if (!dictd->meta_entries) return;  // non-fatal: feature page will be empty

    // Second pass: record each entry's word + definition.
    for (uint32_t i = 0; i < meta_total; i++) {
        const sd_dictfile_index_entry *entry = sd_dictfile_index_get_entry((sd_dictfile_index *)idx, i);
        if (!entry || !entry->word) continue;

        dictd_meta_entry *m = &dictd->meta_entries[dictd->meta_count];
        m->word = strdup(entry->word);
        if (!m->word) continue;

        // Fetch definition text for this entry.
        char *defi = NULL;
        sd_dictfile_index_entry temp = {.word = NULL, .offset = entry->offset, .size = entry->size};
        sd_dictd_fetch(dictd->dictd_dict, &temp, &defi);
        m->definition = defi;  // may be NULL

        // Keep the well-known fields for info_get / fast access.
        if (strcmp(entry->word, "00-database-short") == 0 ||
            strcmp(entry->word, "00databaseshort") == 0) {
            if (defi) { free(dictd->db_title); dictd->db_title = strdup(defi); }
        } else if (strcmp(entry->word, "00-database-info") == 0 ||
                   strcmp(entry->word, "00databaseinfo") == 0) {
            if (defi) { free(dictd->db_description); dictd->db_description = strdup(defi); }
        } else if (strcmp(entry->word, "00-database-url") == 0 ||
                   strcmp(entry->word, "00databaseurl") == 0) {
            if (defi) { free(url); url = strdup(defi); }
        } else if (strcmp(entry->word, "00databaseutf8") == 0) {
            dictd->utf8_mode = true;
        }

        dictd->meta_count++;
    }

    // Append url to description
    if (url) {
        if (dictd->db_description) {
            size_t desc_len = strlen(dictd->db_description);
            size_t url_len = strlen(url);
            char *new_desc = malloc(desc_len + 12 + url_len + 1);
            if (new_desc) {
                memcpy(new_desc, dictd->db_description, desc_len);
                memcpy(new_desc + desc_len, "\n\nSource: ", 10);
                memcpy(new_desc + desc_len + 10, url, url_len + 1);
                free(dictd->db_description);
                dictd->db_description = new_desc;
            }
        }
        free(url);
    }
}

static ud_dictd_index *ud_dictd_index_from_udx_value(const udx_value_entry_item *item) {
    if (!item || !item->data || item->size < 8) return NULL;

    ud_dictd_index *idx = calloc(1, sizeof(ud_dictd_index));
    if (!idx) return NULL;

    memcpy(&idx->offset, item->data, 4);
    memcpy(&idx->size, item->data + 4, 4);
    uobject_init(&idx->obj, &ud_dictd_index_type, NULL);

    return idx;
}

// ============================================================
// File List
// ============================================================

static unidict_status ud_dictd_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict) {
        *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    unidict_file_info_array *udx_infos = NULL;
    if (dictd->udx_dict) {
        if (dictd->udx_dict->ops->file_infos_get) dictd->udx_dict->ops->file_infos_get(dictd->udx_dict, &udx_infos);
    }

    const char *file_paths[3];
    int count = 0;

    if (dictd->dictd_dict) {
        sd_dictd_paths paths = sd_dictd_get_paths(dictd->dictd_dict);
        if (paths.index_path) file_paths[count++] = paths.index_path;
        if (paths.dict_path) file_paths[count++] = paths.dict_path;
    }
    if (udx_infos && udx_infos->count > 0) file_paths[count++] = udx_infos->items[0].path;

    *out_infos = unidict_file_infos_from_paths(file_paths, count);
    if (udx_infos) unidict_file_info_array_free(udx_infos);

    return UNIDICT_OK;
}

// ============================================================
// Index Activate
// ============================================================

static unidict_status ud_dictd_index_activate(unidict *dict, unidict_index_type index_type) {
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    if (dictd->udx_dict) {
        unidict_close(dictd->udx_dict);
        dictd->udx_dict = NULL;
    }
    if (dictd->dictd_dict) {
        sd_dictd_close(dictd->dictd_dict);
        dictd->dictd_dict = NULL;
    }
    dict->active_index = UNIDICT_INDEX_NONE;

    const char *index_path = dictd->index_path;
    if (!index_path) return UNIDICT_ERR_INTERNAL;

    // EXTERNAL or NONE: try UDX first
    if (index_type == UNIDICT_INDEX_EXTERNAL || index_type == UNIDICT_INDEX_NONE) {
        char *udx_path = ud_dictd_get_udx_path(index_path);
        if (udx_path) {
            unidict *udx_dict = ud_udx_open(udx_path, NULL);
            free(udx_path);

            if (udx_dict) {
                // Still need sd for fetch (reads from .dict file), skip .index
                sd_dictd *dictd_dict = NULL;
                sd_status st = sd_dictd_open(index_path, true, &dictd_dict);
                if (st != SD_OK || !dictd_dict) {
                    unidict_close(udx_dict);
                    // Fall through to builtin mode
                } else {
                    dictd->udx_dict = udx_dict;
                    dictd->dictd_dict = dictd_dict;
                    dict->active_index = UNIDICT_INDEX_EXTERNAL;
                    return UNIDICT_OK;
                }
            }
        }
    }

    // BUILTIN or NONE fallback: open sd_dictd with full index
    sd_dictd *dictd_dict = NULL;
    sd_status st = sd_dictd_open(index_path, false, &dictd_dict);
    if (st == SD_OK && dictd_dict) {
        dictd->dictd_dict = dictd_dict;
        dict->active_index = UNIDICT_INDEX_BUILTIN;
        load_db_metadata(dictd);
        return UNIDICT_OK;
    }

    return UNIDICT_ERR_IO;
}

// ============================================================
// Feature pages: "meta" lists every 00-database-* metadata entry
// ============================================================

static unidict_status ud_dictd_feature_pages_list(unidict *dict, unidict_feature_page_array **out_pages) {
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    unidict_feature_page_array *arr = calloc(1, sizeof(*arr));
    if (!arr) return UNIDICT_ERR_NOMEM;

    // The "meta" page only makes sense if there are 00-database-* entries.
    if (dictd->meta_count > 0) {
        unidict_feature_page *items = calloc(1, sizeof(*items));
        if (!items) { free(arr); return UNIDICT_ERR_NOMEM; }
        items[0].key = strdup("meta");
        items[0].name = strdup("Database Metadata");
        arr->items = items;
        arr->count = 1;
    }

    *out_pages = arr;
    return UNIDICT_OK;
}

// Minimal HTML escaping into a malloc'd buffer; appends the escaped form.
static void meta_html_append(char **buf, size_t *len, size_t *cap, const char *text) {
    if (!text) return;
    for (const char *p = text; *p; p++) {
        const char *entity = NULL;
        if (*p == '&') entity = "&amp;";
        else if (*p == '<') entity = "&lt;";
        else if (*p == '>') entity = "&gt;";
        else if (*p == '"') entity = "&quot;";

        size_t need = entity ? strlen(entity) : 1;
        if (*len + need + 1 > *cap) {
            while (*len + need + 1 > *cap) *cap = *cap ? *cap * 2 : 128;
            char *nb = realloc(*buf, *cap);
            if (!nb) return;  // best-effort; skip on OOM
            *buf = nb;
        }
        if (entity) { memcpy(*buf + *len, entity, need); *len += need; }
        else { (*buf)[*len++] = *p; }
    }
}

static unidict_status ud_dictd_feature_page_read(unidict *dict, const char *key, char **out_html) {
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    // Only "meta" is supported; ignore any query suffix.
    size_t base_len = strlen(key);
    const char *query = strchr(key, '?');
    if (query) base_len = (size_t)(query - key);
    if (base_len != 4 || strncmp(key, "meta", 4) != 0) {
        return UNIDICT_ERR_NOT_FOUND;
    }

    if (dictd->meta_count == 0) return UNIDICT_ERR_NOT_FOUND;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    // Open table
    const char *prefix = "<!DOCTYPE html>\n<html>\n<head><meta charset=\"utf-8\">\n"
                         "<title>Database Metadata</title></head>\n<body>\n"
                         "<h1>Database Metadata</h1>\n<table border=\"1\" cellpadding=\"4\">\n"
                         "<tr><th>Entry</th><th>Value</th></tr>\n";
    meta_html_append(&buf, &len, &cap, prefix);

    for (size_t i = 0; i < dictd->meta_count; i++) {
        meta_html_append(&buf, &len, &cap, "<tr><td>");
        meta_html_append(&buf, &len, &cap, dictd->meta_entries[i].word);
        meta_html_append(&buf, &len, &cap, "</td><td><pre>");
        meta_html_append(&buf, &len, &cap, dictd->meta_entries[i].definition);
        meta_html_append(&buf, &len, &cap, "</pre></td></tr>\n");
    }

    meta_html_append(&buf, &len, &cap, "</table>\n</body>\n</html>\n");

    if (!buf) return UNIDICT_ERR_NOMEM;
    // Ensure null-termination.
    if (len + 1 > cap) {
        char *nb = realloc(buf, len + 1);
        if (!nb) { free(buf); return UNIDICT_ERR_NOMEM; }
        buf = nb;
    }
    buf[len] = '\0';
    *out_html = buf;
    return UNIDICT_OK;
}

// ============================================================
// Constructor
// ============================================================

unidict *ud_dictd_open(const char *index_path, const unidict_open_options *options) {
    if (!index_path) return NULL;

    ud_dictd *dictd = calloc(1, sizeof(ud_dictd));
    if (!dictd) return NULL;

    uobject_init(&dictd->base.obj, &ud_dictd_type, NULL);
    dictd->base.ops = &dictd_ops;
    dictd->base.format = UNIDICT_FORMAT_DICTD;
    dictd->index_path = strdup(index_path);
    if (!dictd->index_path) {
        ud_dictd_release((uobject *)dictd);
        return NULL;
    }

    dictd->base.has_builtin_index = true;
    dictd->base.has_external_index = unidict_detect_external_index(index_path);

    unidict_index_type preset =
        (options && options->index_type != UNIDICT_INDEX_NONE) ? options->index_type : UNIDICT_INDEX_NONE;

    if (ud_dictd_index_activate(&dictd->base, preset) != UNIDICT_OK) {
        ud_dictd_release((uobject *)dictd);
        return NULL;
    }

    return &dictd->base;
}

// ============================================================
// Index External Delete
// ============================================================

static unidict_status ud_dictd_index_external_delete(unidict *dict) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    // Switch to builtin mode (closes udx_dict)
    unidict_status st = ud_dictd_index_activate(dict, UNIDICT_INDEX_BUILTIN);
    if (st != UNIDICT_OK) return st;

    // Delete .udx file
    char *udx_path = ud_dictd_get_udx_path(dictd->index_path);
    if (!udx_path) return UNIDICT_ERR_INTERNAL;

    if (remove(udx_path) != 0 && errno != ENOENT) {
        free(udx_path);
        return UNIDICT_ERR_IO;
    }
    free(udx_path);

    dict->has_external_index = false;
    return UNIDICT_OK;
}

// ============================================================
// Index External Make
// ============================================================

static unidict_status ud_dictd_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                   void *user_data) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;

    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    // Ensure sd is open for iterating .index entries (need full index here)
    if (!dictd->dictd_dict || !sd_dictd_get_index(dictd->dictd_dict)) {
        if (dictd->dictd_dict) {
            sd_dictd_close(dictd->dictd_dict);
            dictd->dictd_dict = NULL;
        }
        if (!dictd->index_path) return UNIDICT_ERR_NO_INDEX;
        sd_dictd *dictd_dict = NULL;
        sd_status st = sd_dictd_open(dictd->index_path, false, &dictd_dict);
        if (st != SD_OK || !dictd_dict) return UNIDICT_ERR_NO_INDEX;
        dictd->dictd_dict = dictd_dict;
        load_db_metadata(dictd);
    }

    const sd_dictfile_index *idx = sd_dictd_get_index(dictd->dictd_dict);
    if (!idx) return UNIDICT_ERR_NO_INDEX;

    uint32_t total = sd_dictfile_index_get_count(idx);
    if (total == 0) return UNIDICT_ERR_NO_INDEX;

    char *udx_path = ud_dictd_get_udx_path(dictd->index_path);
    if (!udx_path) return UNIDICT_ERR_NOMEM;

    udx_writer *writer = udx_writer_open(udx_path);
    if (!writer) {
        free(udx_path);
        return UNIDICT_ERR_IO;
    }

    unidict_status ret = UNIDICT_ERR_INTERNAL;

    // Build info metadata
    unidict_info meta = {0};
    meta.title = dictd->db_title;
    meta.description = dictd->db_description;
    meta.source_lang = NULL;
    meta.target_lang = NULL;
    char *meta_xml = unidict_info_to_xml(&meta);

    udx_db_builder *builder;
    if (meta_xml) {
        builder = udx_db_builder_create_with_metadata(writer, "article",
                    (const uint8_t *)meta_xml, (uint32_t)strlen(meta_xml));
        free(meta_xml);
    } else {
        builder = udx_db_builder_create(writer, "article");
    }
    if (!builder) {
        udx_writer_close(writer);
        goto fail;
    }

    int last_pct = 0;
    for (uint32_t i = 0; i < total; i++) {
        const sd_dictfile_index_entry *entry = sd_dictfile_index_get_entry((sd_dictfile_index *)idx, i);
        if (!entry || !entry->word) continue;
        if (is_db_special_entry(entry->word)) continue;

        // Pack offset(4) + size(4) = 8 bytes
        uint8_t value[8];
        memcpy(value, &entry->offset, 4);
        memcpy(value + 4, &entry->size, 4);

        udx_value_address address = udx_db_builder_add_value(builder, value, 8);
        if (address == UDX_INVALID_ADDRESS) continue;

        udx_db_builder_add_key_entry(builder, entry->word, address, 8);

        if (callback) {
            int pct = (int)((uint64_t)(i + 1) * 100 / total);
            if (pct > last_pct) {
                last_pct = pct;
                if (!callback(dict, UNIDICT_INDEX_STAGE_ARTICLES, pct, user_data)) {
                    udx_db_builder_finalize(builder);
                    udx_writer_close(writer);
                    ret = UNIDICT_ERR_CANCELLED;
                    goto fail;
                }
            }
        }
    }

    udx_status err = udx_db_builder_finalize(builder);
    if (err != UDX_OK) {
        udx_writer_close(writer);
        goto fail;
    }

    err = udx_writer_close(writer);
    if (err != UDX_OK) goto fail;

    free(udx_path);
    dict->has_external_index = true;
    return UNIDICT_OK;

fail:
    remove(udx_path);
    free(udx_path);
    return ret;
}

// ============================================================
// Query implementation
// ============================================================

static unidict_status ud_dictd_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    // External index mode
    if (dictd->udx_dict) {
        udx_db_value_entry *ve = ud_udx_raw_lookup(dictd->udx_dict, key);
        if (!ve || ve->items.count == 0) {
            if (ve) udx_db_value_entry_free(ve);
            *out_entries = NULL;
            return UNIDICT_OK;
        }

        unidict_entry_array *entries = malloc(sizeof(unidict_entry_array));
        if (!entries) {
            udx_db_value_entry_free(ve);
            *out_entries = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        entries->count = ve->items.count;
        entries->items = calloc(ve->items.count, sizeof(unidict_entry *));
        if (!entries->items) {
            free(entries);
            udx_db_value_entry_free(ve);
            *out_entries = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        for (size_t i = 0; i < ve->items.count; i++) {
            udx_value_entry_item *item = &ve->items.elements[i];
            ud_dictd_index *idx = ud_dictd_index_from_udx_value(item);
            if (!idx) continue;

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (!entry) {
                uobject_release(&idx->obj);
                continue;
            }

            entry->key = item->original_key ? strdup(item->original_key) : strdup(key);
            entry->internal_entry = &idx->obj;
            entries->items[i] = entry;
        }

        udx_db_value_entry_free(ve);
        *out_entries = entries;
        return UNIDICT_OK;
    }

    // Builtin mode
    sd_index_entry_array *sd_result = NULL;
    sd_status st = sd_dictd_entry_lookup(dictd->dictd_dict, key, &sd_result);
    if (st == SD_NOT_FOUND) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }
    if (st != SD_OK) {
        *out_entries = NULL;
        return UNIDICT_ERR_INTERNAL;
    }

    unidict_entry_array *entries = malloc(sizeof(unidict_entry_array));
    if (!entries) {
        sd_index_entry_array_free(sd_result);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entries->count = sd_result->count;
    entries->items = calloc(sd_result->count, sizeof(unidict_entry *));
    if (!entries->items) {
        free(entries);
        sd_index_entry_array_free(sd_result);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    for (size_t i = 0; i < sd_result->count; i++) {
        sd_dictfile_index_entry *index_entry = sd_result->items[i];
        if (!index_entry) continue;
        if (is_db_special_entry(index_entry->word)) continue;

        ud_dictd_index *idx = calloc(1, sizeof(ud_dictd_index));
        if (!idx) continue;
        idx->offset = index_entry->offset;
        idx->size = index_entry->size;
        uobject_init(&idx->obj, &ud_dictd_index_type, NULL);

        unidict_entry *entry = calloc(1, sizeof(unidict_entry));
        if (!entry) {
            uobject_release(&idx->obj);
            continue;
        }

        entry->key = index_entry->word;
        index_entry->word = NULL;
        entry->internal_entry = &idx->obj;
        entries->items[i] = entry;
    }

    sd_index_entry_array_free(sd_result);
    *out_entries = entries;
    return UNIDICT_OK;
}

static unidict_status ud_dictd_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    // External index mode: entry_lookup + fetch
    if (dictd->udx_dict) {
        unidict_entry_array *entries = NULL;
        unidict_status st = ud_dictd_entry_lookup(dict, key, &entries);
        if (st != UNIDICT_OK || !entries || entries->count == 0) {
            if (entries) unidict_entry_array_free(entries);
            *out_articles = NULL;
            return st == UNIDICT_OK ? UNIDICT_OK : st;
        }

        unidict_article_array *res = malloc(sizeof(unidict_article_array));
        if (!res) {
            unidict_entry_array_free(entries);
            *out_articles = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        res->count = entries->count;
        res->items = calloc(entries->count, sizeof(unidict_article));
        if (!res->items) {
            free(res);
            unidict_entry_array_free(entries);
            *out_articles = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        for (size_t i = 0; i < entries->count; i++) {
            unidict_article_array *single = NULL;
            ud_dictd_fetch(dict, entries->items[i], &single);
            if (single && single->count > 0) {
                res->items[i].title = entries->items[i]->key ? strdup(entries->items[i]->key) : NULL;
                res->items[i].body = single->items[0].body;
                single->items[0].body = NULL;
            }
            if (single) unidict_article_array_free(single);
        }

        unidict_entry_array_free(entries);
        *out_articles = res;
        return UNIDICT_OK;
    }

    // Builtin mode
    sd_data_entry_array *sd_result = NULL;
    sd_status st = sd_dictd_lookup(dictd->dictd_dict, key, &sd_result);
    if (st == SD_NOT_FOUND) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }
    if (st != SD_OK) {
        *out_articles = NULL;
        return UNIDICT_ERR_INTERNAL;
    }

    unidict_article_array *res = malloc(sizeof(unidict_article_array));
    if (!res) {
        sd_data_entry_array_free(sd_result);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->count = sd_result->count;
    res->items = calloc(sd_result->count, sizeof(unidict_article));
    if (!res->items) {
        free(res);
        sd_data_entry_array_free(sd_result);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    for (size_t i = 0; i < sd_result->count; i++) {
        res->items[i].title = sd_result->items[i].word;
        sd_result->items[i].word = NULL;
        res->items[i].body = sd_result->items[i].definition;
        sd_result->items[i].definition = NULL;
    }

    sd_data_entry_array_free(sd_result);
    *out_articles = res;
    return UNIDICT_OK;
}

static unidict_status ud_dictd_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict) {
        *out_info = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    // External index mode: delegate to UDX info_get (metadata stored in UDX)
    if (dictd->udx_dict && dictd->udx_dict->ops->info_get) {
        unidict_status st = dictd->udx_dict->ops->info_get(dictd->udx_dict, out_info);
        if (st == UNIDICT_OK && *out_info) {
            (*out_info)->format = dict->format;
        }
        return st;
    }

    // Builtin mode: use cached metadata from 00database* entries
    const sd_dictfile_index *idx = dictd->dictd_dict ? sd_dictd_get_index(dictd->dictd_dict) : NULL;

    unidict_info *info = calloc(1, sizeof(unidict_info));
    if (!info) {
        *out_info = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    info->format = dict->format;
    info->title = dictd->db_title ? strdup(dictd->db_title) : strdup("dictd dictionary");
    info->description = dictd->db_description ? strdup(dictd->db_description) : NULL;
    info->author = NULL;
    info->creation_date = NULL;
    info->source_lang = NULL;
    info->target_lang = NULL;

    uint32_t total = idx ? sd_dictfile_index_get_count(idx) : 0;
    info->word_count = (total > dictd->db_special_count) ? (uint64_t)(total - dictd->db_special_count) : 0;

    *out_info = info;
    return UNIDICT_OK;
}

static unidict_status ud_dictd_suggest(unidict *dict, const char *prefix, size_t limit,
                                       unidict_entry_array **out_entries) {
    if (!dict || !prefix) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    // External index mode
    if (dictd->udx_dict) {
        unidict_entry_array *udx_entries = NULL;
        if (!dictd->udx_dict->ops->suggest) {
            *out_entries = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_status st = dictd->udx_dict->ops->suggest(dictd->udx_dict, prefix, limit, &udx_entries);
        if (st != UNIDICT_OK || !udx_entries) {
            *out_entries = NULL;
            return st;
        }

        unidict_entry_array *entries = malloc(sizeof(unidict_entry_array));
        if (!entries) {
            unidict_entry_array_free(udx_entries);
            *out_entries = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        entries->count = udx_entries->count;
        entries->items = calloc(udx_entries->count, sizeof(unidict_entry *));
        if (!entries->items) {
            free(entries);
            unidict_entry_array_free(udx_entries);
            *out_entries = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        for (size_t i = 0; i < udx_entries->count; i++) {
            unidict_entry *udx_entry = udx_entries->items[i];
            if (!udx_entry) continue;

            udx_db_value_entry *ve = ud_udx_raw_fetch(dictd->udx_dict, udx_entry);
            if (!ve || ve->items.count == 0) {
                if (ve) udx_db_value_entry_free(ve);
                continue;
            }

            ud_dictd_index *idx = ud_dictd_index_from_udx_value(&ve->items.elements[0]);
            udx_db_value_entry_free(ve);
            if (!idx) continue;

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (!entry) {
                uobject_release(&idx->obj);
                continue;
            }

            entry->key = strdup(udx_entry->key);
            entry->internal_entry = &idx->obj;
            entries->items[i] = entry;
        }

        unidict_entry_array_free(udx_entries);
        *out_entries = entries;
        return UNIDICT_OK;
    }

    // Builtin mode
    sd_index_entry_array *index_entries = NULL;
    sd_status st = sd_dictd_suggest(dictd->dictd_dict, prefix, limit, &index_entries);
    if (st == SD_NOT_FOUND) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }
    if (st != SD_OK) {
        *out_entries = NULL;
        return UNIDICT_ERR_INTERNAL;
    }

    unidict_entry_array *entries = malloc(sizeof(unidict_entry_array));
    if (!entries) {
        sd_index_entry_array_free(index_entries);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entries->count = index_entries->count;
    entries->items = calloc(index_entries->count, sizeof(unidict_entry *));
    if (!entries->items) {
        free(entries);
        sd_index_entry_array_free(index_entries);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    for (size_t i = 0; i < index_entries->count; i++) {
        sd_dictfile_index_entry *index_entry = index_entries->items[i];
        if (!index_entry) continue;
        if (is_db_special_entry(index_entry->word)) continue;

        ud_dictd_index *idx = calloc(1, sizeof(ud_dictd_index));
        if (!idx) continue;

        idx->offset = index_entry->offset;
        idx->size = index_entry->size;
        uobject_init(&idx->obj, &ud_dictd_index_type, NULL);

        unidict_entry *entry = calloc(1, sizeof(unidict_entry));
        if (!entry) {
            uobject_release(&idx->obj);
            continue;
        }

        entry->key = strdup(index_entry->word);
        entry->internal_entry = &idx->obj;
        entries->items[i] = entry;
    }

    sd_index_entry_array_free(index_entries);
    *out_entries = entries;
    return UNIDICT_OK;
}

// ============================================================
// Entry iterator
// ============================================================

typedef struct {
    unidict_entry_iter base;
    unidict_entry_iter *udx_iter;
    uint32_t pos;
    uint32_t count;
} ud_dictd_entry_iter;

static unidict_entry_iter *ud_dictd_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);

    ud_dictd_entry_iter *iter = calloc(1, sizeof(ud_dictd_entry_iter));
    if (!iter) return NULL;

    iter->base.dict = dict;

    if (dictd->udx_dict) {
        if (!dictd->udx_dict->ops->entry_iter_create) {
            free(iter);
            return NULL;
        }
        iter->udx_iter = dictd->udx_dict->ops->entry_iter_create(dictd->udx_dict);
        if (!iter->udx_iter) {
            free(iter);
            return NULL;
        }
    } else {
        const sd_dictfile_index *idx = dictd->dictd_dict ? sd_dictd_get_index(dictd->dictd_dict) : NULL;
        iter->count = idx ? sd_dictfile_index_get_count(idx) : 0;
    }

    return (unidict_entry_iter *)iter;
}

static unidict_status ud_dictd_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict) {
        if (out_entry) *out_entry = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_dictd_entry_iter *dictd_iter = (ud_dictd_entry_iter *)iter;
    ud_dictd *dictd = uobject_cast(&iter->dict->obj, ud_dictd, base.obj);

    free(iter->current.key);
    iter->current.key = NULL;

    if (dictd_iter->udx_iter) {
        unidict_entry *udx_entry = NULL;
        if (!dictd->udx_dict->ops->entry_iter_next) {
            *out_entry = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_status st = dictd->udx_dict->ops->entry_iter_next(dictd_iter->udx_iter, &udx_entry);
        if (st != UNIDICT_OK || !udx_entry) {
            *out_entry = NULL;
            return UNIDICT_DONE;
        }

        // Fetch UDX value to get offset/size
        udx_db_value_entry *ve = ud_udx_raw_fetch(dictd->udx_dict, udx_entry);
        if (!ve || ve->items.count == 0) {
            if (ve) udx_db_value_entry_free(ve);
            *out_entry = NULL;
            return UNIDICT_ERR_INTERNAL;
        }

        ud_dictd_index *idx = ud_dictd_index_from_udx_value(&ve->items.elements[0]);
        udx_db_value_entry_free(ve);
        if (!idx) {
            *out_entry = NULL;
            return UNIDICT_ERR_INTERNAL;
        }

        // Free previous internal_entry
        if (iter->current.internal_entry) {
            uobject_release(iter->current.internal_entry);
            iter->current.internal_entry = NULL;
        }

        iter->current.key = strdup(udx_entry->key);
        iter->current.internal_entry = &idx->obj;

        if (!iter->current.key) {
            uobject_release(&idx->obj);
            *out_entry = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        *out_entry = &iter->current;
        return UNIDICT_OK;
    }

    // Builtin mode: iterate sd_dictfile_index
    const sd_dictfile_index *sd_idx = sd_dictd_get_index(dictd->dictd_dict);

    while (dictd_iter->pos < dictd_iter->count) {
        const sd_dictfile_index_entry *entry = sd_dictfile_index_get_entry((sd_dictfile_index *)sd_idx, dictd_iter->pos);
        dictd_iter->pos++;
        if (entry && entry->word && !is_db_special_entry(entry->word)) {
            // Valid non-special entry, process below
            ud_dictd_index *idx = calloc(1, sizeof(ud_dictd_index));
            if (!idx) {
                *out_entry = NULL;
                return UNIDICT_ERR_NOMEM;
            }
            idx->offset = entry->offset;
            idx->size = entry->size;
            uobject_init(&idx->obj, &ud_dictd_index_type, NULL);

            if (iter->current.internal_entry) {
                uobject_release(iter->current.internal_entry);
            }

            iter->current.key = strdup(entry->word);
            iter->current.internal_entry = &idx->obj;

            if (!iter->current.key) {
                uobject_release(&idx->obj);
                *out_entry = NULL;
                return UNIDICT_ERR_NOMEM;
            }

            *out_entry = &iter->current;
            return UNIDICT_OK;
        }
    }

    *out_entry = NULL;
    return UNIDICT_DONE;
}

static void ud_dictd_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;
    ud_dictd_entry_iter *dictd_iter = (ud_dictd_entry_iter *)iter;
    free(iter->current.key);
    if (iter->current.internal_entry) {
        uobject_release(iter->current.internal_entry);
    }
    if (dictd_iter->udx_iter) {
        ud_dictd *dictd = uobject_cast(&iter->dict->obj, ud_dictd, base.obj);
        if (dictd->udx_dict->ops->entry_iter_free) dictd->udx_dict->ops->entry_iter_free(dictd_iter->udx_iter);
    }
    free(iter);
}

static unidict_status ud_dictd_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_dictd *dictd = uobject_cast(&dict->obj, ud_dictd, base.obj);
    ud_dictd_index *idx = uobject_cast(entry->internal_entry, ud_dictd_index, obj);

    sd_dictfile_index_entry temp = {.word = NULL, .offset = idx->offset, .size = idx->size};

    char *body = NULL;
    sd_status st = sd_dictd_fetch(dictd->dictd_dict, &temp, &body);
    if (st == SD_NOT_FOUND) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }
    if (st != SD_OK) {
        *out_articles = NULL;
        return UNIDICT_ERR_INTERNAL;
    }

    unidict_article_array *res = malloc(sizeof(unidict_article_array));
    if (!res) {
        free(body);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->items = calloc(1, sizeof(unidict_article));
    if (!res->items) {
        free(res);
        free(body);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->items[0].title = NULL;
    res->items[0].body = body;
    res->count = 1;
    *out_articles = res;
    return UNIDICT_OK;
}
