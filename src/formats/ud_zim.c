//
//  ud_zim.c
//  unidict
//
//  Created by kejinlu on 2026-01-22
//
#include "ud_zim.h"
#include "unidict_internal.h"
#include "ud_udx.h"
#include "ud_mime.h"
#include "udx_writer.h"
#include "czim_archive.h"
#include "czim_archive_ext.h"
#include "czim_file.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

// ============================================================
// ud_zim 结构
// ============================================================

typedef struct ud_zim {
    unidict base;
    char *zim_path;
    czim_archive *archive;
    unidict *udx_dict;
} ud_zim;

// ============================================================
// 索引结构（ZIM 实现）
// ============================================================

typedef struct {
    uobject obj;
    uint32_t index;
} ud_zim_index;

static void ud_zim_index_release(uobject *obj) {
    ud_zim_index *idx = uobject_cast(obj, ud_zim_index, obj);
    free(idx);
}

static const uobject_type ud_zim_index_type = {
    .name = "ud_zim_index",
    .size = sizeof(ud_zim_index),
    .release = ud_zim_index_release,
};

// ============================================================
// ud_zim 类型定义
// ============================================================

static void ud_zim_release(uobject *obj) {
    if (!obj) return;
    ud_zim *zim = uobject_cast(obj, ud_zim, base.obj);
    if (zim->udx_dict) {
        unidict_close(zim->udx_dict);
        zim->udx_dict = NULL;
    }
    if (zim->archive) {
        czim_archive_close(zim->archive);
    }
    free(zim->zim_path);
    free(zim);
}

static const uobject_type ud_zim_type = {
    .name = "ud_zim",
    .size = sizeof(ud_zim),
    .release = ud_zim_release,
};

// ============================================================
// 辅助函数
// ============================================================

static char *html_to_text(const uint8_t *data, size_t size) {
    char *text = malloc(size + 1);
    if (!text) return NULL;
    memcpy(text, data, size);
    text[size] = '\0';
    return text;
}

static const char *mime_from_blob(const czim_blob *blob) {
    const uint8_t *data = czim_blob_data(blob);
    size_t size = czim_blob_size(blob);
    return ud_detect_image_mime(data, size);
}

static char *zim_get_metadata_string(czim_archive *archive, const char *name) {
    czim_entry *entry = czim_archive_find_metadata(archive, name, NULL);
    if (!entry) return NULL;
    czim_blob blob = czim_archive_get_blob(archive, entry);
    czim_entry_free(entry);
    if (!czim_blob_data(&blob) || czim_blob_size(&blob) == 0) {
        czim_blob_free(&blob);
        return NULL;
    }
    char *str = malloc(czim_blob_size(&blob) + 1);
    if (str) {
        memcpy(str, czim_blob_data(&blob), czim_blob_size(&blob));
        str[czim_blob_size(&blob)] = '\0';
    }
    czim_blob_free(&blob);
    return str;
}

static bool zim_is_article_entry(ud_zim *zim, const czim_entry *entry) {
    if (!czim_entry_is_article(entry)) return false;
    const char *mime = czim_archive_mime_type(zim->archive, entry->mime_type);
    return mime && strcmp(mime, "text/html") == 0;
}

static bool zim_is_resource_entry(ud_zim *zim, const czim_entry *entry) {
    if (!czim_entry_is_article(entry)) return false;
    char ns = czim_entry_get_namespace(entry);
    if (ns == 'M' || ns == 'X' || ns == 'W') return false;
    const char *mime = czim_archive_mime_type(zim->archive, entry->mime_type);
    return mime && strcmp(mime, "text/html") != 0;
}

static char *zim_get_udx_path(const char *zim_path) {
    const char *ext = strrchr(zim_path, '.');
    if (!ext) return NULL;
    size_t base_len = ext - zim_path;
    char *udx_path = malloc(base_len + 5);
    if (!udx_path) return NULL;
    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, zim_path);
    return udx_path;
}

static uint32_t zim_ref_from_value_item(const udx_value_entry_item *item) {
    if (!item || !item->data || item->size < 4) return UINT32_MAX;
    uint32_t ref = 0;
    memcpy(&ref, item->data, 4);
    return ref;
}

