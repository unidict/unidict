//
//  ud_udx.c
//  unidict
//
//  Created by kejinlu on 2026-05-22
//
#include "ud_udx.h"
#include "unidict_internal.h"
#include "unidict_log.h"
#include "udx_reader.h"
#include "udx_types.h"
#include "udx_types_internal.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    unidict base;
    udx_reader *reader;
    udx_db *article_db;
    udx_db *resource_db;
    char *udx_path;
} ud_udx;

// ============================================================
// Entry Ref (suggest → fetch fast path)
// ============================================================

typedef struct {
    uobject obj;
    const udx_db_key_entry *key_entry;
    bool owned;
} ud_udx_db_key_entry;

static void ud_udx_db_key_entry_release(uobject *obj) {
    ud_udx_db_key_entry *wrapper = uobject_cast(obj, ud_udx_db_key_entry, obj);
    if (wrapper->owned) {
        udx_db_key_entry_cleanup((udx_db_key_entry *)wrapper->key_entry);
        free((void *)wrapper->key_entry);
    }
    free(wrapper);
}

static const uobject_type ud_udx_db_key_entry_type = {
    .name = "ud_udx_db_key_entry",
    .size = sizeof(ud_udx_db_key_entry),
    .release = ud_udx_db_key_entry_release,
};

// ============================================================
// Release & Type
// ============================================================

static void ud_udx_release(uobject *obj) {
    if (!obj) return;
    ud_udx *udx = uobject_cast(obj, ud_udx, base.obj);

    if (udx->resource_db) {
        udx_db_close(udx->resource_db);
    }
    if (udx->article_db) {
        udx_db_close(udx->article_db);
    }
    if (udx->reader) {
        udx_reader_close(udx->reader);
    }
    free(udx->udx_path);
    free(udx);
}

static const uobject_type ud_udx_type = {
    .name = "ud_udx",
    .size = sizeof(ud_udx),
    .release = ud_udx_release,
};

// ============================================================
// MIME type helper
// ============================================================

static const char *guess_mime_from_ext(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return NULL;

    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".bmp") == 0) return "image/bmp";
    if (strcasecmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(ext, ".ogg") == 0) return "audio/ogg";
    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html";
    if (strcasecmp(ext, ".css") == 0) return "text/css";
    if (strcasecmp(ext, ".js") == 0) return "application/javascript";
    return NULL;
}

// ============================================================
// Forward declare vtables (defined later)
// ============================================================

static const unidict_ops ud_udx_ops;

// ============================================================
// Open internal

static unidict *ud_udx_open_internal(const char *file_path, udx_db *article_db, udx_db *resource_db,
                                     udx_reader *reader) {
    ud_udx *udx = calloc(1, sizeof(ud_udx));
    if (!udx) return NULL;

    uobject_init(&udx->base.obj, &ud_udx_type, NULL);
    udx->base.ops = &ud_udx_ops;
    udx->base.format = UNIDICT_FORMAT_UDX;
    udx->base.has_builtin_index = true;
    udx->base.active_index = UNIDICT_INDEX_BUILTIN;

    udx->reader = reader;
    udx->article_db = article_db;
    udx->resource_db = resource_db;
    udx->udx_path = strdup(file_path);

    return &udx->base;
}

// ============================================================
// Raw data access
// ============================================================

udx_db_value_entry *ud_udx_raw_lookup(unidict *dict, const char *key) {
    if (!dict || !key) return NULL;

    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);
    udx_db_value_entry *entry = NULL;
    udx_status status = udx_db_value_entry_lookup(udx->article_db, key, &entry);
    if (status != UDX_OK || !entry || entry->items.count == 0) {
        if (entry) udx_db_value_entry_free(entry);
        return NULL;
    }
    return entry;
}

udx_db_value_entry *ud_udx_raw_fetch(unidict *dict, const unidict_entry *entry) {
    if (!dict || !entry || !entry->internal_entry) return NULL;

    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);
    ud_udx_db_key_entry *wrapper = uobject_cast(entry->internal_entry, ud_udx_db_key_entry, obj);

    udx_db_value_entry *ve = NULL;
    udx_status status = udx_db_value_entry_fetch(udx->article_db, wrapper->key_entry, &ve);
    if (status != UDX_OK || !ve || ve->items.count == 0) {
        if (ve) udx_db_value_entry_free(ve);
        return NULL;
    }
    return ve;
}

