//
//  ud_lingvo.c
//  unidict
//
//  Created by kejinlu on 2026-01-15
//
#include "ud_lingvo.h"
#include "unidict_internal.h"
#include "lsd_reader.h"
#include "lsd_utils.h"
#include "udx_writer.h"
#include "ud_udx.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

// ============================================================
// Struct definition
// ============================================================

typedef struct ud_lingvo ud_lingvo;

struct ud_lingvo {
    unidict base;
    char *lsd_path;
    lsd_reader *reader;
    unidict *udx_dict; // UDX external index
};

// ============================================================
// Entry reference struct (Lingvo implementation)
// ============================================================

typedef struct {
    uobject obj;
    uint32_t reference;
} ud_lingvo_index;

static void ud_lingvo_index_release(uobject *obj) {
    ud_lingvo_index *idx = uobject_cast(obj, ud_lingvo_index, obj);
    free(idx);
}

static const uobject_type ud_lingvo_index_type = {
    .name = "ud_lingvo_index",
    .size = sizeof(ud_lingvo_index),
    .release = ud_lingvo_index_release,
};

// ============================================================
// Iterator structs
// ============================================================

typedef struct {
    unidict_entry_iter base;
    lsd_heading_iter *lsd_iter;
    unidict_entry_iter *udx_iter; // delegated UDX iterator
    ud_lingvo_index current_idx;
} ud_lingvo_entry_iter;

typedef struct {
    unidict_resource_iter base;
    size_t pos;
    size_t count;
    unidict_resource_iter_mode mode;
} ud_lingvo_resource_iter;

// ============================================================
// Virtual function table
// ============================================================

static void ud_lingvo_release(uobject *obj);

static unidict_status lingvo_info_get(unidict *dict, unidict_info **out_info);
static unidict_status lingvo_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status lingvo_index_activate(unidict *dict, unidict_index_type index_type);
static unidict_status lingvo_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                 void *user_data);
static unidict_status lingvo_index_external_delete(unidict *dict);
static unidict_status lingvo_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status lingvo_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status lingvo_suggest(unidict *dict, const char *prefix, size_t limit,
                                     unidict_entry_array **out_entries);
static unidict_status lingvo_lookup_by_entry(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_entry_iter *lingvo_entry_iter_create(unidict *dict);
static unidict_status lingvo_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void lingvo_entry_iter_free(unidict_entry_iter *iter);
static unidict_status lingvo_resource_get(unidict *dict, const char *key, unidict_resource **out_res);
static unidict_resource_iter *lingvo_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode);
static unidict_status lingvo_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res);
static void lingvo_resource_iter_free(unidict_resource_iter *iter);

static const unidict_ops lingvo_ops = {
    .prepare = NULL,
    .info_get = lingvo_info_get,
    .file_infos_get = lingvo_file_infos_get,
    .index_activate = lingvo_index_activate,
    .index_external_make = lingvo_index_external_make,
    .index_external_delete = lingvo_index_external_delete,
    .lookup = lingvo_lookup,
    .entry_lookup = lingvo_entry_lookup,
    .suggest = lingvo_suggest,
    .fetch = lingvo_lookup_by_entry,
    .entry_iter_create = lingvo_entry_iter_create,
    .entry_iter_next = lingvo_entry_iter_next,
    .entry_iter_free = lingvo_entry_iter_free,
    .resource_get = lingvo_resource_get,
    .resource_iter_create = lingvo_resource_iter_create,
    .resource_iter_next = lingvo_resource_iter_next,
    .resource_iter_free = lingvo_resource_iter_free,
};

static const uobject_type ud_lingvo_type = {
    .name = "ud_lingvo",
    .size = sizeof(ud_lingvo),
    .release = ud_lingvo_release,
};

// ============================================================
// Release destructor
// ============================================================

static void ud_lingvo_release(uobject *obj) {
    if (!obj) return;

    ud_lingvo *lingvo = uobject_cast(obj, ud_lingvo, base.obj);

    if (lingvo->udx_dict) {
        unidict_close(lingvo->udx_dict);
        lingvo->udx_dict = NULL;
    }

    if (lingvo->reader) {
        lsd_reader_close(lingvo->reader);
        lingvo->reader = NULL;
    }

    free(lingvo->lsd_path);
    free(lingvo);
}