// ============================================================
// 前向声明
// ============================================================

static unidict_status ud_zim_info_get(unidict *dict, unidict_info **out_info);
static unidict_status ud_zim_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status ud_zim_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status ud_zim_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status ud_zim_suggest(unidict *dict, const char *prefix, size_t limit,
                                     unidict_entry_array **out_entries);
static unidict_status ud_zim_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_status ud_zim_index_activate(unidict *dict, unidict_index_type index_type);
static unidict_status ud_zim_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                 void *user_data);
static unidict_status ud_zim_index_external_delete(unidict *dict);
static unidict_entry_iter *ud_zim_entry_iter_create(unidict *dict);
static unidict_status ud_zim_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void ud_zim_entry_iter_free(unidict_entry_iter *iter);
static unidict_status ud_zim_resource_get(unidict *dict, const char *key, unidict_resource **out_res);
static unidict_resource_iter *ud_zim_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode);
static unidict_status ud_zim_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res);
static void ud_zim_resource_iter_free(unidict_resource_iter *iter);

// ============================================================
// ud_dict 虚函数实现
// ============================================================

static unidict_status ud_zim_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    if (zim->udx_dict) {
        udx_db_value_entry *ve = ud_udx_raw_lookup(zim->udx_dict, key);
        if (!ve || ve->items.count == 0) {
            if (ve) udx_db_value_entry_free(ve);
            *out_entries = NULL;
            return UNIDICT_OK;
        }

        unidict_entry_array *res = calloc(1, sizeof(unidict_entry_array));
        if (!res) {
            udx_db_value_entry_free(ve);
            *out_entries = NULL;
            return UNIDICT_ERR_NOMEM;
        }
        res->count = ve->items.count;
        res->items = calloc(res->count, sizeof(unidict_entry *));
        if (!res->items) {
            free(res);
            udx_db_value_entry_free(ve);
            *out_entries = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        for (size_t i = 0; i < ve->items.count; i++) {
            uint32_t idx = zim_ref_from_value_item(&ve->items.elements[i]);

            ud_zim_index *zim_idx = calloc(1, sizeof(ud_zim_index));
            if (!zim_idx) continue;
            zim_idx->index = idx;
            uobject_init(&zim_idx->obj, &ud_zim_index_type, NULL);

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (!entry) {
                uobject_release(&zim_idx->obj);
                continue;
            }
            entry->key = ve->items.elements[i].original_key ? strdup(ve->items.elements[i].original_key) : strdup(key);
            entry->internal_entry = &zim_idx->obj;
            res->items[i] = entry;
        }
        udx_db_value_entry_free(ve);
        *out_entries = res;
        return UNIDICT_OK;
    }

    uint32_t index;
    czim_entry *zim_entry = czim_archive_find_content_entry_by_title(zim->archive, key, &index);
    if (!zim_entry) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }
    czim_entry_free(zim_entry);

    ud_zim_index *zim_idx = calloc(1, sizeof(ud_zim_index));
    if (!zim_idx) {
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }
    zim_idx->index = index;
    uobject_init(&zim_idx->obj, &ud_zim_index_type, NULL);

    unidict_entry_array *res = malloc(sizeof(unidict_entry_array));
    if (!res) {
        uobject_release(&zim_idx->obj);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->items = calloc(1, sizeof(unidict_entry *));
    if (!res->items) {
        free(res);
        uobject_release(&zim_idx->obj);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    unidict_entry *entry = calloc(1, sizeof(unidict_entry));
    if (!entry) {
        free(res->items);
        free(res);
        uobject_release(&zim_idx->obj);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entry->key = strdup(key);
    entry->internal_entry = &zim_idx->obj;
    res->items[0] = entry;
    res->count = 1;
    *out_entries = res;
    return UNIDICT_OK;
}

static unidict_status ud_zim_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    if (zim->udx_dict) {
        udx_db_value_entry *ve = ud_udx_raw_lookup(zim->udx_dict, key);
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
            uint32_t idx = zim_ref_from_value_item(&ve->items.elements[i]);
            if (idx == UINT32_MAX) continue;

            czim_entry *zim_entry = czim_archive_get_entry_by_index(zim->archive, idx);
            if (!zim_entry) continue;

            const char *title = czim_entry_get_title(zim_entry);

            if (czim_entry_is_redirect(zim_entry)) {
                czim_entry *resolved = czim_archive_resolve_redirect(zim->archive, zim_entry, 10);
                if (resolved) {
                    czim_blob blob = czim_archive_get_blob(zim->archive, resolved);
                    if (czim_blob_data(&blob)) {
                        res->items[i].title = title ? strdup(title) : NULL;
                        res->items[i].body = html_to_text(czim_blob_data(&blob), czim_blob_size(&blob));
                    }
                    czim_blob_free(&blob);
                    czim_entry_free(resolved);
                }
            } else {
                czim_blob blob = czim_archive_get_blob(zim->archive, zim_entry);
                if (czim_blob_data(&blob)) {
                    res->items[i].title = title ? strdup(title) : NULL;
                    res->items[i].body = html_to_text(czim_blob_data(&blob), czim_blob_size(&blob));
                }
                czim_blob_free(&blob);
            }
            czim_entry_free(zim_entry);
        }
        udx_db_value_entry_free(ve);
        *out_articles = res;
        return UNIDICT_OK;
    }

    uint32_t index;
    czim_entry *entry = czim_archive_find_content_entry_by_title(zim->archive, key, &index);
    if (!entry) {
        entry = czim_archive_find_content_entry_by_path(zim->archive, key, &index);
        if (!entry) {
            *out_articles = NULL;
            return UNIDICT_OK;
        }
    }

    // 跟随重定向
    if (czim_entry_is_redirect(entry)) {
        czim_entry *resolved = czim_archive_resolve_redirect(zim->archive, entry, 10);
        czim_entry_free(entry);
        if (!resolved) {
            *out_articles = NULL;
            return UNIDICT_OK;
        }
        entry = resolved;
    }

    // 获取 blob
    char *title = czim_entry_get_title(entry) ? strdup(czim_entry_get_title(entry)) : NULL;
    czim_blob blob = czim_archive_get_blob(zim->archive, entry);
    czim_entry_free(entry);

    if (!czim_blob_data(&blob)) {
        free(title);
        *out_articles = NULL;
        return UNIDICT_OK;
    }

    char *text = html_to_text(czim_blob_data(&blob), czim_blob_size(&blob));
    czim_blob_free(&blob);

    if (!text) {
        free(title);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    unidict_article_array *res = malloc(sizeof(unidict_article_array));
    if (!res) {
        free(text);
        free(title);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->items = malloc(sizeof(unidict_article));
    if (!res->items) {
        free(text);
        free(title);
        free(res);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->items[0].title = title;
    res->items[0].body = text;
    res->count = 1;

    *out_articles = res;
    return UNIDICT_OK;
}

static unidict_status ud_zim_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict) {
        *out_info = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    if (zim->udx_dict && zim->udx_dict->ops->info_get) {
        unidict_status st = zim->udx_dict->ops->info_get(zim->udx_dict, out_info);
        if (st == UNIDICT_OK && *out_info) {
            (*out_info)->format = dict->format;
        }
        return st;
    }

    unidict_info *info = calloc(1, sizeof(unidict_info));
    if (!info) {
        *out_info = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    info->format = UNIDICT_FORMAT_ZIM;

    char *title = zim_get_metadata_string(zim->archive, "Title");
    info->title = title ? title : strdup("ZIM Archive");
    info->description = zim_get_metadata_string(zim->archive, "Description");
    info->author = zim_get_metadata_string(zim->archive, "Creator");
    info->creation_date = zim_get_metadata_string(zim->archive, "Date");
    info->source_lang = zim_get_metadata_string(zim->archive, "Language");
    info->target_lang = NULL;
    info->word_count = czim_archive_article_count(zim->archive);

    // Icon
    uint32_t icon_index;
    czim_entry *icon_entry = czim_archive_get_illustration(zim->archive, 48, &icon_index);
    if (!icon_entry) icon_entry = czim_archive_find_favicon(zim->archive, &icon_index);
    if (icon_entry) {
        czim_blob icon_blob = czim_archive_get_blob(zim->archive, icon_entry);
        czim_entry_free(icon_entry);
        if (czim_blob_data(&icon_blob) && czim_blob_size(&icon_blob) > 0) {
            info->icon_data = malloc(czim_blob_size(&icon_blob));
            if (info->icon_data) {
                memcpy(info->icon_data, czim_blob_data(&icon_blob), czim_blob_size(&icon_blob));
                info->icon_size = czim_blob_size(&icon_blob);
                info->icon_mime_type = strdup(ud_detect_image_mime(czim_blob_data(&icon_blob), czim_blob_size(&icon_blob)));
            }
        }
        czim_blob_free(&icon_blob);
    }

    *out_info = info;
    return UNIDICT_OK;
}

static unidict_status ud_zim_suggest(unidict *dict, const char *prefix, size_t limit,
                                     unidict_entry_array **out_entries) {
    if (!dict || !prefix) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    if (zim->udx_dict) {
        if (!zim->udx_dict->ops->suggest) {
            *out_entries = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_entry_array *udx_entries = NULL;
        unidict_status st = zim->udx_dict->ops->suggest(zim->udx_dict, prefix, limit, &udx_entries);
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

            udx_db_value_entry *ve = ud_udx_raw_fetch(zim->udx_dict, udx_entry);
            uint32_t idx = 0;
            if (ve && ve->items.count > 0) {
                idx = zim_ref_from_value_item(&ve->items.elements[0]);
            }
            if (ve) udx_db_value_entry_free(ve);

            ud_zim_index *zim_idx = calloc(1, sizeof(ud_zim_index));
            if (!zim_idx) continue;
            zim_idx->index = idx;
            uobject_init(&zim_idx->obj, &ud_zim_index_type, NULL);

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (!entry) {
                uobject_release(&zim_idx->obj);
                continue;
            }
            entry->key = strdup(udx_entry->key);
            entry->internal_entry = &zim_idx->obj;
            res->items[i] = entry;
        }

        unidict_entry_array_free(udx_entries);
        *out_entries = res;
        return UNIDICT_OK;
    }

    uint32_t start_index, end_index;
    int rc = czim_archive_find_content_entry_by_title_prefix(zim->archive, prefix, &start_index, &end_index);
    if (rc != CZIM_OK) {
        rc = czim_archive_find_content_entry_by_path_prefix(zim->archive, prefix, &start_index, &end_index);
        if (rc != CZIM_OK) {
            *out_entries = NULL;
            return UNIDICT_OK;
        }
    }

    uint32_t count = end_index - start_index;
    if (limit > 0 && count > limit) {
        count = (uint32_t)limit;
    }
    if (count == 0) {
        *out_entries = NULL;
        return UNIDICT_OK;
    }

    unidict_entry_array *entries = malloc(sizeof(unidict_entry_array));
    if (!entries) {
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    entries->items = calloc(count, sizeof(unidict_entry *));
    if (!entries->items) {
        free(entries);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }
    entries->count = count;

    for (uint32_t i = 0; i < count; i++) {
        czim_entry *entry = czim_archive_get_entry_by_index(zim->archive, start_index + i);
        if (!entry) continue;

        const char *title = czim_entry_get_title(entry);
        if (!title) {
            czim_entry_free(entry);
            continue;
        }

        ud_zim_index *zim_idx = calloc(1, sizeof(ud_zim_index));
        if (zim_idx) {
            zim_idx->index = start_index + i;
            uobject_init(&zim_idx->obj, &ud_zim_index_type, NULL);

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (entry) {
                entry->key = strdup(title);
                entry->internal_entry = &zim_idx->obj;
                entries->items[i] = entry;
            } else {
                uobject_release(&zim_idx->obj);
            }
        }

        czim_entry_free(entry);
    }

    *out_entries = entries;
    return UNIDICT_OK;
}

static unidict_status ud_zim_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);
    ud_zim_index *zim_idx = uobject_cast(entry->internal_entry, ud_zim_index, obj);

    czim_entry *zim_entry = czim_archive_get_entry_by_index(zim->archive, zim_idx->index);
    if (!zim_entry) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }

    // 跟随重定向
    if (czim_entry_is_redirect(zim_entry)) {
        czim_entry *resolved = czim_archive_resolve_redirect(zim->archive, zim_entry, 10);
        czim_entry_free(zim_entry);
        if (!resolved) {
            *out_articles = NULL;
            return UNIDICT_OK;
        }
        zim_entry = resolved;
    }

    czim_blob blob = czim_archive_get_blob(zim->archive, zim_entry);
    czim_entry_free(zim_entry);

    if (!czim_blob_data(&blob)) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }

    char *text = html_to_text(czim_blob_data(&blob), czim_blob_size(&blob));
    czim_blob_free(&blob);

    if (!text) {
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    unidict_article_array *res = malloc(sizeof(unidict_article_array));
    if (!res) {
        free(text);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->items = calloc(1, sizeof(unidict_article));
    if (!res->items) {
        free(res);
        free(text);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->items[0].title = NULL;
    res->items[0].body = text;
    res->count = 1;
    *out_articles = res;
    return UNIDICT_OK;
}

// ============================================================
// Entry Iterator
// ============================================================

typedef struct {
    unidict_entry_iter base;
    uint32_t pos;
    uint32_t total;
    unidict_entry_iter *udx_iter;
} ud_zim_entry_iter;

static unidict_entry_iter *ud_zim_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    ud_zim_entry_iter *iter = calloc(1, sizeof(ud_zim_entry_iter));
    if (!iter) return NULL;

    iter->base.dict = dict;

    if (zim->udx_dict) {
        if (!zim->udx_dict->ops->entry_iter_create) {
            free(iter);
            return NULL;
        }
        iter->udx_iter = zim->udx_dict->ops->entry_iter_create(zim->udx_dict);
        if (!iter->udx_iter) {
            free(iter);
            return NULL;
        }
    } else {
        iter->total = czim_archive_entry_count(zim->archive);
    }

    return (unidict_entry_iter *)iter;
}

static unidict_status ud_zim_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict) {
        if (out_entry) *out_entry = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_zim_entry_iter *zim_iter = (ud_zim_entry_iter *)iter;
    ud_zim *zim = uobject_cast(&iter->dict->obj, ud_zim, base.obj);

    free(iter->current.key);
    iter->current.key = NULL;
    if (iter->current.internal_entry) {
        uobject_release(iter->current.internal_entry);
        iter->current.internal_entry = NULL;
    }

    if (zim_iter->udx_iter) {
        if (!zim->udx_dict->ops->entry_iter_next) {
            *out_entry = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_entry *udx_entry = NULL;
        unidict_status st = zim->udx_dict->ops->entry_iter_next(zim_iter->udx_iter, &udx_entry);
        if (st != UNIDICT_OK || !udx_entry) {
            *out_entry = NULL;
            return UNIDICT_DONE;
        }

        iter->current.key = strdup(udx_entry->key);

        udx_db_value_entry *ve = ud_udx_raw_fetch(zim->udx_dict, udx_entry);
        uint32_t idx = 0;
        if (ve && ve->items.count > 0) {
            idx = zim_ref_from_value_item(&ve->items.elements[0]);
        }
        if (ve) udx_db_value_entry_free(ve);

        ud_zim_index *zim_idx = calloc(1, sizeof(ud_zim_index));
        if (!zim_idx) {
            *out_entry = NULL;
            return UNIDICT_ERR_NOMEM;
        }
        zim_idx->index = idx;
        uobject_init(&zim_idx->obj, &ud_zim_index_type, NULL);
        iter->current.internal_entry = &zim_idx->obj;

        if (!iter->current.key) {
            uobject_release(&zim_idx->obj);
            *out_entry = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        *out_entry = &iter->current;
        return UNIDICT_OK;
    }

    while (zim_iter->pos < zim_iter->total) {
        uint32_t idx = zim_iter->pos++;
        czim_entry *entry = czim_archive_get_entry_by_index(zim->archive, idx);
        if (!entry) continue;

        bool is_article = zim_is_article_entry(zim, entry);
        if (!is_article) {
            czim_entry_free(entry);
            continue;
        }

        const char *title = czim_entry_get_title(entry);
        czim_entry_free(entry);

        ud_zim_index *zim_idx = calloc(1, sizeof(ud_zim_index));
        if (!zim_idx) {
            *out_entry = NULL;
            return UNIDICT_ERR_NOMEM;
        }
        zim_idx->index = idx;
        uobject_init(&zim_idx->obj, &ud_zim_index_type, NULL);

        iter->current.key = strdup(title ? title : "");
        iter->current.internal_entry = &zim_idx->obj;

        if (!iter->current.key) {
            uobject_release(&zim_idx->obj);
            *out_entry = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        *out_entry = &iter->current;
        return UNIDICT_OK;
    }

    *out_entry = NULL;
    return UNIDICT_DONE;
}

static void ud_zim_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;
    ud_zim_entry_iter *zim_iter = (ud_zim_entry_iter *)iter;
    free(iter->current.key);
    if (iter->current.internal_entry) {
        uobject_release(iter->current.internal_entry);
    }
    if (zim_iter->udx_iter) {
        ud_zim *zim = uobject_cast(&iter->dict->obj, ud_zim, base.obj);
        if (zim->udx_dict && zim->udx_dict->ops->entry_iter_free)
            zim->udx_dict->ops->entry_iter_free(zim_iter->udx_iter);
    }
    free(iter);
}

// ============================================================
// Resource Get
// ============================================================

static unidict_status ud_zim_resource_get(unidict *dict, const char *key, unidict_resource **out_res) {
    if (!dict || !key) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    uint32_t index;
    czim_entry *entry = czim_archive_find_content_entry_by_path(zim->archive, key, &index);
    if (!entry) {
        *out_res = NULL;
        return UNIDICT_OK;
    }

    if (czim_entry_is_redirect(entry)) {
        czim_entry *resolved = czim_archive_resolve_redirect(zim->archive, entry, 10);
        if (!resolved) {
            *out_res = NULL;
            return UNIDICT_OK;
        }
        entry = resolved;
    }

    czim_blob blob = czim_archive_get_blob(zim->archive, entry);
    const char *mime = czim_archive_mime_type(zim->archive, entry->mime_type);
    czim_entry_free(entry);

    if (!czim_blob_data(&blob)) {
        czim_blob_free(&blob);
        *out_res = NULL;
        return UNIDICT_OK;
    }

    unidict_resource *res = calloc(1, sizeof(unidict_resource));
    if (!res) {
        czim_blob_free(&blob);
        *out_res = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->key = strdup(key);
    res->size = czim_blob_size(&blob);
    res->data = malloc(res->size);
    if (!res->data) {
        czim_blob_free(&blob);
        free(res->key);
        free(res);
        *out_res = NULL;
        return UNIDICT_ERR_NOMEM;
    }
    memcpy(res->data, czim_blob_data(&blob), res->size);
    res->mime_type = mime ? strdup(mime) : NULL;

    czim_blob_free(&blob);
    *out_res = res;
    return UNIDICT_OK;
}

// ============================================================
// Resource Iterator
// ============================================================

typedef struct {
    unidict_resource_iter base;
    uint32_t pos;
    uint32_t total;
    unidict_resource_iter_mode mode;
} ud_zim_resource_iter;

static unidict_resource_iter *ud_zim_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode) {
    if (!dict) return NULL;
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    ud_zim_resource_iter *iter = calloc(1, sizeof(ud_zim_resource_iter));
    if (!iter) return NULL;

    iter->base.dict = dict;
    iter->total = czim_archive_entry_count(zim->archive);
    iter->mode = mode;

    return (unidict_resource_iter *)iter;
}

static unidict_status ud_zim_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res) {
    if (!iter || !out_res) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_zim_resource_iter *zim_iter = (ud_zim_resource_iter *)iter;
    ud_zim *zim = uobject_cast(&iter->dict->obj, ud_zim, base.obj);

    free(iter->current.key);
    iter->current.key = NULL;
    free(iter->current.data);
    iter->current.data = NULL;
    free(iter->current.mime_type);
    iter->current.mime_type = NULL;
    iter->current.size = 0;

    while (zim_iter->pos < zim_iter->total) {
        uint32_t idx = zim_iter->pos++;
        czim_entry *entry = czim_archive_get_entry_by_index(zim->archive, idx);
        if (!entry) continue;

        bool is_resource = zim_is_resource_entry(zim, entry);
        if (!is_resource) {
            czim_entry_free(entry);
            continue;
        }

        const char *path = czim_entry_get_path(entry);
        const char *mime = czim_archive_mime_type(zim->archive, entry->mime_type);

        iter->current.key = strdup(path ? path : "");

        if (zim_iter->mode == UNIDICT_RESOURCE_ITER_FULL) {
            czim_blob blob = czim_archive_get_blob(zim->archive, entry);
            if (czim_blob_data(&blob)) {
                iter->current.size = czim_blob_size(&blob);
                iter->current.data = malloc(iter->current.size);
                if (iter->current.data) {
                    memcpy(iter->current.data, czim_blob_data(&blob), iter->current.size);
                }
            }
            czim_blob_free(&blob);
            iter->current.mime_type = mime ? strdup(mime) : NULL;
        }

        czim_entry_free(entry);

        if (!iter->current.key) {
            *out_res = NULL;
            return UNIDICT_ERR_NOMEM;
        }

        *out_res = &iter->current;
        return UNIDICT_OK;
    }

    *out_res = NULL;
    return UNIDICT_DONE;
}

static void ud_zim_resource_iter_free(unidict_resource_iter *iter) {
    if (!iter) return;
    free(iter->current.key);
    free(iter->current.data);
    free(iter->current.mime_type);
    free(iter);
}

// ============================================================
// Index Activate
// ============================================================

static unidict_status ud_zim_index_activate(unidict *dict, unidict_index_type index_type) {
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    if (zim->udx_dict) {
        unidict_close(zim->udx_dict);
        zim->udx_dict = NULL;
    }
    dict->active_index = UNIDICT_INDEX_BUILTIN;

    if (index_type == UNIDICT_INDEX_EXTERNAL || index_type == UNIDICT_INDEX_NONE) {
        char *udx_path = zim_get_udx_path(zim->zim_path);
        if (udx_path) {
            unidict *udx_dict = ud_udx_open(udx_path, NULL);
            free(udx_path);

            if (udx_dict) {
                zim->udx_dict = udx_dict;
                dict->active_index = UNIDICT_INDEX_EXTERNAL;
                return UNIDICT_OK;
            }
        }

        if (index_type == UNIDICT_INDEX_EXTERNAL) {
            return UNIDICT_ERR_IO;
        }
    }

    return UNIDICT_OK;
}

// ============================================================
// Index External Make
// ============================================================

static unidict_status ud_zim_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                 void *user_data) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    // Close existing UDX before overwriting
    if (zim->udx_dict) {
        unidict_close(zim->udx_dict);
        zim->udx_dict = NULL;
        dict->active_index = UNIDICT_INDEX_NONE;
    }

    char *udx_path = zim_get_udx_path(zim->zim_path);
    if (!udx_path) return UNIDICT_ERR_INTERNAL;

    udx_writer *writer = udx_writer_open(udx_path);
    if (!writer) {
        free(udx_path);
        return UNIDICT_ERR_IO;
    }

    unidict_status ret = UNIDICT_ERR_INTERNAL;

    // Build info metadata from ZIM archive
    unidict_info meta = {0};
    char *title = zim_get_metadata_string(zim->archive, "Title");
    meta.title = title ? title : "ZIM Archive";
    char *desc = zim_get_metadata_string(zim->archive, "Description");
    meta.description = desc;
    char *creator = zim_get_metadata_string(zim->archive, "Creator");
    meta.author = creator;
    char *date = zim_get_metadata_string(zim->archive, "Date");
    meta.creation_date = date;
    char *lang = zim_get_metadata_string(zim->archive, "Language");
    meta.source_lang = lang;

    char *meta_xml = unidict_info_to_xml(&meta);
    free(title);
    free(desc);
    free(creator);
    free(date);
    free(lang);

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

    uint32_t total = czim_archive_entry_count(zim->archive);
    int last_pct = 0;
    int article_count = 0;

    for (uint32_t i = 0; i < total; i++) {
        czim_entry *entry = czim_archive_get_entry_by_index(zim->archive, i);
        if (!entry) continue;

        if (!zim_is_article_entry(zim, entry)) {
            czim_entry_free(entry);
            continue;
        }

        const char *entry_title = czim_entry_get_title(entry);
        if (!entry_title) {
            czim_entry_free(entry);
            continue;
        }

        uint32_t idx = i;
        udx_value_address address = udx_db_builder_add_value(builder, (const uint8_t *)&idx, 4);
        if (address == UDX_INVALID_ADDRESS) {
            czim_entry_free(entry);
            continue;
        }

        udx_db_builder_add_key_entry(builder, entry_title, address, 4);
        czim_entry_free(entry);

        article_count++;

        if (callback && (article_count % 500) == 0) {
            int pct = total > 0 ? (int)((uint64_t)(i + 1) * 100 / total) : 0;
            if (pct > 100) pct = 100;
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
// Index External Delete
// ============================================================

static unidict_status ud_zim_index_external_delete(unidict *dict) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);

    unidict_status st = ud_zim_index_activate(dict, UNIDICT_INDEX_BUILTIN);
    if (st != UNIDICT_OK) return st;

    char *udx_path = zim_get_udx_path(zim->zim_path);
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
// 虚函数表
// ============================================================

static unidict_status ud_zim_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict) {
        *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_zim *zim = uobject_cast(&dict->obj, ud_zim, base.obj);
    if (!zim->archive || !zim->archive->file) {
        *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    const char *path = czim_file_filename(zim->archive->file);
    if (!path) {
        *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    const char *paths[] = {path};
    *out_infos = unidict_file_infos_from_paths(paths, 1);
    return UNIDICT_OK;
}

static const unidict_ops ud_zim_ops = {
    .prepare = NULL,
    .info_get = ud_zim_info_get,
    .file_infos_get = ud_zim_file_infos_get,
    .index_activate = ud_zim_index_activate,
    .index_external_make = ud_zim_index_external_make,
    .index_external_delete = ud_zim_index_external_delete,
    .lookup = ud_zim_lookup,
    .entry_lookup = ud_zim_entry_lookup,
    .suggest = ud_zim_suggest,
    .fetch = ud_zim_fetch,
    .entry_iter_create = ud_zim_entry_iter_create,
    .entry_iter_next = ud_zim_entry_iter_next,
    .entry_iter_free = ud_zim_entry_iter_free,
    .resource_get = ud_zim_resource_get,
    .resource_iter_create = ud_zim_resource_iter_create,
    .resource_iter_next = ud_zim_resource_iter_next,
    .resource_iter_free = ud_zim_resource_iter_free,
};

// ============================================================
// 公开 API
// ============================================================

unidict *ud_zim_open(const char *path, const unidict_open_options *options) {
    if (!path) return NULL;

    czim_archive *archive = czim_archive_open(path);
    if (!archive) return NULL;

    ud_zim *zim = calloc(1, sizeof(ud_zim));
    if (!zim) {
        czim_archive_close(archive);
        return NULL;
    }

    uobject_init(&zim->base.obj, &ud_zim_type, NULL);
    zim->base.ops = &ud_zim_ops;
    zim->base.format = UNIDICT_FORMAT_ZIM;
    zim->base.has_builtin_index = true;
    zim->base.has_external_index = unidict_detect_external_index(path);
    zim->zim_path = strdup(path);
    if (!zim->zim_path) {
        czim_archive_close(archive);
        ud_zim_release((uobject *)zim);
        return NULL;
    }
    zim->archive = archive;

    unidict_index_type preset =
        (options && options->index_type != UNIDICT_INDEX_NONE) ? options->index_type : UNIDICT_INDEX_NONE;

    if (ud_zim_index_activate(&zim->base, preset) != UNIDICT_OK) {
        ud_zim_release((uobject *)zim);
        return NULL;
    }

    return &zim->base;
}