udx_db_value_entry *ud_udx_raw_resource_get(unidict *dict, const char *key) {
    if (!dict || !key) return NULL;

    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);
    if (!udx->resource_db) return NULL;

    udx_db_value_entry *entry = NULL;
    udx_status status = udx_db_value_entry_lookup(udx->resource_db, key, &entry);
    if (status != UDX_OK || !entry || entry->items.count == 0) {
        if (entry) udx_db_value_entry_free(entry);
        return NULL;
    }
    return entry;
}

// ============================================================
// Raw resource iterator
// ============================================================

struct ud_udx_raw_res_iter {
    unidict *dict;
    udx_db_iter *udx_iter;
};

ud_udx_raw_res_iter *ud_udx_raw_res_iter_create(unidict *dict) {
    if (!dict) return NULL;

    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);
    if (!udx->resource_db) return NULL;

    ud_udx_raw_res_iter *iter = calloc(1, sizeof(ud_udx_raw_res_iter));
    if (!iter) return NULL;

    iter->dict = dict;
    iter->udx_iter = udx_db_iter_create(udx->resource_db);
    if (!iter->udx_iter) {
        free(iter);
        return NULL;
    }
    return iter;
}

udx_db_value_entry *ud_udx_raw_res_iter_next(ud_udx_raw_res_iter *iter) {
    if (!iter) return NULL;

    ud_udx *udx = uobject_cast(&iter->dict->obj, ud_udx, base.obj);

    const udx_db_key_entry *entry = NULL;
    udx_status status = udx_db_iter_next(iter->udx_iter, &entry);
    if (status != UDX_OK || !entry || entry->items.count == 0) return NULL;

    udx_db_value_entry *ve = NULL;
    status = udx_db_value_entry_fetch(udx->resource_db, entry, &ve);
    if (status != UDX_OK || !ve || ve->items.count == 0) {
        if (ve) udx_db_value_entry_free(ve);
        return NULL;
    }
    return ve;
}

void ud_udx_raw_res_iter_free(ud_udx_raw_res_iter *iter) {
    if (!iter) return;
    if (iter->udx_iter) udx_db_iter_destroy(iter->udx_iter);
    free(iter);
}

// ============================================================
// Conversion helpers
// ============================================================

static unidict_article_array *value_entry_to_articles(udx_db_value_entry *ve) {
    if (!ve || ve->items.count == 0) return NULL;

    unidict_article_array *res = malloc(sizeof(unidict_article_array));
    if (!res) return NULL;

    res->count = ve->items.count;
    res->items = calloc(ve->items.count, sizeof(unidict_article));
    if (!res->items) {
        free(res);
        return NULL;
    }

    for (size_t i = 0; i < ve->items.count; i++) {
        udx_value_entry_item *item = &ve->items.elements[i];
        res->items[i].title = item->original_key ? strdup(item->original_key) : NULL;
        if (item->data && item->size > 0) {
            res->items[i].body = malloc(item->size + 1);
            if (res->items[i].body) {
                memcpy(res->items[i].body, item->data, item->size);
                res->items[i].body[item->size] = '\0';
            }
        } else {
            res->items[i].body = strdup("");
        }
    }
    return res;
}

// ============================================================
// VTable
// ============================================================

static unidict_status ud_udx_info_get(unidict *dict, unidict_info **out_info);
static unidict_status ud_udx_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status ud_udx_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status ud_udx_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status ud_udx_suggest(unidict *dict, const char *prefix, size_t limit,
                                     unidict_entry_array **out_entries);
static unidict_status ud_udx_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_entry_iter *ud_udx_entry_iter_create(unidict *dict);
static unidict_status ud_udx_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void ud_udx_entry_iter_free(unidict_entry_iter *iter);
static unidict_status ud_udx_resource_get(unidict *dict, const char *key, unidict_resource **out_res);
static unidict_resource_iter *ud_udx_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode);
static unidict_status ud_udx_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res);
static void ud_udx_resource_iter_free(unidict_resource_iter *iter);

static const unidict_ops ud_udx_ops = {
    .prepare = NULL,
    .info_get = ud_udx_info_get,
    .file_infos_get = ud_udx_file_infos_get,
    .index_external_make = NULL,
    .index_external_delete = NULL,
    .lookup = ud_udx_lookup,
    .entry_lookup = ud_udx_entry_lookup,
    .suggest = ud_udx_suggest,
    .fetch = ud_udx_fetch,
    .entry_iter_create = ud_udx_entry_iter_create,
    .entry_iter_next = ud_udx_entry_iter_next,
    .entry_iter_free = ud_udx_entry_iter_free,
    .resource_get = ud_udx_resource_get,
    .resource_iter_create = ud_udx_resource_iter_create,
    .resource_iter_next = ud_udx_resource_iter_next,
    .resource_iter_free = ud_udx_resource_iter_free,
};

