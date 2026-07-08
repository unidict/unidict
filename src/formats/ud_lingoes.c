//
//  ud_lingoes.c
//  unidict
//
//  Created by kejinlu on 2026-01-31
//
#include "ud_lingoes.h"
#include "unidict_internal.h"
#include "unidict_log.h"
#include "ud_udx.h"
#include <sys/stat.h>
#include "ud_mime.h"
#include "udx_writer.h"
#include "ldx_reader.h"
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
static void ud_lingoes_release(uobject *obj);
static unidict_status lingoes_info_get(unidict *dict, unidict_info **out_info);
static unidict_status lingoes_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status lingoes_index_activate(unidict *dict, unidict_index_type index_type);
static unidict_status lingoes_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                  void *user_data);
static unidict_status lingoes_index_external_delete(unidict *dict);
static unidict_status lingoes_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status lingoes_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status lingoes_suggest(unidict *dict, const char *prefix, size_t limit,
                                      unidict_entry_array **out_entries);
static unidict_status lingoes_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_entry_iter *lingoes_entry_iter_create(unidict *dict);
static unidict_status lingoes_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void lingoes_entry_iter_free(unidict_entry_iter *iter);
static unidict_status lingoes_resource_get(unidict *dict, const char *key, unidict_resource **out_res);
static unidict_resource_iter *lingoes_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode);
static unidict_status lingoes_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res);
static void lingoes_resource_iter_free(unidict_resource_iter *iter);

// ============================================================
// Type Definition
// ============================================================
typedef struct ud_lingoes ud_lingoes;

struct ud_lingoes {
    unidict base;
    char *ldx_path;
    char *pref_lang;

    ldx_reader *reader;
    unidict *udx_dict;
};

static const uobject_type ud_lingoes_type = {
    .name = "ud_lingoes",
    .size = sizeof(ud_lingoes),
    .release = ud_lingoes_release,
};

// ============================================================
// Release
// ============================================================

static void ud_lingoes_release(uobject *obj) {
    if (!obj) return;

    ud_lingoes *lingoes = uobject_cast(obj, ud_lingoes, base.obj);

    if (lingoes->udx_dict) {
        unidict_close(lingoes->udx_dict);
        lingoes->udx_dict = NULL;
    }

    if (lingoes->reader) {
        ldx_reader_close(lingoes->reader);
        lingoes->reader = NULL;
    }

    free(lingoes->ldx_path);
    free(lingoes->pref_lang);
    free(lingoes);
}

// ============================================================
// Virtual Function Table
// ============================================================

static const unidict_ops lingoes_ops = {
    .prepare = NULL,
    .info_get = lingoes_info_get,
    .file_infos_get = lingoes_file_infos_get,
    .index_activate = lingoes_index_activate,
    .index_external_make = lingoes_index_external_make,
    .index_external_delete = lingoes_index_external_delete,
    .lookup = lingoes_lookup,
    .entry_lookup = lingoes_entry_lookup,
    .suggest = lingoes_suggest,
    .fetch = lingoes_fetch,
    .entry_iter_create = lingoes_entry_iter_create,
    .entry_iter_next = lingoes_entry_iter_next,
    .entry_iter_free = lingoes_entry_iter_free,
    .resource_get = lingoes_resource_get,
    .resource_iter_create = lingoes_resource_iter_create,
    .resource_iter_next = lingoes_resource_iter_next,
    .resource_iter_free = lingoes_resource_iter_free,
};

// ============================================================
// Helpers
// ============================================================

// Fill missing (NULL) fields by scanning all items for non-empty values (strdup).
static void lingoes_info_fallback_from_items(unidict_info *meta, const ldx_info_item *items, int count) {
    if (!meta || !items || count <= 0) return;

    for (int i = 0; i < count; i++) {
        if ((!meta->subtitle || !meta->subtitle[0]) && items[i].title && items[i].title[0]) {
            free(meta->subtitle);
            meta->subtitle = strdup(items[i].title);
        }
        if ((!meta->description || !meta->description[0]) && items[i].description && items[i].description[0]) {
            free(meta->description);
            meta->description = strdup(items[i].description);
        }
        if ((!meta->author || !meta->author[0]) && items[i].author && items[i].author[0]) {
            free(meta->author);
            meta->author = strdup(items[i].author);
        }
        if ((!meta->email || !meta->email[0]) && items[i].email && items[i].email[0]) {
            free(meta->email);
            meta->email = strdup(items[i].email);
        }

        if (meta->subtitle && meta->subtitle[0]
         && meta->description && meta->description[0]
         && meta->author && meta->author[0]
         && meta->email && meta->email[0])
            break;
    }
}

