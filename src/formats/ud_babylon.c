//
//  ud_babylon.c
//  unidict
//
//  Created by kejinlu on 2026-02-02
//
#include "ud_babylon.h"
#include "unidict_internal.h"
#include "unidict_log.h"
#include "ud_udx.h"
#include "ud_mime.h"
#include "bgl_reader.h"
#include "bgl_definition.h"
#include "bgl_info.h"
#include "bgl_text.h"
#include "udx_writer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _MSC_VER
#include "ud_compat.h"
#else
#include <strings.h>
#endif
#include <errno.h>

// ============================================================
// Forward Declarations
// ============================================================
static void ud_babylon_release(uobject *obj);
static unidict_status ud_babylon_prepare(unidict *dict);

typedef struct {
    unidict base;
    char *bgl_path;
    bgl_reader *reader;
    unidict *udx_dict;
} ud_babylon;

// ============================================================
// Type Definition
// ============================================================
static const uobject_type ud_babylon_type = {
    .name = "ud_babylon",
    .size = sizeof(ud_babylon),
    .release = ud_babylon_release,
};

// ============================================================
// Release
// ============================================================

static void ud_babylon_release(uobject *obj) {
    if (!obj) return;

    ud_babylon *babylon = uobject_cast(obj, ud_babylon, base.obj);

    if (babylon->udx_dict) {
        unidict_close(babylon->udx_dict);
        babylon->udx_dict = NULL;
    }

    if (babylon->reader) {
        bgl_reader_close(babylon->reader);
        babylon->reader = NULL;
    }

    free(babylon->bgl_path);
    free(babylon);
}

// ============================================================
// Virtual Function Table (Babylon Implementation)
// ============================================================
static unidict_status ud_babylon_info_get(unidict *dict, unidict_info **out_info);
static unidict_status ud_babylon_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status ud_babylon_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                     void *user_data);
static unidict_status ud_babylon_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status ud_babylon_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status ud_babylon_suggest(unidict *dict, const char *prefix, size_t limit,
                                         unidict_entry_array **out_entries);
static unidict_status ud_babylon_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_entry_iter *ud_babylon_entry_iter_create(unidict *dict);
static unidict_status ud_babylon_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void ud_babylon_entry_iter_free(unidict_entry_iter *iter);
static unidict_status ud_babylon_resource_get(unidict *dict, const char *key, unidict_resource **out_res);
static unidict_resource_iter *ud_babylon_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode);
static unidict_status ud_babylon_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res);
static void ud_babylon_resource_iter_free(unidict_resource_iter *iter);
static unidict_status ud_babylon_index_activate(unidict *dict, unidict_index_type index_type);
static unidict_status ud_babylon_index_external_delete(unidict *dict);

static const unidict_ops babylon_ops = {
    .prepare = ud_babylon_prepare,
    .info_get = ud_babylon_info_get,
    .file_infos_get = ud_babylon_file_infos_get,
    .index_external_make = ud_babylon_index_external_make,
    .index_external_delete = ud_babylon_index_external_delete,
    .lookup = ud_babylon_lookup,
    .entry_lookup = ud_babylon_entry_lookup,
    .suggest = ud_babylon_suggest,
    .fetch = ud_babylon_fetch,
    .entry_iter_create = ud_babylon_entry_iter_create,
    .entry_iter_next = ud_babylon_entry_iter_next,
    .entry_iter_free = ud_babylon_entry_iter_free,
    .resource_get = ud_babylon_resource_get,
    .resource_iter_create = ud_babylon_resource_iter_create,
    .resource_iter_next = ud_babylon_resource_iter_next,
    .resource_iter_free = ud_babylon_resource_iter_free,
    .index_activate = ud_babylon_index_activate,
};

// ============================================================
// Load
// ============================================================

