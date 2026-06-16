//
//  ud_stardict.c
//  unidict
//
//  Created by kejinlu on 2026-01-01
//
#include "ud_stardict.h"
#include "unidict_internal.h"
#include "unidict_log.h"
#include "ud_udx.h"
#include "udx_writer.h"
#include "sd_stardict.h"
#include "sd_dictfile_index.h"
#include "sd_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

// ============================================================
// 索引对象（存储 offset/size 用于快速查询）
// ============================================================

typedef struct {
    uobject obj;
    uint32_t offset;
    uint32_t size;
} ud_stardict_index;

static void ud_stardict_index_release(uobject *obj) {
    ud_stardict_index *idx = uobject_cast(obj, ud_stardict_index, obj);
    free(idx);
}

static const uobject_type ud_stardict_index_type = {
    .name = "ud_stardict_index",
    .size = sizeof(ud_stardict_index),
    .release = ud_stardict_index_release,
};

// ============================================================
// StarDict 对象
// ============================================================

typedef struct ud_stardict ud_stardict;

struct ud_stardict {
    unidict base;
    sd_stardict *sd;
    unidict *udx_dict; // UDX external index
    char *ifo_path;    // stored .ifo path for deriving .udx path
};

// ============================================================
// 虚函数表
// ============================================================

static void ud_stardict_release(uobject *obj);

static unidict_status ud_stardict_info_get(unidict *dict, unidict_info **out_info);
static unidict_status ud_stardict_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status ud_stardict_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status ud_stardict_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status ud_stardict_suggest(unidict *dict, const char *prefix, size_t limit,
                                          unidict_entry_array **out_entries);
static unidict_status ud_stardict_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_entry_iter *ud_stardict_entry_iter_create(unidict *dict);
static unidict_status ud_stardict_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void ud_stardict_entry_iter_free(unidict_entry_iter *iter);
static unidict_status ud_stardict_index_activate(unidict *dict, unidict_index_type index_type);
static unidict_status ud_stardict_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                      void *user_data);
static unidict_status ud_stardict_index_external_delete(unidict *dict);

static const unidict_ops stardict_ops = {
    .prepare = NULL,
    .info_get = ud_stardict_info_get,
    .file_infos_get = ud_stardict_file_infos_get,
    .index_activate = ud_stardict_index_activate,
    .index_external_make = ud_stardict_index_external_make,
    .index_external_delete = ud_stardict_index_external_delete,
    .lookup = ud_stardict_lookup,
    .entry_lookup = ud_stardict_entry_lookup,
    .suggest = ud_stardict_suggest,
    .fetch = ud_stardict_fetch,
    .entry_iter_create = ud_stardict_entry_iter_create,
    .entry_iter_next = ud_stardict_entry_iter_next,
    .entry_iter_free = ud_stardict_entry_iter_free,
    .resource_get = NULL,
    .resource_iter_create = NULL,
    .resource_iter_next = NULL,
    .resource_iter_free = NULL,
};

static const uobject_type ud_stardict_type = {
    .name = "ud_stardict",
    .size = sizeof(ud_stardict),
    .release = ud_stardict_release,
};

// ============================================================
// 释放函数
// ============================================================

static void ud_stardict_release(uobject *obj) {
    if (!obj) return;
    ud_stardict *stardict = uobject_cast(obj, ud_stardict, base.obj);

    if (stardict->udx_dict) {
        unidict_close(stardict->udx_dict);
        stardict->udx_dict = NULL;
    }

    if (stardict->sd) {
        sd_stardict_close(stardict->sd);
        stardict->sd = NULL;
    }

    free(stardict->ifo_path);
    free(stardict);
}

// ============================================================
// Helpers
// ============================================================

static char *ud_stardict_get_udx_path(const char *ifo_path) {
    const char *ext = strrchr(ifo_path, '.');
    if (!ext) return NULL;
    size_t base_len = ext - ifo_path;
    char *udx_path = malloc(base_len + 5);
    if (!udx_path) return NULL;
    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, ifo_path);
    return udx_path;
}