// ============================================================
// Constructor
// ============================================================

unidict *ud_lingoes_open(const char *file_path, const unidict_open_options *options) {
    if (!file_path) return NULL;

    const char *ext = strrchr(file_path, '.');
    if (!ext || (strcasecmp(ext, ".ld2") != 0 && strcasecmp(ext, ".ldx") != 0)) return NULL;

    ud_lingoes *lingoes = (ud_lingoes *)calloc(1, sizeof(ud_lingoes));
    if (!lingoes) return NULL;

    uobject_init(&lingoes->base.obj, &ud_lingoes_type, NULL);
    lingoes->base.ops = &lingoes_ops;
    lingoes->base.format = UNIDICT_FORMAT_LINGOES;
    lingoes->ldx_path = strdup(file_path);
    if (!lingoes->ldx_path) {
        ud_lingoes_release((uobject *)lingoes);
        return NULL;
    }

    lingoes->base.has_builtin_index = false;
    lingoes->base.has_external_index = unidict_detect_external_index(file_path);
    lingoes->pref_lang = (options && options->lingoes_pref_lang) ? strdup(options->lingoes_pref_lang) : NULL;

    unidict_index_type preset =
        (options && options->index_type != UNIDICT_INDEX_NONE) ? options->index_type : UNIDICT_INDEX_NONE;

    if (lingoes_index_activate((unidict *)lingoes, preset) != UNIDICT_OK) {
        ud_lingoes_release((uobject *)lingoes);
        return NULL;
    }

    return (unidict *)lingoes;
}

// ============================================================
// Info
// ============================================================

static unidict_status lingoes_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict || !out_info) return UNIDICT_ERR_INVALID_PARAM;
    *out_info = NULL;

    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);

    // UDX mode: delegate to UDX info_get (reads from stored metadata)
    if (lingoes->udx_dict && lingoes->udx_dict->ops->info_get) {
        unidict_status st = lingoes->udx_dict->ops->info_get(lingoes->udx_dict, out_info);
        if (st == UNIDICT_OK && *out_info) {
            (*out_info)->format = UNIDICT_FORMAT_LINGOES;
        }
        return st;
    }

    // Native mode: read from ldx reader
    unidict_info *info = calloc(1, sizeof(unidict_info));
    if (!info) return UNIDICT_ERR_NOMEM;

    info->format = UNIDICT_FORMAT_LINGOES;

    if (lingoes->reader) {
        const ldx_info *native = ldx_reader_get_info(lingoes->reader);
        info->word_count = native ? (uint64_t)native->gls_count : 0;
        info->source_lang = (native && native->from_lang) ? strdup(native->from_lang) : NULL;

        // Format version
        if (native && (native->version_major > 0 || native->version_minor > 0)) {
            char ver[32];
            snprintf(ver, sizeof(ver), "%d.%d", native->version_major, native->version_minor);
            info->format_version = strdup(ver);
        }

        // Icon
        if (native && native->icon_data && native->icon_size > 0) {
            info->icon_data = malloc(native->icon_size);
            if (info->icon_data) {
                memcpy(info->icon_data, native->icon_data, native->icon_size);
                info->icon_size = native->icon_size;
                info->icon_mime_type = strdup(ud_detect_image_mime(native->icon_data, native->icon_size));
            }
        }

        // Title from dict/@name (always present), fallback to item title
        if (native && native->name && native->name[0]) {
            info->title = strdup(native->name);
        }

        if (native && native->items && native->item_count > 0) {
            // Find matching item by pref_lang, fallback to items[0]
            const ldx_info_item *item = &native->items[0];
            if (lingoes->pref_lang) {
                for (int i = 0; i < native->item_count; i++) {
                    if (native->items[i].lang && strcmp(native->items[i].lang, lingoes->pref_lang) == 0) {
                        item = &native->items[i];
                        break;
                    }
                }
            }
            // Fill from matched item, then fallback from other items
            if (!info->title) {
                info->title = (item->title && item->title[0]) ? strdup(item->title) : strdup("Lingoes Dictionary");
            }
            info->subtitle = (item->title && item->title[0]) ? strdup(item->title) : NULL;
            info->description = (item->description && item->description[0]) ? strdup(item->description) : NULL;
            info->author = (item->author && item->author[0]) ? strdup(item->author) : NULL;
            info->email = (item->email && item->email[0]) ? strdup(item->email) : NULL;
            lingoes_info_fallback_from_items(info, native->items, native->item_count);
            if (item->edition > 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", item->edition);
                info->edition = strdup(buf);
            }
        } else if (!info->title) {
            info->title = strdup("Lingoes Dictionary");
        }
    } else {
        info->title = strdup("Lingoes Dictionary");
    }

    *out_info = info;
    return UNIDICT_OK;
}