static unidict_status ud_babylon_index_activate(unidict *dict, unidict_index_type index_type) {
    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);

    // Clean up existing resources
    if (babylon->udx_dict) {
        unidict_close(babylon->udx_dict);
        babylon->udx_dict = NULL;
    }
    if (babylon->reader) {
        bgl_reader_close(babylon->reader);
        babylon->reader = NULL;
    }
    dict->active_index = UNIDICT_INDEX_NONE;

    const char *bgl_path = babylon->bgl_path;
    const char *ext = bgl_path ? strrchr(bgl_path, '.') : NULL;
    if (!ext) return UNIDICT_ERR_INTERNAL;

    size_t base_len = ext - bgl_path;

    // EXTERNAL or NONE: try UDX first
    // BUILTIN: skip UDX, go directly to raw reader (Babylon has no builtin index,
    // BUILTIN means "raw mode without external index")
    if (index_type == UNIDICT_INDEX_EXTERNAL || index_type == UNIDICT_INDEX_NONE) {
        char *udx_path = (char *)malloc(base_len + 5);
        if (!udx_path) return UNIDICT_ERR_NOMEM;
        snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, bgl_path);

        unidict *udx_dict = ud_udx_open(udx_path, NULL);
        free(udx_path);

        if (udx_dict) {
            babylon->udx_dict = udx_dict;
            dict->active_index = UNIDICT_INDEX_EXTERNAL;
            return UNIDICT_OK;
        }
    }

    // BUILTIN or NONE fallback: open BGL reader (raw mode)
    bgl_reader *reader = bgl_reader_open(bgl_path);
    if (!reader) return UNIDICT_ERR_IO;

    babylon->reader = reader;
    // babylon has no builtin index, active_index stays NONE
    return UNIDICT_OK;
}

// ============================================================
// Constructor/Destructor
// ============================================================

unidict *ud_babylon_open(const char *bgl_path, const unidict_open_options *options) {
    if (!bgl_path) return NULL;

    const char *ext = strrchr(bgl_path, '.');
    if (!ext || strcasecmp(ext, ".bgl") != 0) return NULL;

    ud_babylon *babylon = (ud_babylon *)calloc(1, sizeof(ud_babylon));
    if (!babylon) return NULL;

    uobject_init(&babylon->base.obj, &ud_babylon_type, NULL);
    babylon->base.ops = &babylon_ops;
    babylon->base.format = UNIDICT_FORMAT_BABYLON;
    babylon->bgl_path = strdup(bgl_path);
    if (!babylon->bgl_path) {
        ud_babylon_release((uobject *)babylon);
        return NULL;
    }

    // Detect capabilities
    babylon->base.has_builtin_index = false;
    babylon->base.has_external_index = unidict_detect_external_index(bgl_path);

    // Set preset and load
    unidict_index_type preset =
        (options && options->index_type != UNIDICT_INDEX_NONE) ? options->index_type : UNIDICT_INDEX_NONE;

    if (ud_babylon_index_activate((unidict *)babylon, preset) != UNIDICT_OK) {
        ud_babylon_release((uobject *)babylon);
        return NULL;
    }

    return (unidict *)babylon;
}

static unidict_status ud_babylon_prepare(unidict *dict) {
    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);

    // UDX path: already prepared during open
    if (babylon->udx_dict) {
        return UNIDICT_OK;
    }

    // BGL path: prepare the reader (full gzip stream scan)
    if (babylon->reader) {
        if (bgl_reader_prepare(babylon->reader) != 0) {
            return UNIDICT_ERR_INTERNAL;
        }
    }

    return UNIDICT_OK;
}

// ============================================================
// Delegated UDX ops
// ============================================================

static unidict_status ud_babylon_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key) {
        if (out_articles) *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);
    if (!babylon->udx_dict) {
        UD_LOG_ERROR("no index available, build external index first");
        *out_articles = NULL;
        return UNIDICT_ERR_NO_INDEX;
    }
    if (!babylon->udx_dict->ops->lookup) {
        *out_articles = NULL;
        return UNIDICT_ERR_NOT_SUPPORTED;
    }
    return babylon->udx_dict->ops->lookup(babylon->udx_dict, key, out_articles);
}

static unidict_status ud_babylon_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key) {
        if (out_entries) *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);
    if (!babylon->udx_dict) {
        UD_LOG_ERROR("no index available, build external index first");
        *out_entries = NULL;
        return UNIDICT_ERR_NO_INDEX;
    }
    if (!babylon->udx_dict->ops->entry_lookup) {
        *out_entries = NULL;
        return UNIDICT_ERR_NOT_SUPPORTED;
    }
    return babylon->udx_dict->ops->entry_lookup(babylon->udx_dict, key, out_entries);
}