// ============================================================
// Public Open (delegates to internal after vtable is available)
// ============================================================

unidict *ud_udx_open(const char *file_path, const unidict_open_options *options) {
    if (!file_path) return NULL;

    udx_reader *reader = NULL;
    if (udx_reader_open(file_path, &reader) != UDX_OK || !reader) return NULL;

    udx_db *article_db = NULL;
    if (udx_db_open(reader, "article", &article_db) != UDX_OK || !article_db) {
        if (udx_db_open(reader, NULL, &article_db) != UDX_OK || !article_db) {
            udx_reader_close(reader);
            return NULL;
        }
    }

    udx_db *resource_db = NULL;
    udx_db_open(reader, "resource", &resource_db);

    unidict *dict = ud_udx_open_internal(file_path, article_db, resource_db, reader);
    if (!dict) {
        udx_db_close(article_db);
        if (resource_db) udx_db_close(resource_db);
        udx_reader_close(reader);
    }
    return dict;
}

// ============================================================
// Info
// ============================================================

static unidict_status ud_udx_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict) {
        if (out_info) *out_info = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);

    // Try reading metadata from UDX
    uint32_t meta_size = 0;
    const uint8_t *meta_data = udx_db_get_metadata(udx->article_db, &meta_size);
    if (meta_data && meta_size > 0) {
        // Null-terminate for XML parsing
        char *meta_xml = malloc(meta_size + 1);
        if (meta_xml) {
            memcpy(meta_xml, meta_data, meta_size);
            meta_xml[meta_size] = '\0';
            unidict_info *info = unidict_info_from_xml(meta_xml);
            free(meta_xml);
            if (info) {
                info->format = dict->format;
                info->word_count = (uint64_t)udx_db_get_item_count(udx->article_db);
                *out_info = info;
                return UNIDICT_OK;
            }
        }
    }

    // Fallback: no metadata
    unidict_info *info = calloc(1, sizeof(unidict_info));
    if (!info) {
        *out_info = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    info->format = dict->format;
    info->title = strdup(udx_db_get_name(udx->article_db) ?: "UDX Dictionary");
    info->word_count = (uint64_t)udx_db_get_item_count(udx->article_db);

    *out_info = info;
    return UNIDICT_OK;
}

// ============================================================
// File List
// ============================================================

static unidict_status ud_udx_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict) {
        if (out_infos) *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);

    if (!udx->udx_path) {
        *out_infos = NULL;
        return UNIDICT_OK;
    }

    const char *paths[] = {udx->udx_path};
    *out_infos = unidict_file_infos_from_paths(paths, 1);
    return *out_infos ? UNIDICT_OK : UNIDICT_ERR_NOMEM;
}

// ============================================================
// Lookup (standard op → raw_lookup + convert)
// ============================================================

static unidict_status ud_udx_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key) {
        if (out_articles) *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    udx_db_value_entry *ve = ud_udx_raw_lookup(dict, key);
    if (!ve) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }

    *out_articles = value_entry_to_articles(ve);
    udx_db_value_entry_free(ve);

    return *out_articles ? UNIDICT_OK : UNIDICT_ERR_NOMEM;
}

// ============================================================
// Entry Lookup
// ============================================================