// ============================================================
// Constructor
// ============================================================

unidict *ud_lingvo_open(const char *lsd_path, const unidict_open_options *options) {
    if (!lsd_path) return NULL;

    ud_lingvo *lingvo = calloc(1, sizeof(ud_lingvo));
    if (!lingvo) return NULL;

    uobject_init(&lingvo->base.obj, &ud_lingvo_type, NULL);
    lingvo->base.ops = &lingvo_ops;
    lingvo->base.format = UNIDICT_FORMAT_LINGVO;

    lingvo->lsd_path = strdup(lsd_path);
    if (!lingvo->lsd_path) {
        free(lingvo);
        return NULL;
    }

    lsd_status st = lsd_reader_open(lsd_path, &lingvo->reader);
    if (st != LSD_OK) {
        free(lingvo->lsd_path);
        free(lingvo);
        return NULL;
    }

    return &lingvo->base;
}

// ============================================================
// Index helpers
// ============================================================

static char *lingvo_get_udx_path(const char *lsd_path) {
    size_t len = strlen(lsd_path);
    size_t base_len;

    if (len >= 4 && strcasecmp(lsd_path + len - 4, ".lsd") == 0) {
        base_len = len - 4;
    } else {
        return NULL;
    }

    char *udx_path = malloc(base_len + 5);
    if (!udx_path) return NULL;
    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, lsd_path);
    return udx_path;
}

// Extract uint32_t reference from UDX value entry item
static uint32_t lingvo_ref_from_value_item(const udx_value_entry_item *item) {
    if (!item || !item->data || item->size < 4) return 0;
    uint32_t ref = 0;
    memcpy(&ref, item->data, 4);
    return ref;
}

// ============================================================
// Info
// ============================================================

static unidict_status lingvo_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict) {
        *out_info = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    // External index mode: delegate to UDX info_get (metadata stored in UDX)
    if (lingvo->udx_dict && lingvo->udx_dict->ops->info_get) {
        unidict_status st = lingvo->udx_dict->ops->info_get(lingvo->udx_dict, out_info);
        if (st == UNIDICT_OK && *out_info) {
            (*out_info)->format = dict->format;
            // Fill version from LSD header (not stored in UDX metadata)
            if (!(*out_info)->format_version && lingvo->reader) {
                const lsd_header *header = lsd_reader_get_header(lingvo->reader);
                if (header) {
                    char ver_buf[16];
                    snprintf(ver_buf, sizeof(ver_buf), "0x%08X", header->version);
                    (*out_info)->format_version = strdup(ver_buf);
                }
            }
        }
        return st;
    }

    if (!lingvo->reader) {
        *out_info = NULL;
        return UNIDICT_OK;
    }

    unidict_info *result = malloc(sizeof(unidict_info));
    if (!result) {
        *out_info = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    result->format = UNIDICT_FORMAT_LINGVO;

    char *name = NULL;
    if (lsd_reader_get_name(lingvo->reader, &name) == LSD_OK && name) {
        result->title = name;
    } else {
        result->title = strdup("");
    }

    char *annotation = NULL;
    if (lsd_reader_read_annotation(lingvo->reader, &annotation) == LSD_OK && annotation) {
        result->description = annotation;
    } else {
        result->description = NULL;
    }
    result->author = NULL;
    result->creation_date = NULL;
    result->source_lang = NULL;
    result->target_lang = NULL;
    const lsd_header *header = lsd_reader_get_header(lingvo->reader);
    if (header) {
        const char *src = lsd_reader_get_source_lang(lingvo->reader);
        if (src) result->source_lang = strdup(src);
        const char *tgt = lsd_reader_get_target_lang(lingvo->reader);
        if (tgt) result->target_lang = strdup(tgt);
    }
    result->word_count = header ? header->entries_count : 0;

    if (header) {
        char ver_buf[16];
        snprintf(ver_buf, sizeof(ver_buf), "0x%08X", header->version);
        result->format_version = strdup(ver_buf);
    } else {
        result->format_version = NULL;
    }

    *out_info = result;
    return UNIDICT_OK;
}

// ============================================================
// File list
// ============================================================

static unidict_status lingvo_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict) {
        *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    unidict_file_info_array *udx_infos = NULL;
    if (lingvo->udx_dict) {
        if (lingvo->udx_dict->ops->file_infos_get)
            lingvo->udx_dict->ops->file_infos_get(lingvo->udx_dict, &udx_infos);
    }

    const char *paths[2];
    int count = 0;
    if (lingvo->lsd_path) paths[count++] = lingvo->lsd_path;
    if (udx_infos && udx_infos->count > 0) paths[count++] = udx_infos->items[0].path;

    *out_infos = unidict_file_infos_from_paths(paths, count);
    if (udx_infos) unidict_file_info_array_free(udx_infos);

    return *out_infos ? UNIDICT_OK : UNIDICT_ERR_NOMEM;
}