static ud_stardict_index *ud_stardict_index_from_udx_value(const udx_value_entry_item *item) {
    if (!item || !item->data || item->size < 8) return NULL;

    ud_stardict_index *idx = calloc(1, sizeof(ud_stardict_index));
    if (!idx) return NULL;

    memcpy(&idx->offset, item->data, 4);
    memcpy(&idx->size, item->data + 4, 4);
    uobject_init(&idx->obj, &ud_stardict_index_type, NULL);

    return idx;
}

// ============================================================
// File List
// ============================================================

static unidict_status ud_stardict_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict) {
        *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);

    unidict_file_info_array *udx_infos = NULL;
    if (stardict->udx_dict) {
        if (stardict->udx_dict->ops->file_infos_get)
            stardict->udx_dict->ops->file_infos_get(stardict->udx_dict, &udx_infos);
    }

    const char *file_paths[5];
    int count = 0;

    if (stardict->sd) {
        sd_stardict_paths paths = sd_stardict_get_paths(stardict->sd);
        if (paths.ifo_path) file_paths[count++] = paths.ifo_path;
        if (paths.idx_path) file_paths[count++] = paths.idx_path;
        if (paths.dict_path) file_paths[count++] = paths.dict_path;
        if (paths.syn_path) file_paths[count++] = paths.syn_path;
    }
    if (udx_infos && udx_infos->count > 0) file_paths[count++] = udx_infos->items[0].path;

    *out_infos = unidict_file_infos_from_paths(file_paths, count);
    if (udx_infos) unidict_file_info_array_free(udx_infos);

    return UNIDICT_OK;
}

// ============================================================
// Index Activate
// ============================================================

static unidict_status ud_stardict_index_activate(unidict *dict, unidict_index_type index_type) {
    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);

    if (stardict->udx_dict) {
        unidict_close(stardict->udx_dict);
        stardict->udx_dict = NULL;
    }
    if (stardict->sd) {
        sd_stardict_close(stardict->sd);
        stardict->sd = NULL;
    }
    dict->active_index = UNIDICT_INDEX_NONE;

    const char *ifo_path = stardict->ifo_path;
    if (!ifo_path) return UNIDICT_ERR_INTERNAL;

    // EXTERNAL or NONE: try UDX first
    if (index_type == UNIDICT_INDEX_EXTERNAL || index_type == UNIDICT_INDEX_NONE) {
        char *udx_path = ud_stardict_get_udx_path(ifo_path);
        if (udx_path) {
            unidict *udx_dict = ud_udx_open(udx_path, NULL);
            free(udx_path);

            if (udx_dict) {
                stardict->udx_dict = udx_dict;
                dict->active_index = UNIDICT_INDEX_EXTERNAL;

                // Still need sd for fetch (reads from .dict file), skip .idx/.syn
                sd_stardict *sd = NULL;
                sd_status st = sd_stardict_open(ifo_path, true, &sd);
                if (st == SD_OK && sd) {
                    stardict->sd = sd;
                }
                return UNIDICT_OK;
            }
        }
    }

    // BUILTIN or NONE fallback: open sd_stardict with full index
    sd_stardict *sd = NULL;
    sd_status st = sd_stardict_open(ifo_path, false, &sd);
    if (st == SD_OK && sd) {
        stardict->sd = sd;
        dict->active_index = UNIDICT_INDEX_BUILTIN;
        return UNIDICT_OK;
    }

    return UNIDICT_ERR_IO;
}

// ============================================================
// 构造函数
// ============================================================