// ============================================================
// File List
// ============================================================

static unidict_status lingoes_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict || !out_infos) return UNIDICT_ERR_INVALID_PARAM;
    *out_infos = NULL;

    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);

    // Derive .udx path and check if it exists on disk
    char *udx_path = NULL;
    if (lingoes->ldx_path) {
        const char *ldx_path = lingoes->ldx_path;
        const char *ext = strrchr(ldx_path, '.');
        size_t base_len = ext ? (size_t)(ext - ldx_path) : strlen(ldx_path);
        udx_path = malloc(base_len + 5);
        if (udx_path) {
            snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, ldx_path);
            struct stat udx_st;
            if (stat(udx_path, &udx_st) != 0) {
                free(udx_path);
                udx_path = NULL;
            }
        }
    }

    const char *paths[2];
    int count = 0;

    if (lingoes->ldx_path) paths[count++] = lingoes->ldx_path;
    if (udx_path) paths[count++] = udx_path;

    *out_infos = unidict_file_infos_from_paths(paths, count);
    free(udx_path);

    return UNIDICT_OK;
}

// ============================================================
// Index Activate
// ============================================================

static unidict_status lingoes_index_activate(unidict *dict, unidict_index_type index_type) {
    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);

    if (lingoes->udx_dict) {
        unidict_close(lingoes->udx_dict);
        lingoes->udx_dict = NULL;
    }
    if (lingoes->reader) {
        ldx_reader_close(lingoes->reader);
        lingoes->reader = NULL;
    }
    dict->active_index = UNIDICT_INDEX_NONE;

    const char *ldx_path = lingoes->ldx_path;
    const char *ext = ldx_path ? strrchr(ldx_path, '.') : NULL;
    if (!ext) return UNIDICT_ERR_INTERNAL;

    size_t base_len = ext - ldx_path;

    // EXTERNAL or NONE: try UDX first
    if (index_type == UNIDICT_INDEX_EXTERNAL || index_type == UNIDICT_INDEX_NONE) {
        char *udx_path = (char *)malloc(base_len + 5);
        if (!udx_path) return UNIDICT_ERR_NOMEM;
        snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, ldx_path);

        unidict *udx_dict = ud_udx_open(udx_path, NULL);
        free(udx_path);

        if (udx_dict) {
            lingoes->udx_dict = udx_dict;
            dict->active_index = UNIDICT_INDEX_EXTERNAL;
            return UNIDICT_OK;
        }
    }

    // BUILTIN or NONE fallback: open LDX reader
    ldx_reader *reader = NULL;
    ldx_status lst = ldx_reader_open(ldx_path, &reader);
    if (lst != LDX_OK) return UNIDICT_ERR_IO;

    lingoes->reader = reader;
    // lingoes has no builtin index, active_index stays NONE
    return UNIDICT_OK;
}

// ============================================================
// Index External Make
// ============================================================