// ============================================================
// Index activate
// ============================================================

static unidict_status lingvo_index_activate(unidict *dict, unidict_index_type index_type) {
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    // Close existing UDX if open
    if (lingvo->udx_dict) {
        unidict_close(lingvo->udx_dict);
        lingvo->udx_dict = NULL;
    }
    dict->active_index = UNIDICT_INDEX_BUILTIN;

    // EXTERNAL or NONE (auto): try UDX
    if (index_type == UNIDICT_INDEX_EXTERNAL || index_type == UNIDICT_INDEX_NONE) {
        char *udx_path = lingvo_get_udx_path(lingvo->lsd_path);
        if (udx_path) {
            unidict *udx_dict = ud_udx_open(udx_path, NULL);
            free(udx_path);

            if (udx_dict) {
                lingvo->udx_dict = udx_dict;
                dict->active_index = UNIDICT_INDEX_EXTERNAL;
                return UNIDICT_OK;
            }
        }

        if (index_type == UNIDICT_INDEX_EXTERNAL) {
            return UNIDICT_ERR_IO;
        }
    }

    // BUILTIN or fallback: LSD reader is always available
    return UNIDICT_OK;
}

// ============================================================
// Index external make
//
// Builds a UDX external index containing only article index data
// (key → uint32_t reference). Article content is read on demand via
// lsd_reader_read_article() using the stored reference.
//
// Resource (overlay) data is NOT indexed here: the overlay index (sorted
// name array) is already fully loaded into memory by lsd_reader_open,
// so name-based lookup is fast enough and does not benefit from UDX.
// ============================================================

static unidict_status lingvo_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                 void *user_data) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    char *udx_path = lingvo_get_udx_path(lingvo->lsd_path);
    if (!udx_path) return UNIDICT_ERR_INTERNAL;

    udx_writer *writer = udx_writer_open(udx_path);
    if (!writer) {
        free(udx_path);
        return UNIDICT_ERR_IO;
    }

    unidict_status ret = UNIDICT_ERR_INTERNAL;

    // Build info metadata from LSD reader
    unidict_info meta = {0};
    const lsd_header *header = lsd_reader_get_header(lingvo->reader);
    char *name = NULL;
    if (lsd_reader_get_name(lingvo->reader, &name) == LSD_OK && name) {
        meta.title = name;
    }
    char *annotation = NULL;
    if (lsd_reader_read_annotation(lingvo->reader, &annotation) == LSD_OK && annotation) {
        meta.description = annotation;
    }
    if (header) {
        meta.source_lang = (char *)lsd_reader_get_source_lang(lingvo->reader);
        meta.target_lang = (char *)lsd_reader_get_target_lang(lingvo->reader);
    }
    char *meta_xml = unidict_info_to_xml(&meta);
    free(name);
    free(annotation);

    udx_db_builder *builder = NULL;
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

    lsd_heading_iter *iter = lsd_heading_iter_create(lingvo->reader);
    if (!iter) {
        udx_db_builder_finalize(builder);
        udx_writer_close(writer);
        goto fail;
    }

    int entry_count = 0;
    int last_pct = 0;
    const lsd_heading *heading = NULL;

    while (lsd_heading_iter_next(iter, &heading) == LSD_OK) {
        char *key = NULL;
        lsd_utf16_to_utf8(heading->text, heading->text_length, &key);
        if (!key) continue;

        // Store reference as 4-byte value
        uint32_t ref = heading->reference;
        udx_value_address address = udx_db_builder_add_value(builder, (const uint8_t *)&ref, 4);
        if (address == UDX_INVALID_ADDRESS) {
            free(key);
            continue;
        }

        udx_status err = udx_db_builder_add_key_entry(builder, key, address, 4);
        free(key);

        if (err != UDX_OK) continue;

        entry_count++;

        if (callback && (entry_count % 500) == 0) {
            const lsd_header *hdr = lsd_reader_get_header(lingvo->reader);
            int total = hdr ? (int)hdr->entries_count : entry_count;
            int pct = total > 0 ? (entry_count * 100) / total : 0;
            if (pct > 100) pct = 100;
            if (pct > last_pct) {
                last_pct = pct;
                if (!callback(dict, UNIDICT_INDEX_STAGE_ARTICLES, pct, user_data)) {
                    lsd_heading_iter_destroy(iter);
                    udx_db_builder_finalize(builder);
                    udx_writer_close(writer);
                    ret = UNIDICT_ERR_CANCELLED;
                    goto fail;
                }
            }
        }
    }

    lsd_heading_iter_destroy(iter);

    udx_status err = udx_db_builder_finalize(builder);
    if (err != UDX_OK) {
        udx_writer_close(writer);
        ret = UNIDICT_ERR_IO;
        goto fail;
    }

    err = udx_writer_close(writer);
    if (err != UDX_OK) {
        ret = UNIDICT_ERR_IO;
        goto fail;
    }

    free(udx_path);
    dict->has_external_index = true;
    return UNIDICT_OK;