unidict *ud_stardict_open(const char *ifo_path, const unidict_open_options *options) {
    if (!ifo_path) return NULL;

    ud_stardict *stardict = calloc(1, sizeof(ud_stardict));
    if (!stardict) return NULL;

    uobject_init(&stardict->base.obj, &ud_stardict_type, NULL);
    stardict->base.ops = &stardict_ops;
    stardict->base.format = UNIDICT_FORMAT_STARDICT;
    stardict->ifo_path = strdup(ifo_path);
    if (!stardict->ifo_path) {
        ud_stardict_release((uobject *)stardict);
        return NULL;
    }

    stardict->base.has_builtin_index = true;
    stardict->base.has_external_index = unidict_detect_external_index(ifo_path);

    unidict_index_type preset =
        (options && options->index_type != UNIDICT_INDEX_NONE) ? options->index_type : UNIDICT_INDEX_NONE;

    if (ud_stardict_index_activate(&stardict->base, preset) != UNIDICT_OK) {
        ud_stardict_release((uobject *)stardict);
        return NULL;
    }

    return &stardict->base;
}

// ============================================================
// Index External Delete
// ============================================================

static unidict_status ud_stardict_index_external_delete(unidict *dict) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);

    // Switch to builtin mode (closes udx_dict)
    unidict_status st = ud_stardict_index_activate(dict, UNIDICT_INDEX_BUILTIN);
    if (st != UNIDICT_OK) return st;

    // Delete .udx file
    char *udx_path = ud_stardict_get_udx_path(stardict->ifo_path);
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

static unidict_status ud_stardict_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                      void *user_data) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;

    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);

    // Close existing UDX before overwriting
    if (stardict->udx_dict) {
        unidict_close(stardict->udx_dict);
        stardict->udx_dict = NULL;
        dict->active_index = UNIDICT_INDEX_NONE;
    }

    // Ensure sd is open for iterating .idx entries (need full index here)
    if (!stardict->sd || !stardict_get_index(stardict->sd)) {
        if (stardict->sd) {
            sd_stardict_close(stardict->sd);
            stardict->sd = NULL;
        }
        if (!stardict->ifo_path) return UNIDICT_ERR_NO_INDEX;
        sd_stardict *sd = NULL;
        sd_status st = sd_stardict_open(stardict->ifo_path, false, &sd);
        if (st != SD_OK || !sd) return UNIDICT_ERR_NO_INDEX;
        stardict->sd = sd;
    }

    const sd_dictfile_index *idx = stardict_get_index(stardict->sd);
    if (!idx) return UNIDICT_ERR_NO_INDEX;

    uint32_t total = sd_dictfile_index_get_count(idx);
    if (total == 0) return UNIDICT_ERR_NO_INDEX;

    char *udx_path = ud_stardict_get_udx_path(stardict->ifo_path);
    if (!udx_path) return UNIDICT_ERR_NOMEM;

    udx_writer *writer = udx_writer_open(udx_path);
    if (!writer) {
        free(udx_path);
        return UNIDICT_ERR_IO;
    }

    unidict_status ret = UNIDICT_ERR_INTERNAL;

    udx_db_builder *builder = udx_db_builder_create(writer, "article");
    if (!builder) {
        udx_writer_close(writer);
        goto fail;
    }

    int last_pct = 0;
    for (uint32_t i = 0; i < total; i++) {
        const sd_dictfile_index_entry *entry = sd_dictfile_index_get_entry((sd_dictfile_index *)idx, i);
        if (!entry || !entry->word) continue;

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
// 查询实现
// ============================================================

static unidict_status ud_stardict_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);

    // External index mode
    if (stardict->udx_dict) {
        udx_db_value_entry *ve = ud_udx_raw_lookup(stardict->udx_dict, key);
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
            ud_stardict_index *idx = ud_stardict_index_from_udx_value(item);
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
    sd_status st = stardict_entry_lookup(stardict->sd, key, &sd_result);
    if (st == SD_NOT_FOUND) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }
    if (st != SD_OK) {
        *out_entries = NULL;
        return UNIDICT_ERR_INTERNAL;
    }

    unidict_entry_array *res = malloc(sizeof(unidict_entry_array));
    if (!res) {
        sd_index_entry_array_free(sd_result);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->count = sd_result->count;
    res->items = calloc(sd_result->count, sizeof(unidict_entry *));
    if (!res->items) {
        free(res);
        sd_index_entry_array_free(sd_result);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    for (size_t i = 0; i < sd_result->count; i++) {
        sd_dictfile_index_entry *index_entry = sd_result->items[i];

        ud_stardict_index *idx = calloc(1, sizeof(ud_stardict_index));
        if (!idx) continue;
        idx->offset = index_entry->offset;
        idx->size = index_entry->size;
        uobject_init(&idx->obj, &ud_stardict_index_type, NULL);

        unidict_entry *entry = calloc(1, sizeof(unidict_entry));
        if (!entry) {
            uobject_release(&idx->obj);
            continue;
        }

        entry->key = index_entry->word;
        index_entry->word = NULL;
        entry->internal_entry = &idx->obj;
        res->items[i] = entry;
    }

    sd_index_entry_array_free(sd_result);
    *out_entries = res;
    return UNIDICT_OK;
}

static unidict_status ud_stardict_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);

    // External index mode: entry_lookup + fetch
    if (stardict->udx_dict) {
        unidict_entry_array *entries = NULL;
        unidict_status st = ud_stardict_entry_lookup(dict, key, &entries);
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
            ud_stardict_fetch(dict, entries->items[i], &single);
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
    sd_status st = stardict_lookup(stardict->sd, key, &sd_result);
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

static unidict_status ud_stardict_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict) {
        *out_info = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);

    const sd_stardict_ifo *ifo = stardict_get_info(stardict->sd);
    if (!ifo) {
        *out_info = NULL;
        return UNIDICT_OK;
    }

    unidict_info *inf = malloc(sizeof(unidict_info));
    if (!inf) {
        *out_info = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    inf->format = dict->format;
    inf->title = ifo->bookname ? strdup(ifo->bookname) : NULL;
    inf->description = ifo->description ? strdup(ifo->description) : NULL;
    inf->author = ifo->author ? strdup(ifo->author) : NULL;
    inf->creation_date = ifo->date ? strdup(ifo->date) : NULL;
    inf->source_lang = NULL;
    inf->target_lang = NULL;
    inf->word_count = ifo->wordcount;

    *out_info = inf;
    return UNIDICT_OK;
}