static unidict_status lingoes_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                  void *user_data) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;

    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);

    // Close existing UDX before overwriting
    if (lingoes->udx_dict) {
        unidict_close(lingoes->udx_dict);
        lingoes->udx_dict = NULL;
        dict->active_index = UNIDICT_INDEX_NONE;
    }

    ldx_reader *reader = lingoes->reader;

    // Lazy load LDX reader if not already loaded
    if (!reader) {
        if (!lingoes->ldx_path) {
            UD_LOG_ERROR("Cannot determine LD2/LDX file path - ldx_path is NULL");
            return UNIDICT_ERR_NO_INDEX;
        }

        UD_LOG_INFO("Lazy loading LDX reader: %s", lingoes->ldx_path);
        ldx_status lst = ldx_reader_open(lingoes->ldx_path, &reader);

        if (lst != LDX_OK) {
            UD_LOG_ERROR("Failed to open LD2/LDX file for index creation: %d", lst);
            return (lst == LDX_ERR_IO) ? UNIDICT_ERR_IO : UNIDICT_ERR_INTERNAL;
        }

        lingoes->reader = reader;
    }

    if (!reader || !lingoes->ldx_path) return UNIDICT_ERR_NO_INDEX;

    const char *ld2_path = lingoes->ldx_path;

    const char *ext = strrchr(ld2_path, '.');
    if (!ext || (strcasecmp(ext, ".ld2") != 0 && strcasecmp(ext, ".ldx") != 0)) {
        UD_LOG_ERROR("LD2 file path does not have .ld2 or .ldx extension: %s", ld2_path);
        return UNIDICT_ERR_NO_INDEX;
    }

    size_t base_len = ext - ld2_path;
    char *udx_path = (char *)malloc(base_len + 5);
    if (!udx_path) {
        UD_LOG_ERROR("Failed to allocate memory for UDX path");
        return UNIDICT_ERR_NOMEM;
    }

    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, ld2_path);
    UD_LOG_INFO("Building UDX index: %s -> %s", ld2_path, udx_path);

    udx_writer *writer = udx_writer_open(udx_path);
    if (!writer) {
        UD_LOG_ERROR("Failed to open UDX writer for: %s", udx_path);
        free(udx_path);
        return UNIDICT_ERR_NO_INDEX;
    }

    unidict_status ret = UNIDICT_ERR_INTERNAL;

    // Build info metadata via info_get (native path, no UDX yet)
    unidict_info *meta_info = NULL;
    unidict_status info_st = lingoes_info_get(dict, &meta_info);
    char *meta_xml = NULL;
    if (info_st == UNIDICT_OK && meta_info) {
        meta_xml = unidict_info_to_xml(meta_info);
        unidict_info_free(meta_info);
    }

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

    ldx_gls_iter *iter = ldx_reader_gls_iter_create(reader, LDX_GLS_ENTRY_MODE_DEF_STRING);
    if (!iter) {
        UD_LOG_ERROR("Failed to create GLS iterator");
        udx_db_builder_finalize(builder);
        udx_writer_close(writer);
        goto fail;
    }

    int entry_count = 0;
    int error_count = 0;
    int total_entries = (int)ldx_reader_get_info(reader)->gls_count;
    int resource_count_pre = ldx_reader_res_count(reader);
    int grand_total = total_entries + resource_count_pre;
    int last_pct = 0;

    UD_LOG_INFO("Reading entries from LD2 file...");

    const ldx_gls_entry *entry = NULL;
    while (ldx_reader_gls_iter_next(iter, &entry) == LDX_OK) {
        if (!entry->word || !entry->definition) continue;

        size_t def_len = strlen(entry->definition);

        udx_value_address address =
            udx_db_builder_add_value(builder, (const uint8_t *)entry->definition, (uint32_t)def_len);
        if (address == UDX_INVALID_ADDRESS) {
            UD_LOG_ERROR("Failed to add chunk for word '%s'", entry->word);
            error_count++;
            continue;
        }

        udx_status err = udx_db_builder_add_key_entry(builder, entry->word, address, (uint32_t)def_len);
        if (err != UDX_OK) {
            UD_LOG_ERROR("Failed to add word entry '%s' to UDX: error %d", entry->word, err);
            error_count++;
            continue;
        }

        entry_count++;

        if (callback && grand_total > 0) {
            int pct = entry_count * 100 / grand_total;
            if (pct > last_pct) {
                last_pct = pct;
                if (!callback(dict, pct, user_data)) {
                    ldx_reader_gls_iter_free(iter);
                    udx_db_builder_finalize(builder);
                    udx_writer_close(writer);
                    ret = UNIDICT_ERR_CANCELLED;
                    goto fail;
                }
            }
        }
    }

    ldx_reader_gls_iter_free(iter);

    UD_LOG_INFO("Processed %d entries, %d errors", entry_count, error_count);

    udx_status err = udx_db_builder_finalize(builder);
    if (err != UDX_OK) {
        UD_LOG_ERROR("Failed to finalize UDX database: error %d", err);
        udx_writer_close(writer);
        goto fail;
    }

    // Build resource database
    int resource_count = resource_count_pre;
    if (resource_count > 0) {
        UD_LOG_INFO("Building resource database (%d resources)...", resource_count);

        udx_db_builder *res_builder = udx_db_builder_create(writer, "resource");
        if (!res_builder) {
            UD_LOG_ERROR("Failed to create resource database builder");
            udx_writer_close(writer);
            ret = UNIDICT_ERR_INTERNAL;
            goto fail;
        }

        ldx_res_iter *res_iter = ldx_reader_res_iter_create(reader, LDX_RES_ENTRY_MODE_DATA_BLOB);
        if (!res_iter) {
            UD_LOG_ERROR("Failed to create LDX resource iterator");
            udx_db_builder_finalize(res_builder);
            udx_writer_close(writer);
            ret = UNIDICT_ERR_INTERNAL;
            goto fail;
        }

        const ldx_res_entry *res_entry = NULL;
        int res_added = 0;
        while (ldx_reader_res_iter_next(res_iter, &res_entry) == LDX_OK) {
            if (!res_entry->name || !res_entry->data || res_entry->data_size == 0) continue;

            udx_value_address address =
                udx_db_builder_add_value(res_builder, res_entry->data, (uint32_t)res_entry->data_size);
            if (address == UDX_INVALID_ADDRESS) {
                UD_LOG_ERROR("Failed to add resource '%s'", res_entry->name);
                continue;
            }

            err = udx_db_builder_add_key_entry(res_builder, res_entry->name, address, (uint32_t)res_entry->data_size);
            if (err != UDX_OK) {
                UD_LOG_ERROR("Failed to index resource '%s'", res_entry->name);
                continue;
            }
            res_added++;

            if (callback && grand_total > 0) {
                int pct = (entry_count + res_added) * 100 / grand_total;
                if (pct > last_pct) {
                    last_pct = pct;
                    if (!callback(dict, pct, user_data)) {
                        ldx_reader_res_iter_free(res_iter);
                        udx_db_builder_finalize(res_builder);
                        udx_writer_close(writer);
                        ret = UNIDICT_ERR_CANCELLED;
                        goto fail;
                    }
                }
            }
        }

        ldx_reader_res_iter_free(res_iter);

        UD_LOG_INFO("Finalizing resource database (%d resources)...", res_added);
        err = udx_db_builder_finalize(res_builder);
        if (err != UDX_OK) {
            UD_LOG_ERROR("Failed to finalize resource database: error %d", err);
            udx_writer_close(writer);
            ret = UNIDICT_ERR_IO;
            goto fail;
        }
    }

    err = udx_writer_close(writer);
    if (err != UDX_OK) {
        UD_LOG_ERROR("Failed to close UDX writer: error %d", err);
        goto fail;
    }

    UD_LOG_INFO("UDX index built successfully: %d entries", entry_count);

    free(udx_path);
    dict->has_external_index = true;
    if (callback && last_pct < 100) {
        callback(dict, 100, user_data);
    }
    return UNIDICT_OK;