fail:
    remove(udx_path);
    free(udx_path);
    return ret;
}

// ============================================================
// Index external delete
// ============================================================

static unidict_status lingvo_index_external_delete(unidict *dict) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    // Switch to builtin mode (closes udx_dict)
    unidict_status st = lingvo_index_activate(dict, UNIDICT_INDEX_BUILTIN);
    if (st != UNIDICT_OK) return st;

    // Delete .udx file
    char *udx_path = lingvo_get_udx_path(lingvo->lsd_path);
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
// Lookup
// ============================================================

static unidict_status lingvo_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    if (lingvo->udx_dict) {
        udx_db_value_entry *ve = ud_udx_raw_lookup(lingvo->udx_dict, key);
        if (!ve || ve->items.count == 0) {
            if (ve) udx_db_value_entry_free(ve);
            *out_articles = NULL;
            return UNIDICT_OK;
        }

        unidict_article_array *res = calloc(1, sizeof(unidict_article_array));
        if (!res) {
            udx_db_value_entry_free(ve);
            *out_articles = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        res->count = ve->items.count;
        res->items = calloc(res->count, sizeof(unidict_article));
        if (!res->items) {
            free(res);
            udx_db_value_entry_free(ve);
            *out_articles = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        for (size_t i = 0; i < ve->items.count; i++) {
            uint32_t ref = lingvo_ref_from_value_item(&ve->items.elements[i]);
            char *body = NULL;
            if (ref > 0 && lsd_reader_read_article(lingvo->reader, ref, &body) == LSD_OK && body) {
                res->items[i].title = ve->items.elements[i].original_key
                                          ? strdup(ve->items.elements[i].original_key)
                                          : NULL;
                res->items[i].body = body;
            }
        }

        udx_db_value_entry_free(ve);
        *out_articles = res;
        return UNIDICT_OK;
    }

    lsd_heading heading;
    memset(&heading, 0, sizeof(heading));
    if (lsd_reader_find_heading(lingvo->reader, key, &heading) != LSD_OK) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }

    char *title = NULL;
    lsd_utf16_to_utf8(heading.text, heading.text_length, &title);

    char *definition = NULL;
    lsd_status ret = lsd_reader_read_article(lingvo->reader, heading.reference, &definition);
    lsd_heading_destroy(&heading);

    if (ret != LSD_OK || !definition) {
        free(title);
        *out_articles = NULL;
        return UNIDICT_OK;
    }

    unidict_article_array *res = malloc(sizeof(unidict_article_array));
    if (!res) {
        free(definition);
        free(title);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->count = 1;
    res->items = malloc(sizeof(unidict_article));
    if (!res->items) {
        free(res);
        free(definition);
        free(title);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->items[0].title = title;
    res->items[0].body = definition;
    *out_articles = res;
    return UNIDICT_OK;
}

static unidict_status lingvo_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    if (lingvo->udx_dict) {
        udx_db_value_entry *ve = ud_udx_raw_lookup(lingvo->udx_dict, key);
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
            uint32_t ref = lingvo_ref_from_value_item(&ve->items.elements[i]);

            ud_lingvo_index *idx = calloc(1, sizeof(ud_lingvo_index));
            if (!idx) continue;
            idx->reference = ref;
            uobject_init(&idx->obj, &ud_lingvo_index_type, NULL);

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (!entry) {
                uobject_release(&idx->obj);
                continue;
            }
            entry->key = ve->items.elements[i].original_key ? strdup(ve->items.elements[i].original_key) : strdup(key);
            entry->internal_entry = &idx->obj;
            entries->items[i] = entry;
        }

        udx_db_value_entry_free(ve);
        *out_entries = entries;
        return UNIDICT_OK;
    }

    lsd_heading heading;
    memset(&heading, 0, sizeof(heading));
    if (lsd_reader_find_heading(lingvo->reader, key, &heading) != LSD_OK) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }

    ud_lingvo_index *lingvo_idx = calloc(1, sizeof(ud_lingvo_index));
    if (!lingvo_idx) {
        lsd_heading_destroy(&heading);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }
    lingvo_idx->reference = heading.reference;
    uobject_init(&lingvo_idx->obj, &ud_lingvo_index_type, NULL);
    lsd_heading_destroy(&heading);

    unidict_entry_array *entries = malloc(sizeof(unidict_entry_array));
    if (!entries) {
        uobject_release(&lingvo_idx->obj);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entries->items = calloc(1, sizeof(unidict_entry *));
    if (!entries->items) {
        free(entries);
        uobject_release(&lingvo_idx->obj);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    unidict_entry *entry = calloc(1, sizeof(unidict_entry));
    if (!entry) {
        free(entries->items);
        free(entries);
        uobject_release(&lingvo_idx->obj);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entry->key = strdup(key);
    entry->internal_entry = &lingvo_idx->obj;
    entries->items[0] = entry;
    entries->count = 1;
    *out_entries = entries;
    return UNIDICT_OK;
}

static unidict_status lingvo_suggest(unidict *dict, const char *prefix, size_t limit,
                                     unidict_entry_array **out_entries) {
    if (!dict || !prefix) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    if (lingvo->udx_dict) {
        if (!lingvo->udx_dict->ops->suggest) {
            *out_entries = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_entry_array *udx_entries = NULL;
        unidict_status st = lingvo->udx_dict->ops->suggest(lingvo->udx_dict, prefix, limit, &udx_entries);
        if (st != UNIDICT_OK || !udx_entries) {
            *out_entries = NULL;
            return st;
        }

        unidict_entry_array *res = calloc(1, sizeof(unidict_entry_array));
        if (!res) {
            unidict_entry_array_free(udx_entries);
            *out_entries = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        res->count = udx_entries->count;
        res->items = calloc(udx_entries->count, sizeof(unidict_entry *));
        if (!res->items) {
            free(res);
            unidict_entry_array_free(udx_entries);
            *out_entries = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        for (size_t i = 0; i < udx_entries->count; i++) {
            unidict_entry *udx_entry = udx_entries->items[i];
            if (!udx_entry) continue;

            udx_db_value_entry *ve = ud_udx_raw_fetch(lingvo->udx_dict, udx_entry);
            if (!ve || ve->items.count == 0) {
                if (ve) udx_db_value_entry_free(ve);
                continue;
            }

            uint32_t ref = lingvo_ref_from_value_item(&ve->items.elements[0]);
            udx_db_value_entry_free(ve);

            ud_lingvo_index *lingvo_idx = calloc(1, sizeof(ud_lingvo_index));
            if (!lingvo_idx) continue;
            lingvo_idx->reference = ref;
            uobject_init(&lingvo_idx->obj, &ud_lingvo_index_type, NULL);

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (!entry) {
                uobject_release(&lingvo_idx->obj);
                continue;
            }

            entry->key = strdup(udx_entry->key);
            entry->internal_entry = &lingvo_idx->obj;
            res->items[i] = entry;
        }

        unidict_entry_array_free(udx_entries);
        *out_entries = res;
        return UNIDICT_OK;
    }

    lsd_heading *headings = NULL;
    size_t count = 0;

    if (lsd_reader_prefix(lingvo->reader, prefix, limit, &headings, &count) != LSD_OK) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }

    if (count == 0) {
        free(headings);
        *out_entries = NULL;
        return UNIDICT_OK;
    }

    unidict_entry_array *entries = malloc(sizeof(unidict_entry_array));
    if (!entries) {
        for (size_t i = 0; i < count; i++) {
            lsd_heading_destroy(&headings[i]);
        }
        free(headings);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entries->count = count;
    entries->items = calloc(count, sizeof(unidict_entry *));
    if (!entries->items) {
        free(entries);
        for (size_t i = 0; i < count; i++) {
            lsd_heading_destroy(&headings[i]);
        }
        free(headings);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    for (size_t i = 0; i < count; i++) {
        ud_lingvo_index *lingvo_idx = calloc(1, sizeof(ud_lingvo_index));
        if (lingvo_idx) {
            lingvo_idx->reference = headings[i].reference;
            uobject_init(&lingvo_idx->obj, &ud_lingvo_index_type, NULL);

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (entry) {
                lsd_utf16_to_utf8(headings[i].text, headings[i].text_length, &entry->key);
                entry->internal_entry = &lingvo_idx->obj;
                entries->items[i] = entry;
            } else {
                uobject_release(&lingvo_idx->obj);
            }
        }
        lsd_heading_destroy(&headings[i]);
    }

    free(headings);
    *out_entries = entries;
    return UNIDICT_OK;
}

static unidict_status lingvo_lookup_by_entry(unidict *dict, unidict_entry *entry,
                                             unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);
    ud_lingvo_index *lingvo_idx = uobject_cast(entry->internal_entry, ud_lingvo_index, obj);

    char *body = NULL;
    if (lsd_reader_read_article(lingvo->reader, lingvo_idx->reference, &body) != LSD_OK || !body) {
        *out_articles = NULL;
        return UNIDICT_OK;
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

// ============================================================
// Entry iterator
// ============================================================

static unidict_entry_iter *lingvo_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    ud_lingvo_entry_iter *iter = calloc(1, sizeof(ud_lingvo_entry_iter));
    if (!iter) return NULL;

    iter->base.dict = dict;
    iter->base.current.key = NULL;
    iter->base.current.internal_entry = &iter->current_idx.obj;
    uobject_init(&iter->current_idx.obj, &ud_lingvo_index_type, NULL);

    if (lingvo->udx_dict) {
        if (!lingvo->udx_dict->ops->entry_iter_create) {
            free(iter);
            return NULL;
        }
        iter->udx_iter = lingvo->udx_dict->ops->entry_iter_create(lingvo->udx_dict);
        if (!iter->udx_iter) {
            free(iter);
            return NULL;
        }
    } else {
        iter->lsd_iter = lsd_heading_iter_create(lingvo->reader);
        if (!iter->lsd_iter) {
            free(iter);
            return NULL;
        }
    }

    return (unidict_entry_iter *)iter;
}

static unidict_status lingvo_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict) {
        if (out_entry) *out_entry = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_lingvo_entry_iter *lingvo_iter = (ud_lingvo_entry_iter *)iter;
    ud_lingvo *lingvo = uobject_cast(&iter->dict->obj, ud_lingvo, base.obj);

    free(iter->current.key);
    iter->current.key = NULL;

    if (lingvo_iter->udx_iter) {
        if (!lingvo->udx_dict->ops->entry_iter_next) {
            *out_entry = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_entry *udx_entry = NULL;
        unidict_status st = lingvo->udx_dict->ops->entry_iter_next(lingvo_iter->udx_iter, &udx_entry);
        if (st != UNIDICT_OK || !udx_entry) {
            *out_entry = NULL;
            return UNIDICT_DONE;
        }

        iter->current.key = strdup(udx_entry->key);

        udx_db_value_entry *ve = ud_udx_raw_fetch(lingvo->udx_dict, udx_entry);
        if (ve && ve->items.count > 0) {
            lingvo_iter->current_idx.reference = lingvo_ref_from_value_item(&ve->items.elements[0]);
        } else {
            lingvo_iter->current_idx.reference = 0;
        }
        if (ve) udx_db_value_entry_free(ve);
    } else {
        const lsd_heading *heading = NULL;
        lsd_status st = lsd_heading_iter_next(lingvo_iter->lsd_iter, &heading);
        if (st != LSD_OK || !heading) {
            *out_entry = NULL;
            return UNIDICT_DONE;
        }

        lsd_utf16_to_utf8(heading->text, heading->text_length, &iter->current.key);
        lingvo_iter->current_idx.reference = heading->reference;
    }

    if (!iter->current.key) {
        *out_entry = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    *out_entry = &iter->current;
    return UNIDICT_OK;
}

static void lingvo_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;
    ud_lingvo_entry_iter *lingvo_iter = (ud_lingvo_entry_iter *)iter;
    free(iter->current.key);
    // current_idx is an embedded member of iter (not separately allocated),
    // so it must NOT be uobject_release()'d — it is freed together with iter.
    if (lingvo_iter->lsd_iter) lsd_heading_iter_destroy(lingvo_iter->lsd_iter);
    if (lingvo_iter->udx_iter) {
        ud_lingvo *lingvo = uobject_cast(&iter->dict->obj, ud_lingvo, base.obj);
        if (lingvo->udx_dict && lingvo->udx_dict->ops->entry_iter_free)
            lingvo->udx_dict->ops->entry_iter_free(lingvo_iter->udx_iter);
    }
    free(iter);
}

// ============================================================
// Resource operations
// ============================================================

static unidict_status lingvo_resource_get(unidict *dict, const char *key, unidict_resource **out_res) {
    if (!dict || !key || !out_res) return UNIDICT_ERR_INVALID_PARAM;
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    uint8_t *data = NULL;
    size_t size = 0;
    lsd_status st = lsd_reader_read_overlay(lingvo->reader, key, &data, &size);
    if (st != LSD_OK) {
        *out_res = NULL;
        return UNIDICT_OK;
    }

    unidict_resource *res = calloc(1, sizeof(unidict_resource));
    if (!res) {
        free(data);
        *out_res = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->key = strdup(key);
    res->data = data;
    res->size = size;
    res->mime_type = NULL;
    *out_res = res;
    return UNIDICT_OK;
}

static unidict_resource_iter *lingvo_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode) {
    if (!dict) return NULL;
    ud_lingvo *lingvo = uobject_cast(&dict->obj, ud_lingvo, base.obj);

    size_t count = lsd_reader_get_overlay_count(lingvo->reader);

    ud_lingvo_resource_iter *iter = calloc(1, sizeof(ud_lingvo_resource_iter));
    if (!iter) return NULL;

    iter->base.dict = dict;
    iter->base.current.key = NULL;
    iter->base.current.data = NULL;
    iter->base.current.size = 0;
    iter->base.current.mime_type = NULL;
    iter->pos = 0;
    iter->count = count;
    iter->mode = mode;

    return (unidict_resource_iter *)iter;
}

static unidict_status lingvo_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res) {
    if (!iter || !iter->dict) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_lingvo_resource_iter *lingvo_iter = (ud_lingvo_resource_iter *)iter;

    if (lingvo_iter->pos >= lingvo_iter->count) {
        *out_res = NULL;
        return UNIDICT_DONE;
    }

    ud_lingvo *lingvo = uobject_cast(&iter->dict->obj, ud_lingvo, base.obj);

    // Free previous data
    free(iter->current.key);
    iter->current.key = NULL;
    free(iter->current.data);
    iter->current.data = NULL;
    iter->current.size = 0;
    free(iter->current.mime_type);
    iter->current.mime_type = NULL;

    char *name = NULL;
    uint8_t *data = NULL;
    size_t size = 0;

    if (lingvo_iter->mode == UNIDICT_RESOURCE_ITER_KEY) {
        lsd_status st = lsd_reader_read_overlay_at(lingvo->reader, lingvo_iter->pos, &name, NULL, NULL);
        if (st != LSD_OK) {
            *out_res = NULL;
            return UNIDICT_DONE;
        }
    } else {
        lsd_status st = lsd_reader_read_overlay_at(lingvo->reader, lingvo_iter->pos, &name, &data, &size);
        if (st != LSD_OK) {
            *out_res = NULL;
            return UNIDICT_DONE;
        }
    }

    lingvo_iter->pos++;
    iter->current.key = name;
    iter->current.data = data;
    iter->current.size = size;

    *out_res = &iter->current;
    return UNIDICT_OK;
}

static void lingvo_resource_iter_free(unidict_resource_iter *iter) {
    if (!iter) return;
    free(iter->current.key);
    free(iter->current.data);
    free(iter->current.mime_type);
    free(iter);
}