static unidict_status ud_stardict_suggest(unidict *dict, const char *prefix, size_t limit,
                                          unidict_entry_array **out_entries) {
    if (!dict || !prefix) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);

    // External index mode
    if (stardict->udx_dict) {
        unidict_entry_array *udx_entries = NULL;
        if (!stardict->udx_dict->ops->suggest) {
            *out_entries = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_status st = stardict->udx_dict->ops->suggest(stardict->udx_dict, prefix, limit, &udx_entries);
        if (st != UNIDICT_OK || !udx_entries) {
            *out_entries = NULL;
            return UNIDICT_OK;
        }

        unidict_entry_array *res = malloc(sizeof(unidict_entry_array));
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

            udx_db_value_entry *ve = ud_udx_raw_fetch(stardict->udx_dict, udx_entry);
            if (!ve || ve->items.count == 0) {
                if (ve) udx_db_value_entry_free(ve);
                continue;
            }

            ud_stardict_index *idx = ud_stardict_index_from_udx_value(&ve->items.elements[0]);
            udx_db_value_entry_free(ve);
            if (!idx) continue;

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (!entry) {
                uobject_release(&idx->obj);
                continue;
            }

            entry->key = strdup(udx_entry->key);
            entry->internal_entry = &idx->obj;
            res->items[i] = entry;
        }

        unidict_entry_array_free(udx_entries);
        *out_entries = res;
        return UNIDICT_OK;
    }

    // Builtin mode
    sd_index_entry_array *sd_result = NULL;
    sd_status st = stardict_suggest(stardict->sd, prefix, limit, &sd_result);
    if (st == SD_NOT_FOUND) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }
    if (st != SD_OK) {
        *out_entries = NULL;
        return UNIDICT_ERR_INTERNAL;
    }

    unidict_entry_array *res = malloc(sizeof(unidict_entry_array));
    if (!res) {
        sd_index_entry_array_free(sd_result);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->count = sd_result->count;
    res->items = calloc(sd_result->count, sizeof(unidict_entry *));
    if (!res->items) {
        free(res);
        sd_index_entry_array_free(sd_result);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    for (size_t i = 0; i < sd_result->count; i++) {
        sd_dictfile_index_entry *index_entry = sd_result->items[i];
        if (!index_entry) continue;

        ud_stardict_index *idx = calloc(1, sizeof(ud_stardict_index));
        if (!idx) continue;

        idx->offset = index_entry->offset;
        idx->size = index_entry->size;
        uobject_init(&idx->obj, &ud_stardict_index_type, NULL);

        unidict_entry *entry = calloc(1, sizeof(unidict_entry));
        if (!entry) {
            uobject_release(&idx->obj);
            continue;
        }

        entry->key = strdup(index_entry->word);
        entry->internal_entry = &idx->obj;
        res->items[i] = entry;
    }

    sd_index_entry_array_free(sd_result);
    *out_entries = res;
    return UNIDICT_OK;
}

static unidict_status ud_stardict_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);
    ud_stardict_index *idx = uobject_cast(entry->internal_entry, ud_stardict_index, obj);

    sd_dictfile_index_entry temp = {.word = NULL, .offset = idx->offset, .size = idx->size};

    char *body = NULL;
    sd_status st = stardict_fetch(stardict->sd, &temp, &body);
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