fail:
    remove(udx_path);
    free(udx_path);
    return ret;
}

// ============================================================
// Index External Delete
// ============================================================

static unidict_status lingoes_index_external_delete(unidict *dict) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);

    unidict_status st = lingoes_index_activate(dict, UNIDICT_INDEX_BUILTIN);
    if (st != UNIDICT_OK) return st;

    const char *ldx_path = lingoes->ldx_path;
    const char *ext = strrchr(ldx_path, '.');
    if (!ext) return UNIDICT_ERR_INTERNAL;
    size_t base_len = ext - ldx_path;
    char *udx_path = malloc(base_len + 5);
    if (!udx_path) return UNIDICT_ERR_NOMEM;
    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, ldx_path);

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

static unidict_status lingoes_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key || !out_articles) return UNIDICT_ERR_INVALID_PARAM;
    *out_articles = NULL;

    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);
    if (!lingoes->udx_dict) return UNIDICT_ERR_NO_INDEX;

    if (!lingoes->udx_dict->ops->lookup) return UNIDICT_ERR_NOT_SUPPORTED;
    return lingoes->udx_dict->ops->lookup(lingoes->udx_dict, key, out_articles);
}

// ============================================================
// Entry Lookup
// ============================================================

static unidict_status lingoes_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key || !out_entries) return UNIDICT_ERR_INVALID_PARAM;
    *out_entries = NULL;

    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);
    if (!lingoes->udx_dict) return UNIDICT_ERR_NO_INDEX;

    if (!lingoes->udx_dict->ops->entry_lookup) return UNIDICT_ERR_NOT_SUPPORTED;
    return lingoes->udx_dict->ops->entry_lookup(lingoes->udx_dict, key, out_entries);
}