static unidict_status ud_udx_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key) {
        if (out_entries) *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);

    udx_db_key_entry *key_entry = NULL;
    udx_status status = udx_db_key_entry_lookup(udx->article_db, key, &key_entry);
    if (status != UDX_OK || !key_entry || key_entry->items.count == 0) {
        if (key_entry) udx_db_key_entry_free(key_entry);
        *out_entries = NULL;
        return UNIDICT_OK;
    }

    ud_udx_db_key_entry *wrapper = calloc(1, sizeof(ud_udx_db_key_entry));
    if (!wrapper) {
        udx_db_key_entry_free(key_entry);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }
    uobject_init(&wrapper->obj, &ud_udx_db_key_entry_type, NULL);

    wrapper->key_entry = key_entry;
    wrapper->owned = true;

    unidict_entry *entry = calloc(1, sizeof(unidict_entry));
    if (!entry) {
        uobject_release(&wrapper->obj);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entry->key = strdup(wrapper->key_entry->items.elements[0].original_key ?: "");
    entry->internal_entry = &wrapper->obj;

    unidict_entry_array *entries = malloc(sizeof(unidict_entry_array));
    if (!entries) {
        free(entry->key);
        free(entry);
        uobject_release(&wrapper->obj);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entries->count = 1;
    entries->items = calloc(1, sizeof(unidict_entry *));
    if (!entries->items) {
        free(entries);
        free(entry->key);
        free(entry);
        uobject_release(&wrapper->obj);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entries->items[0] = entry;
    *out_entries = entries;
    return UNIDICT_OK;
}

// ============================================================
// Suggest
// ============================================================

static unidict_status ud_udx_suggest(unidict *dict, const char *prefix, size_t limit,
                                     unidict_entry_array **out_entries) {
    if (!dict || !prefix) {
        if (out_entries) *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);

    udx_db_key_entry_array *entries = NULL;
    udx_status status = udx_db_key_entry_prefix_match(udx->article_db, prefix, limit, &entries);
    if (status != UDX_OK || !entries || entries->count == 0) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }

    unidict_entry_array *res = malloc(sizeof(unidict_entry_array));
    if (!res) {
        udx_db_key_entry_array_free(entries);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->count = entries->count;
    res->items = calloc(entries->count, sizeof(unidict_entry *));
    if (!res->items) {
        free(res);
        udx_db_key_entry_array_free(entries);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    for (size_t i = 0; i < entries->count; i++) {
        udx_db_key_entry *src = &entries->elements[i];

        ud_udx_db_key_entry *wrapper = calloc(1, sizeof(ud_udx_db_key_entry));
        if (!wrapper) break;
        uobject_init(&wrapper->obj, &ud_udx_db_key_entry_type, NULL);

        udx_db_key_entry *copy = malloc(sizeof(udx_db_key_entry));
        if (!copy) { uobject_release(&wrapper->obj); break; }
        *copy = *src;
        memset(src, 0, sizeof(*src));
        wrapper->key_entry = copy;
        wrapper->owned = true;

        unidict_entry *entry = calloc(1, sizeof(unidict_entry));
        if (!entry) {
            uobject_release(&wrapper->obj);
            break;
        }

        entry->key = strdup(wrapper->key_entry->items.elements[0].original_key ?: "");
        entry->internal_entry = &wrapper->obj;
        res->items[i] = entry;
    }

    udx_db_key_entry_array_free(entries);
    *out_entries = res;
    return UNIDICT_OK;
}

// ============================================================
// Fetch (standard op → raw_fetch + convert)
// ============================================================

static unidict_status ud_udx_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry) {
        if (out_articles) *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    udx_db_value_entry *ve = ud_udx_raw_fetch(dict, entry);
    if (!ve) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }

    *out_articles = value_entry_to_articles(ve);
    udx_db_value_entry_free(ve);

    return *out_articles ? UNIDICT_OK : UNIDICT_ERR_NOMEM;
}

// ============================================================
// Entry Iterator
// ============================================================

typedef struct {
    unidict_entry_iter base;
    udx_db_iter *udx_iter;
} ud_udx_entry_iter;

static unidict_entry_iter *ud_udx_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;

    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);

    ud_udx_entry_iter *iter = malloc(sizeof(ud_udx_entry_iter));
    if (!iter) return NULL;

    iter->base.dict = dict;
    iter->base.current.key = NULL;
    iter->base.current.internal_entry = NULL;
    iter->udx_iter = udx_db_iter_create(udx->article_db);

    if (!iter->udx_iter) {
        free(iter);
        return NULL;
    }

    return (unidict_entry_iter *)iter;
}

static unidict_status ud_udx_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict) {
        if (out_entry) *out_entry = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_udx_entry_iter *udx_iter = (ud_udx_entry_iter *)iter;

    const udx_db_key_entry *udx_entry = NULL;
    udx_status status = udx_db_iter_next(udx_iter->udx_iter, &udx_entry);
    if (status != UDX_OK || !udx_entry || udx_entry->items.count == 0) {
        *out_entry = NULL;
        return UNIDICT_DONE;
    }

    free(iter->current.key);
    if (iter->current.internal_entry) {
        uobject_release(iter->current.internal_entry);
        iter->current.internal_entry = NULL;
    }

    iter->current.key = strdup(udx_entry->items.elements[0].original_key ?: "");
    if (!iter->current.key) {
        *out_entry = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    // Wrap key_entry for fetch (borrowed pointer — valid until next iter_next or iter_free)
    ud_udx_db_key_entry *wrapper = calloc(1, sizeof(ud_udx_db_key_entry));
    if (!wrapper) {
        free(iter->current.key);
        iter->current.key = NULL;
        *out_entry = NULL;
        return UNIDICT_ERR_NOMEM;
    }
    uobject_init(&wrapper->obj, &ud_udx_db_key_entry_type, NULL);
    wrapper->key_entry = udx_entry;
    wrapper->owned = false;
    iter->current.internal_entry = &wrapper->obj;

    *out_entry = &iter->current;
    return UNIDICT_OK;
}

static void ud_udx_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;

    ud_udx_entry_iter *udx_iter = (ud_udx_entry_iter *)iter;
    free(iter->current.key);
    if (iter->current.internal_entry) {
        uobject_release(iter->current.internal_entry);
    }
    if (udx_iter->udx_iter) udx_db_iter_destroy(udx_iter->udx_iter);
    free(iter);
}