// ============================================================
// Entry Iterator
// ============================================================

typedef struct {
    unidict_entry_iter base;
    ud_stardict_index current_idx;
    uint32_t pos;
    uint32_t count;
    const sd_dictfile_index *index;
} ud_stardict_entry_iter;

static unidict_entry_iter *ud_stardict_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;
    ud_stardict *stardict = uobject_cast(&dict->obj, ud_stardict, base.obj);

    const sd_dictfile_index *idx = stardict_get_index(stardict->sd);
    if (!idx) return NULL;

    ud_stardict_entry_iter *iter = calloc(1, sizeof(ud_stardict_entry_iter));
    if (!iter) return NULL;

    iter->base.dict = dict;
    iter->base.current.key = NULL;
    iter->base.current.internal_entry = &iter->current_idx.obj;
    iter->pos = 0;
    iter->count = sd_dictfile_index_get_count(idx);
    iter->index = idx;

    uobject_init(&iter->current_idx.obj, &ud_stardict_index_type, NULL);

    return (unidict_entry_iter *)iter;
}

static unidict_status ud_stardict_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict) {
        if (out_entry) *out_entry = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_stardict_entry_iter *stardict_iter = (ud_stardict_entry_iter *)iter;

    if (stardict_iter->pos >= stardict_iter->count) {
        *out_entry = NULL;
        return UNIDICT_DONE;
    }

    const sd_dictfile_index_entry *entry =
        sd_dictfile_index_get_entry((sd_dictfile_index *)stardict_iter->index, stardict_iter->pos);
    stardict_iter->pos++;

    if (!entry) {
        *out_entry = NULL;
        return UNIDICT_DONE;
    }

    free(iter->current.key);
    iter->current.key = strdup(entry->word);
    if (!iter->current.key) {
        *out_entry = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    stardict_iter->current_idx.offset = entry->offset;
    stardict_iter->current_idx.size = entry->size;

    *out_entry = &iter->current;
    return UNIDICT_OK;
}

static void ud_stardict_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;
    free(iter->current.key);
    // current_idx is an embedded member of iter (not separately allocated),
    // so it must NOT be uobject_release()'d — it is freed together with iter.
    free(iter);
}