static unidict_status ud_babylon_suggest(unidict *dict, const char *prefix, size_t limit,
                                         unidict_entry_array **out_entries) {
    if (!dict || !prefix) {
        if (out_entries) *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);
    if (!babylon->udx_dict) {
        UD_LOG_ERROR("no index available, build external index first");
        *out_entries = NULL;
        return UNIDICT_ERR_NO_INDEX;
    }
    if (!babylon->udx_dict->ops->suggest) {
        *out_entries = NULL;
        return UNIDICT_ERR_NOT_SUPPORTED;
    }
    return babylon->udx_dict->ops->suggest(babylon->udx_dict, prefix, limit, out_entries);
}

static unidict_status ud_babylon_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry) {
        if (out_articles) *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);
    if (!babylon->udx_dict) {
        UD_LOG_ERROR("no index available, build external index first");
        *out_articles = NULL;
        return UNIDICT_ERR_NO_INDEX;
    }
    if (!babylon->udx_dict->ops->fetch) {
        *out_articles = NULL;
        return UNIDICT_ERR_NOT_SUPPORTED;
    }
    return babylon->udx_dict->ops->fetch(babylon->udx_dict, entry, out_articles);
}

static unidict_entry_iter *ud_babylon_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;

    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);
    if (!babylon->udx_dict) return NULL;

    if (!babylon->udx_dict->ops->entry_iter_create) return NULL;
    return babylon->udx_dict->ops->entry_iter_create(babylon->udx_dict);
}

static unidict_status ud_babylon_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict) {
        if (out_entry) *out_entry = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    return iter->dict->ops->entry_iter_next(iter, out_entry);
}

static void ud_babylon_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;
    iter->dict->ops->entry_iter_free(iter);
}

static unidict_status ud_babylon_resource_get(unidict *dict, const char *key, unidict_resource **out_res) {
    if (!dict || !key) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);
    if (!babylon->udx_dict) {
        UD_LOG_ERROR("no index available, build external index first");
        *out_res = NULL;
        return UNIDICT_ERR_NO_INDEX;
    }
    if (!babylon->udx_dict->ops->resource_get) {
        *out_res = NULL;
        return UNIDICT_ERR_NOT_SUPPORTED;
    }
    return babylon->udx_dict->ops->resource_get(babylon->udx_dict, key, out_res);
}

static unidict_resource_iter *ud_babylon_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode) {
    if (!dict) return NULL;

    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);
    if (!babylon->udx_dict) return NULL;

    if (!babylon->udx_dict->ops->resource_iter_create) return NULL;
    return babylon->udx_dict->ops->resource_iter_create(babylon->udx_dict, mode);
}

static unidict_status ud_babylon_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res) {
    if (!iter || !out_res) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    return iter->dict->ops->resource_iter_next(iter, out_res);
}

static void ud_babylon_resource_iter_free(unidict_resource_iter *iter) {
    if (!iter) return;
    iter->dict->ops->resource_iter_free(iter);
}

// ============================================================
// Icon
// ============================================================

// ============================================================
// Info
// ============================================================