// ============================================================
// Resource Get (standard op → raw_resource_get + convert)
// ============================================================

static unidict_status ud_udx_resource_get(unidict *dict, const char *key, unidict_resource **out_res) {
    if (!dict || !key) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    udx_db_value_entry *ve = ud_udx_raw_resource_get(dict, key);
    if (!ve) {
        *out_res = NULL;
        return UNIDICT_OK;
    }

    udx_value_entry_item *item = &ve->items.elements[0];
    unidict_resource *res = calloc(1, sizeof(unidict_resource));
    if (!res) {
        udx_db_value_entry_free(ve);
        *out_res = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->size = item->size;
    res->data = malloc(res->size);
    if (!res->data) {
        free(res);
        udx_db_value_entry_free(ve);
        *out_res = NULL;
        return UNIDICT_ERR_NOMEM;
    }
    memcpy(res->data, item->data, res->size);
    res->key = strdup(key);
    const char *mime = guess_mime_from_ext(key);
    res->mime_type = mime ? strdup(mime) : NULL;

    udx_db_value_entry_free(ve);
    *out_res = res;
    return UNIDICT_OK;
}

// ============================================================
// Resource Iterator
// ============================================================

typedef struct {
    unidict_resource_iter base;
    udx_db_iter *udx_iter;
    unidict_resource_iter_mode mode;
} ud_udx_resource_iter;

static unidict_resource_iter *ud_udx_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode) {
    if (!dict) return NULL;

    ud_udx *udx = uobject_cast(&dict->obj, ud_udx, base.obj);
    if (!udx->resource_db) return NULL;

    ud_udx_resource_iter *iter = calloc(1, sizeof(ud_udx_resource_iter));
    if (!iter) return NULL;

    iter->base.dict = dict;
    iter->mode = mode;
    iter->udx_iter = udx_db_iter_create(udx->resource_db);
    if (!iter->udx_iter) {
        free(iter);
        return NULL;
    }

    return (unidict_resource_iter *)iter;
}

static void ud_udx_resource_current_cleanup(unidict_resource *res) {
    if (!res) return;
    free(res->key);
    res->key = NULL;
    free(res->data);
    res->data = NULL;
    free(res->mime_type);
    res->mime_type = NULL;
    res->size = 0;
}

static unidict_status ud_udx_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res) {
    if (!iter || !out_res) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_udx_resource_iter *res_iter = (ud_udx_resource_iter *)iter;
    ud_udx *udx = uobject_cast(&res_iter->base.dict->obj, ud_udx, base.obj);

    ud_udx_resource_current_cleanup(&iter->current);

    const udx_db_key_entry *entry = NULL;
    udx_status status = udx_db_iter_next(res_iter->udx_iter, &entry);
    if (status != UDX_OK || !entry || entry->items.count == 0) {
        *out_res = NULL;
        return UNIDICT_DONE;
    }

    const char *key = entry->items.elements[0].original_key ?: "";
    iter->current.key = strdup(key);
    if (!iter->current.key) {
        *out_res = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    if (res_iter->mode == UNIDICT_RESOURCE_ITER_FULL) {
        udx_db_value_entry *ve = NULL;
        udx_status fetch_st = udx_db_value_entry_fetch(udx->resource_db, entry, &ve);
        if (fetch_st == UDX_OK && ve && ve->items.count > 0) {
            udx_value_entry_item *item = &ve->items.elements[0];
            iter->current.size = item->size;
            iter->current.data = malloc(iter->current.size);
            if (iter->current.data) {
                memcpy(iter->current.data, item->data, iter->current.size);
            }
        }
        if (ve) udx_db_value_entry_free(ve);

        const char *mime = guess_mime_from_ext(key);
        iter->current.mime_type = mime ? strdup(mime) : NULL;
    }

    *out_res = &iter->current;
    return UNIDICT_OK;
}

static void ud_udx_resource_iter_free(unidict_resource_iter *iter) {
    if (!iter) return;
    ud_udx_resource_iter *res_iter = (ud_udx_resource_iter *)iter;
    ud_udx_resource_current_cleanup(&iter->current);
    if (res_iter->udx_iter) udx_db_iter_destroy(res_iter->udx_iter);
    free(iter);
}
