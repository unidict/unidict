//
//  ud_lingvo_dsl.c
//  unidict
//
//  Created by kejinlu on 2026-05-27
//
#include "ud_lingvo_dsl.h"
#include "unidict_internal.h"
#include "unidict_log.h"
#include "ud_udx.h"
#include "dsl_reader.h"
#include "udx_writer.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

// ============================================================
// Private struct
// ============================================================

typedef struct {
    unidict base;
    char *dsl_path;

    dsl_reader *reader;
    unidict *udx_dict;
} ud_lingvo_dsl;

// ============================================================
// Type definition
// ============================================================

static void ud_lingvo_dsl_release(uobject *obj);

static const uobject_type ud_lingvo_dsl_type = {
    .name = "ud_lingvo_dsl",
    .size = sizeof(ud_lingvo_dsl),
    .release = ud_lingvo_dsl_release,
};

// ============================================================
// Release
// ============================================================

static void ud_lingvo_dsl_release(uobject *obj) {
    if (!obj) return;
    ud_lingvo_dsl *dsl = uobject_cast(obj, ud_lingvo_dsl, base.obj);

    if (dsl->udx_dict) {
        unidict_close(dsl->udx_dict);
        dsl->udx_dict = NULL;
    }
    if (dsl->reader) {
        dsl_reader_close(dsl->reader);
        dsl->reader = NULL;
    }
    free(dsl->dsl_path);
    free(dsl);
}

// ============================================================
// Helpers
// ============================================================

// Check if path ends with .dsl (handles both .dsl and .dsl.dz)
static bool is_dsl_extension(const char *path) {
    if (!path) return false;
    size_t len = strlen(path);
    if (len < 4) return false;

    // .dsl.dz
    if (len >= 7 && strcasecmp(path + len - 7, ".dsl.dz") == 0) return true;
    // .dsl
    if (strcasecmp(path + len - 4, ".dsl") == 0) return true;

    return false;
}

// Derive .udx path from .dsl or .dsl.dz path
static char *dsl_get_udx_path(const char *dsl_path) {
    size_t len = strlen(dsl_path);

    size_t base_len;
    if (len >= 7 && strcasecmp(dsl_path + len - 7, ".dsl.dz") == 0) {
        base_len = len - 7;
    } else if (len >= 4 && strcasecmp(dsl_path + len - 4, ".dsl") == 0) {
        base_len = len - 4;
    } else {
        return NULL;
    }

    char *udx_path = malloc(base_len + 5);
    if (!udx_path) return NULL;
    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, dsl_path);
    return udx_path;
}

// ============================================================
// Forward declarations
// ============================================================

static unidict_status ud_lingvo_dsl_index_activate(unidict *dict, unidict_index_type index_type);
static unidict_status ud_lingvo_dsl_info_get(unidict *dict, unidict_info **out_info);
static unidict_status ud_lingvo_dsl_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status ud_lingvo_dsl_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                        void *user_data);
static unidict_status ud_lingvo_dsl_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status ud_lingvo_dsl_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status ud_lingvo_dsl_suggest(unidict *dict, const char *prefix, size_t limit,
                                            unidict_entry_array **out_entries);
static unidict_status ud_lingvo_dsl_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_entry_iter *ud_lingvo_dsl_entry_iter_create(unidict *dict);
static unidict_status ud_lingvo_dsl_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void ud_lingvo_dsl_entry_iter_free(unidict_entry_iter *iter);
static unidict_status ud_lingvo_dsl_index_external_delete(unidict *dict);

// ============================================================
// Virtual function table
// ============================================================

static const unidict_ops lingvo_dsl_ops = {
    .prepare = NULL,
    .info_get = ud_lingvo_dsl_info_get,
    .file_infos_get = ud_lingvo_dsl_file_infos_get,
    .index_activate = ud_lingvo_dsl_index_activate,
    .index_external_make = ud_lingvo_dsl_index_external_make,
    .index_external_delete = ud_lingvo_dsl_index_external_delete,
    .lookup = ud_lingvo_dsl_lookup,
    .entry_lookup = ud_lingvo_dsl_entry_lookup,
    .suggest = ud_lingvo_dsl_suggest,
    .fetch = ud_lingvo_dsl_fetch,
    .entry_iter_create = ud_lingvo_dsl_entry_iter_create,
    .entry_iter_next = ud_lingvo_dsl_entry_iter_next,
    .entry_iter_free = ud_lingvo_dsl_entry_iter_free,
    .resource_get = NULL,
    .resource_iter_create = NULL,
    .resource_iter_next = NULL,
    .resource_iter_free = NULL,
};

// ============================================================
// Index activate
// ============================================================