static unidict_status ud_babylon_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict) {
        if (out_info) *out_info = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);

    if (babylon->udx_dict) {
        if (!babylon->udx_dict->ops || !babylon->udx_dict->ops->info_get) {
            *out_info = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_status ret = babylon->udx_dict->ops->info_get(babylon->udx_dict, out_info);
        if (ret == UNIDICT_OK && *out_info) {
            (*out_info)->format = UNIDICT_FORMAT_BABYLON;
        }
        return ret;
    }

    unidict_info *info = calloc(1, sizeof(unidict_info));
    if (!info) {
        *out_info = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    const bgl_info *bglinfo = bgl_get_info(babylon->reader);
    info->format = UNIDICT_FORMAT_BABYLON;
    info->title = bglinfo->title ? strdup(bglinfo->title) : strdup("Babylon Dictionary");
    info->description = bglinfo->description ? strdup(bglinfo->description) : NULL;
    info->word_count = (uint64_t)bgl_get_entry_count(babylon->reader);
    info->author = bglinfo->author ? strdup(bglinfo->author) : NULL;
    info->email = bglinfo->email ? strdup(bglinfo->email) : NULL;
    info->creation_date = NULL;
    info->source_lang = bglinfo->source_lang ? strdup(bglinfo->source_lang) : NULL;
    info->target_lang = bglinfo->target_lang ? strdup(bglinfo->target_lang) : NULL;

    // Icon
    if (bglinfo->icon && bglinfo->icon_size > 0) {
        info->icon_data = malloc(bglinfo->icon_size);
        if (info->icon_data) {
            memcpy(info->icon_data, bglinfo->icon, bglinfo->icon_size);
            info->icon_size = bglinfo->icon_size;
            info->icon_mime_type = strdup(ud_detect_image_mime(bglinfo->icon, bglinfo->icon_size));
        }
    }

    *out_info = info;
    return UNIDICT_OK;
}

// ============================================================
// File List
// ============================================================

static unidict_status ud_babylon_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict) {
        if (out_infos) *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);

    // Get UDX file list from ud_udx
    unidict_file_info_array *udx_infos = NULL;
    if (babylon->udx_dict) {
        if (babylon->udx_dict->ops->file_infos_get)
            babylon->udx_dict->ops->file_infos_get(babylon->udx_dict, &udx_infos);
    }

    int udx_count = udx_infos ? (int)udx_infos->count : 0;
    const char *paths[2];
    int count = 0;

    if (babylon->bgl_path) paths[count++] = babylon->bgl_path;
    if (udx_infos && udx_count > 0) paths[count++] = udx_infos->items[0].path;

    *out_infos = unidict_file_infos_from_paths(paths, count);
    if (udx_infos) unidict_file_info_array_free(udx_infos);

    return *out_infos ? UNIDICT_OK : UNIDICT_ERR_NOMEM;
}

// ============================================================
// Index External Delete
// ============================================================

static unidict_status ud_babylon_index_external_delete(unidict *dict) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);

    // Switch to raw reader mode (closes udx_dict)
    unidict_status st = ud_babylon_index_activate(dict, UNIDICT_INDEX_BUILTIN);
    if (st != UNIDICT_OK) return st;

    // Delete .udx file
    const char *bgl_path = babylon->bgl_path;
    const char *ext = strrchr(bgl_path, '.');
    if (!ext) return UNIDICT_ERR_INTERNAL;
    size_t base_len = ext - bgl_path;
    char *udx_path = malloc(base_len + 5);
    if (!udx_path) return UNIDICT_ERR_NOMEM;
    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, bgl_path);

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