// ============================================================
// Suggest
// ============================================================

static unidict_status lingoes_suggest(unidict *dict, const char *prefix, size_t limit,
                                      unidict_entry_array **out_entries) {
    if (!dict || !prefix || !out_entries) return UNIDICT_ERR_INVALID_PARAM;
    *out_entries = NULL;

    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);
    if (!lingoes->udx_dict) return UNIDICT_ERR_NO_INDEX;

    if (!lingoes->udx_dict->ops->suggest) return UNIDICT_ERR_NOT_SUPPORTED;
    return lingoes->udx_dict->ops->suggest(lingoes->udx_dict, prefix, limit, out_entries);
}

// ============================================================
// Fetch
// ============================================================

static unidict_status lingoes_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry || !out_articles) return UNIDICT_ERR_INVALID_PARAM;
    *out_articles = NULL;

    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);
    if (!lingoes->udx_dict) return UNIDICT_ERR_NO_INDEX;

    if (!lingoes->udx_dict->ops->fetch) return UNIDICT_ERR_NOT_SUPPORTED;
    return lingoes->udx_dict->ops->fetch(lingoes->udx_dict, entry, out_articles);
}

// ============================================================
// Entry Iterator
// ============================================================

static unidict_entry_iter *lingoes_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;

    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);
    if (!lingoes->udx_dict) return NULL;

    if (!lingoes->udx_dict->ops->entry_iter_create) return NULL;
    return lingoes->udx_dict->ops->entry_iter_create(lingoes->udx_dict);
}

static unidict_status lingoes_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict || !out_entry) return UNIDICT_ERR_INVALID_PARAM;
    return iter->dict->ops->entry_iter_next(iter, out_entry);
}

static void lingoes_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;
    iter->dict->ops->entry_iter_free(iter);
}

// ============================================================
// Resource Get
// ============================================================

static unidict_status lingoes_resource_get(unidict *dict, const char *key, unidict_resource **out_res) {
    if (!dict || !key || !out_res) return UNIDICT_ERR_INVALID_PARAM;
    *out_res = NULL;

    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);
    if (!lingoes->udx_dict) return UNIDICT_ERR_NO_INDEX;

    if (!lingoes->udx_dict->ops->resource_get) return UNIDICT_ERR_NOT_SUPPORTED;
    return lingoes->udx_dict->ops->resource_get(lingoes->udx_dict, key, out_res);
}

// ============================================================
// Resource Iterator
// ============================================================

static unidict_resource_iter *lingoes_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode) {
    if (!dict) return NULL;
    ud_lingoes *lingoes = uobject_cast(&dict->obj, ud_lingoes, base.obj);
    if (!lingoes->udx_dict) return NULL;

    if (!lingoes->udx_dict->ops->resource_iter_create) return NULL;
    return lingoes->udx_dict->ops->resource_iter_create(lingoes->udx_dict, mode);
}

static unidict_status lingoes_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res) {
    if (!iter || !iter->dict || !out_res) return UNIDICT_ERR_INVALID_PARAM;
    return iter->dict->ops->resource_iter_next(iter, out_res);
}

static void lingoes_resource_iter_free(unidict_resource_iter *iter) {
    if (!iter) return;
    iter->dict->ops->resource_iter_free(iter);
}