static unidict_status ud_lingvo_dsl_index_activate(unidict *dict, unidict_index_type index_type) {
    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);

    if (dsl->udx_dict) {
        unidict_close(dsl->udx_dict);
        dsl->udx_dict = NULL;
    }
    if (dsl->reader) {
        dsl_reader_close(dsl->reader);
        dsl->reader = NULL;
    }
    dict->active_index = UNIDICT_INDEX_NONE;

    // EXTERNAL or NONE: try UDX first
    // BUILTIN: skip UDX, go directly to raw reader (DSL has no builtin index,
    // BUILTIN means "raw mode without external index")
    if (index_type == UNIDICT_INDEX_EXTERNAL || index_type == UNIDICT_INDEX_NONE) {
        char *udx_path = dsl_get_udx_path(dsl->dsl_path);
        if (udx_path) {
            unidict *udx_dict = ud_udx_open(udx_path, NULL);
            free(udx_path);

            if (udx_dict) {
                dsl->udx_dict = udx_dict;
                dict->active_index = UNIDICT_INDEX_EXTERNAL;
                return UNIDICT_OK;
            }
        }
    }

    // BUILTIN or fallback: open DSL reader (raw mode, no index)
    dsl_reader *reader = NULL;
    lsd_status st = dsl_reader_open(dsl->dsl_path, &reader);
    if (st != LSD_OK) return UNIDICT_ERR_IO;

    dsl->reader = reader;
    // DSL has no builtin index, active_index stays NONE
    return UNIDICT_OK;
}

// ============================================================
// Constructor
// ============================================================

unidict *ud_lingvo_dsl_open(const char *file_path, const unidict_open_options *options) {
    if (!file_path || !is_dsl_extension(file_path)) return NULL;

    ud_lingvo_dsl *dsl = calloc(1, sizeof(ud_lingvo_dsl));
    if (!dsl) return NULL;

    uobject_init(&dsl->base.obj, &ud_lingvo_dsl_type, NULL);
    dsl->base.ops = &lingvo_dsl_ops;
    dsl->base.format = UNIDICT_FORMAT_DSL;

    dsl->dsl_path = strdup(file_path);
    if (!dsl->dsl_path) {
        free(dsl);
        return NULL;
    }

    dsl->base.has_builtin_index = false;
    dsl->base.has_external_index = unidict_detect_external_index(file_path);

    unidict_index_type preset =
        (options && options->index_type != UNIDICT_INDEX_NONE) ? options->index_type : UNIDICT_INDEX_NONE;

    if (ud_lingvo_dsl_index_activate((unidict *)dsl, preset) != UNIDICT_OK) {
        free(dsl->dsl_path);
        free(dsl);
        return NULL;
    }

    return (unidict *)dsl;
}

// ============================================================
// Delegated UDX ops
// ============================================================

static unidict_status ud_lingvo_dsl_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key) {
        if (out_articles) *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);
    if (!dsl->udx_dict) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }
    if (!dsl->udx_dict->ops->lookup) {
        *out_articles = NULL;
        return UNIDICT_ERR_NOT_SUPPORTED;
    }
    return dsl->udx_dict->ops->lookup(dsl->udx_dict, key, out_articles);
}

static unidict_status ud_lingvo_dsl_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key) {
        if (out_entries) *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);
    if (!dsl->udx_dict) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }
    if (!dsl->udx_dict->ops->entry_lookup) {
        *out_entries = NULL;
        return UNIDICT_ERR_NOT_SUPPORTED;
    }
    return dsl->udx_dict->ops->entry_lookup(dsl->udx_dict, key, out_entries);
}

static unidict_status ud_lingvo_dsl_suggest(unidict *dict, const char *prefix, size_t limit,
                                            unidict_entry_array **out_entries) {
    if (!dict || !prefix) {
        if (out_entries) *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);
    if (!dsl->udx_dict) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }
    if (!dsl->udx_dict->ops->suggest) {
        *out_entries = NULL;
        return UNIDICT_ERR_NOT_SUPPORTED;
    }
    return dsl->udx_dict->ops->suggest(dsl->udx_dict, prefix, limit, out_entries);
}

static unidict_status ud_lingvo_dsl_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry) {
        if (out_articles) *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);
    if (!dsl->udx_dict) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }
    if (!dsl->udx_dict->ops->fetch) {
        *out_articles = NULL;
        return UNIDICT_ERR_NOT_SUPPORTED;
    }
    return dsl->udx_dict->ops->fetch(dsl->udx_dict, entry, out_articles);
}

// ============================================================
// Entry iterator (delegated to UDX)
// ============================================================

static unidict_entry_iter *ud_lingvo_dsl_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;
    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);
    if (!dsl->udx_dict) return NULL;
    if (!dsl->udx_dict->ops->entry_iter_create) return NULL;
    return dsl->udx_dict->ops->entry_iter_create(dsl->udx_dict);
}

static unidict_status ud_lingvo_dsl_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict) {
        if (out_entry) *out_entry = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    return iter->dict->ops->entry_iter_next(iter, out_entry);
}

static void ud_lingvo_dsl_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;
    iter->dict->ops->entry_iter_free(iter);
}

// ============================================================
// Info
// ============================================================