static unidict_status ud_babylon_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                     void *user_data) {
    if (!dict) {
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_babylon *babylon = uobject_cast(&dict->obj, ud_babylon, base.obj);

    // Close existing UDX before overwriting
    if (babylon->udx_dict) {
        unidict_close(babylon->udx_dict);
        babylon->udx_dict = NULL;
        dict->active_index = UNIDICT_INDEX_NONE;
    }

    bgl_reader *reader = babylon->reader;

    // Lazy load BGL reader if not already loaded
    if (!reader) {
        if (!babylon->bgl_path) {
            UD_LOG_ERROR("Cannot determine BGL file path - bgl_path is NULL");
            return UNIDICT_ERR_NO_INDEX;
        }

        UD_LOG_INFO("Lazy loading BGL reader: %s", babylon->bgl_path);

        reader = bgl_reader_open(babylon->bgl_path);

        if (!reader) {
            UD_LOG_ERROR("Failed to open BGL file for index creation");
            return UNIDICT_ERR_IO;
        }

        if (bgl_reader_prepare(reader) != 0) {
            bgl_reader_close(reader);
            UD_LOG_ERROR("Failed to prepare BGL reader for index creation");
            return UNIDICT_ERR_INTERNAL;
        }
        babylon->reader = reader;
    }

    if (!reader || !babylon->bgl_path) {
        return UNIDICT_ERR_NO_INDEX;
    }

    // Generate output path by replacing .bgl extension with .udx
    const char *bgl_path = babylon->bgl_path;

    const char *ext = strrchr(bgl_path, '.');
    if (!ext || (strcasecmp(ext, ".bgl") != 0)) {
        UD_LOG_ERROR("BGL file path does not have .bgl extension: %s", bgl_path);
        return UNIDICT_ERR_INVALID_PARAM;
    }

    size_t base_len = ext - bgl_path;
    char *udx_path = (char *)malloc(base_len + 5);
    if (!udx_path) {
        UD_LOG_ERROR("Failed to allocate memory for UDX path");
        return UNIDICT_ERR_NOMEM;
    }

    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, bgl_path);
    UD_LOG_INFO("Building UDX index: %s -> %s", bgl_path, udx_path);

    // Open UDX writer
    udx_writer *writer = udx_writer_open(udx_path);
    if (!writer) {
        UD_LOG_ERROR("Failed to open UDX writer for: %s", udx_path);
        free(udx_path);
        return UNIDICT_ERR_IO;
    }

    unidict_status ret = UNIDICT_ERR_INTERNAL;

    // Build info metadata from BGL info
    const bgl_info *bglinfo = bgl_get_info(reader);
    unidict_info meta = {0};
    meta.title = bglinfo->title;
    meta.description = bglinfo->description;
    meta.author = bglinfo->author;
    meta.email = bglinfo->email;
    meta.source_lang = bglinfo->source_lang;
    meta.target_lang = bglinfo->target_lang;
    char *meta_xml = unidict_info_to_xml(&meta);

    // Create entry database builder (with metadata)
    udx_db_builder *builder = NULL;
    if (meta_xml) {
        builder = udx_db_builder_create_with_metadata(writer, "article",
                    (const uint8_t *)meta_xml, (uint32_t)strlen(meta_xml));
        free(meta_xml);
    } else {
        builder = udx_db_builder_create(writer, "article");
    }
    if (!builder) {
        UD_LOG_ERROR("Failed to create UDX database builder");
        udx_writer_close(writer);
        goto fail;
    }

    // Create iterator to read all entries
    bgl_entry_iterator *iter = bgl_entry_iterator_create(reader);
    if (!iter) {
        UD_LOG_ERROR("Failed to create BGL iterator");
        udx_db_builder_finalize(builder);
        udx_writer_close(writer);
        goto fail;
    }

    // Iterate through all entries and add to UDX
    int entry_count = 0;
    int error_count = 0;
    int total_entries = bgl_get_entry_count(reader);
    int last_pct = 0;
    int processed = 0;

    UD_LOG_INFO("Reading entries from BGL file...");

    const bgl_entry *entry;
    while (bgl_entry_iterator_next(iter, &entry) == BGL_OK) {
        char *formatted_def = bgl_format_definition(&entry->def);
        if (!formatted_def) {
            UD_LOG_ERROR("Failed to format definition for entry '%s'", entry->word);
            continue;
        }

        size_t def_len = strlen(formatted_def);

        udx_value_address address =
            udx_db_builder_add_value(builder, (const uint8_t *)formatted_def, (uint32_t)def_len);
        if (address == UDX_INVALID_ADDRESS) {
            UD_LOG_ERROR("Failed to add chunk for entry '%s'", entry->word);
            free(formatted_def);
            error_count++;
            continue;
        }

        char *word = strdup(entry->word);
        if (!word) {
            free(formatted_def);
            error_count++;
            continue;
        }
        bgl_strip_dollar_indexes(&word);

        udx_status err = udx_db_builder_add_key_entry(builder, word, address, (uint32_t)def_len);
        free(word);
        if (err != UDX_OK) {
            UD_LOG_ERROR("Failed to add word entry '%s' to UDX: error %d", entry->word, err);
            free(formatted_def);
            error_count++;
            continue;
        }

        entry_count++;

        for (int i = 0; i < entry->alternate_count; i++) {
            // Skip NULL alternates and those starting with '/' (redundant duplicates)
            if (!entry->alternates[i] || entry->alternates[i][0] == '/') continue;
            char *alt = strdup(entry->alternates[i]);
            if (!alt) continue;
            bgl_strip_dollar_indexes(&alt);
            err = udx_db_builder_add_key_entry(builder, alt, address, (uint32_t)def_len);
            free(alt);
            if (err != UDX_OK) {
                UD_LOG_ERROR("Failed to add alternate '%s' to UDX: error %d", entry->alternates[i], err);
                error_count++;
            } else {
                entry_count++;
            }
        }

        free(formatted_def);

        processed++;
        if (callback && total_entries > 0) {
            int pct = processed * 100 / total_entries;
            if (pct > last_pct) {
                last_pct = pct;
                if (!callback(dict, UNIDICT_INDEX_STAGE_ARTICLES, pct, user_data)) {
                    bgl_entry_iterator_free(iter);
                    udx_db_builder_finalize(builder);
                    udx_writer_close(writer);
                    ret = UNIDICT_ERR_CANCELLED;
                    goto fail;
                }
            }
        }
    }

    bgl_entry_iterator_free(iter);

    UD_LOG_INFO("Processed %d entries, %d errors", entry_count, error_count);

    UD_LOG_INFO("Finalizing entry database...");
    udx_status err = udx_db_builder_finalize(builder);
    if (err != UDX_OK) {
        UD_LOG_ERROR("Failed to finalize entry database: error %d", err);
        udx_writer_close(writer);
        ret = UNIDICT_ERR_IO;
        goto fail;
    }

    // Build resource database
    int resource_count = bgl_get_resource_count(reader);
    if (resource_count > 0) {
        UD_LOG_INFO("Building resource database (%d resources)...", resource_count);

        udx_db_builder *res_builder = udx_db_builder_create(writer, "resource");
        if (!res_builder) {
            UD_LOG_ERROR("Failed to create resource database builder");
            udx_writer_close(writer);
            goto fail;
        }

        bgl_resource_iterator *res_iter = bgl_resource_iterator_create(reader);
        if (!res_iter) {
            UD_LOG_ERROR("Failed to create BGL resource iterator");
            udx_db_builder_finalize(res_builder);
            udx_writer_close(writer);
            goto fail;
        }

        const bgl_resource *res;
        int res_added = 0;
        int res_last_pct = 0;
        while (bgl_resource_iterator_next(res_iter, &res) == BGL_OK) {
            if (!res->name || !res->data || res->data_size == 0) continue;

            udx_value_address address = udx_db_builder_add_value(res_builder, res->data, (uint32_t)res->data_size);
            if (address == UDX_INVALID_ADDRESS) {
                UD_LOG_ERROR("Failed to add resource '%s'", res->name);
                continue;
            }

            err = udx_db_builder_add_key_entry(res_builder, res->name, address, (uint32_t)res->data_size);
            if (err != UDX_OK) {
                UD_LOG_ERROR("Failed to index resource '%s'", res->name);
                continue;
            }
            res_added++;
            if (callback && resource_count > 0) {
                int pct = res_added * 100 / resource_count;
                if (pct > res_last_pct) {
                    res_last_pct = pct;
                    if (!callback(dict, UNIDICT_INDEX_STAGE_RESOURCES, pct, user_data)) {
                        bgl_resource_iterator_free(res_iter);
                        udx_db_builder_finalize(res_builder);
                        udx_writer_close(writer);
                        ret = UNIDICT_ERR_CANCELLED;
                        goto fail;
                    }
                }
            }
        }

        bgl_resource_iterator_free(res_iter);

        UD_LOG_INFO("Finalizing resource database (%d resources)...", res_added);
        err = udx_db_builder_finalize(res_builder);
        if (err != UDX_OK) {
            UD_LOG_ERROR("Failed to finalize resource database: error %d", err);
            udx_writer_close(writer);
            ret = UNIDICT_ERR_IO;
            goto fail;
        }
    }

    UD_LOG_INFO("Closing UDX file...");
    err = udx_writer_close(writer);
    if (err != UDX_OK) {
        UD_LOG_ERROR("Failed to close UDX writer: error %d", err);
        ret = UNIDICT_ERR_IO;
        goto fail;
    }

    UD_LOG_INFO("UDX index built successfully: %d entries", entry_count);

    free(udx_path);
    dict->has_external_index = true;
    return UNIDICT_OK;

fail:
    remove(udx_path);
    free(udx_path);
    return ret;
}