static unidict_status ud_lingvo_dsl_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict) {
        if (out_info) *out_info = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);
    unidict_info *res = calloc(1, sizeof(unidict_info));
    if (!res) {
        *out_info = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->format = UNIDICT_FORMAT_DSL;

    if (dsl->udx_dict) {
        unidict_info *udx_info = NULL;
        if (dsl->udx_dict->ops->info_get) dsl->udx_dict->ops->info_get(dsl->udx_dict, &udx_info);
        res->title = (udx_info && udx_info->title) ? strdup(udx_info->title) : strdup("DSL Dictionary");
        res->word_count = udx_info ? udx_info->word_count : 0;
        if (udx_info) unidict_info_free(udx_info);
    } else if (dsl->reader) {
        const dsl_header *hdr = dsl_reader_get_header(dsl->reader);
        if (hdr && hdr->name) {
            res->title = strdup(hdr->name);
        } else {
            char *name = NULL;
            dsl_reader_get_name(dsl->reader, &name);
            res->title = name ? name : strdup("DSL Dictionary");
        }
        if (hdr && hdr->index_language) res->source_lang = strdup(hdr->index_language);
        if (hdr && hdr->contents_language) res->target_lang = strdup(hdr->contents_language);
    } else {
        res->title = strdup("DSL Dictionary");
    }

    *out_info = res;
    return UNIDICT_OK;
}

// ============================================================
// File list
// ============================================================

static unidict_status ud_lingvo_dsl_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict) {
        if (out_infos) *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);

    unidict_file_info_array *udx_infos = NULL;
    if (dsl->udx_dict) {
        if (dsl->udx_dict->ops->file_infos_get) dsl->udx_dict->ops->file_infos_get(dsl->udx_dict, &udx_infos);
    }

    int udx_count = udx_infos ? (int)udx_infos->count : 0;
    const char *paths[2];
    int count = 0;

    if (dsl->dsl_path) paths[count++] = dsl->dsl_path;
    if (udx_infos && udx_count > 0) paths[count++] = udx_infos->items[0].path;

    *out_infos = unidict_file_infos_from_paths(paths, count);
    if (udx_infos) unidict_file_info_array_free(udx_infos);

    return *out_infos ? UNIDICT_OK : UNIDICT_ERR_NOMEM;
}

// ============================================================
// Index external delete
// ============================================================

static unidict_status ud_lingvo_dsl_index_external_delete(unidict *dict) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);

    // Switch to raw reader mode (closes udx_dict)
    unidict_status st = ud_lingvo_dsl_index_activate(dict, UNIDICT_INDEX_BUILTIN);
    if (st != UNIDICT_OK) return st;

    // Delete .udx file
    char *udx_path = dsl_get_udx_path(dsl->dsl_path);
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
// Index external make
// ============================================================

static unidict_status ud_lingvo_dsl_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                        void *user_data) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;

    ud_lingvo_dsl *dsl = uobject_cast(&dict->obj, ud_lingvo_dsl, base.obj);

    // Lazy load DSL reader if needed
    if (!dsl->reader) {
        dsl_reader *reader = NULL;
        lsd_status st = dsl_reader_open(dsl->dsl_path, &reader);
        if (st != LSD_OK) return UNIDICT_ERR_IO;
        dsl->reader = reader;
    }

    char *udx_path = dsl_get_udx_path(dsl->dsl_path);
    if (!udx_path) return UNIDICT_ERR_INTERNAL;

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

    dsl_article_iter *iter = dsl_article_iter_create(dsl->reader);
    if (!iter) {
        udx_db_builder_finalize(builder);
        udx_writer_close(writer);
        goto fail;
    }

    int entry_count = 0;
    int last_pct = 0;
    const dsl_article *art = NULL;

    UD_LOG_INFO("Building UDX index from DSL: %s", dsl->dsl_path);

    while (dsl_article_iter_next(iter, &art) == LSD_OK) {
        // Process parent article + all sub-articles
        for (int si = -1; si < art->sub_article_count; si++) {
            const dsl_article *a = (si < 0) ? art : &art->sub_articles[si];
            if (a->heading_count == 0 || !a->definition) continue;

            size_t def_len = a->definition_length;

            udx_value_address address =
                udx_db_builder_add_value(builder, (const uint8_t *)a->definition, (uint32_t)def_len);
            if (address == UDX_INVALID_ADDRESS) continue;

            for (int h = 0; h < a->heading_count; h++) {
                for (int k = 0; k < a->headings[h].key_count; k++) {
                    udx_db_builder_add_key_entry(builder, a->headings[h].keys[k], address, (uint32_t)def_len);
                    entry_count++;
                }
            }
        }

        if (callback && (entry_count % 500) == 0) {
            int pct = (entry_count / 50) * 5; // rough estimate
            if (pct > 100) pct = 100;
            if (pct > last_pct) {
                last_pct = pct;
                if (!callback(dict, UNIDICT_INDEX_STAGE_ARTICLES, pct, user_data)) {
                    dsl_article_iter_destroy(iter);
                    udx_db_builder_finalize(builder);
                    udx_writer_close(writer);
                    ret = UNIDICT_ERR_CANCELLED;
                    goto fail;
                }
            }
        }
    }

    dsl_article_iter_destroy(iter);

    UD_LOG_INFO("DSL -> UDX: %d entries", entry_count);

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
