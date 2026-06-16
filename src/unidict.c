//
//  unidict.c
//  unidict
//
//  Created by kejinlu on 2025-11-25
//
#include "unidict_internal.h"
#include "unidict_log.h"
#include "ud_mdict.h"
#include "ud_stardict.h"
#include "ud_dictd.h"
#include "ud_lingvo.h"
#include "ud_epwing.h"
#include "ud_zim.h"
#include "ud_lingoes.h"
#include "ud_babylon.h"
#include "ud_udx.h"
#include "ud_lingvo_dsl.h"
#include "utils/ud_base64.h"
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include "ud_compat.h"   // strcasecmp on Windows (no <strings.h>)
#else
#include <strings.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

// ============================================================
// Status codes
// ============================================================

const char *unidict_strerror(unidict_status status) {
    switch (status) {
    case UNIDICT_OK:
        return "success";
    case UNIDICT_DONE:
        return "iteration complete";
    case UNIDICT_ERR_NOT_FOUND:
        return "key not found";
    case UNIDICT_ERR_NO_INDEX:
        return "index not available";
    case UNIDICT_ERR_IO:
        return "I/O error";
    case UNIDICT_ERR_NOMEM:
        return "out of memory";
    case UNIDICT_ERR_INVALID_PARAM:
        return "invalid parameter";
    case UNIDICT_ERR_NOT_SUPPORTED:
        return "operation not supported";
    case UNIDICT_ERR_CORRUPT:
        return "corrupt data";
    case UNIDICT_ERR_CANCELLED:
        return "operation cancelled";
    case UNIDICT_ERR_INTERNAL:
        return "internal error";
    default:
        return "unknown error";
    }
}

// ============================================================
// Internal: Dictionary format enum
// ============================================================

const char *unidict_format_name(unidict_format format) {
    switch (format) {
        case UNIDICT_FORMAT_BABYLON:  return "babylon";
        case UNIDICT_FORMAT_DICTD:    return "dictd";
        case UNIDICT_FORMAT_EPWING:   return "epwing";
        case UNIDICT_FORMAT_LINGOES:  return "lingoes";
        case UNIDICT_FORMAT_LINGVO:   return "lingvo";
        case UNIDICT_FORMAT_MDICT:    return "mdict";
        case UNIDICT_FORMAT_STARDICT: return "stardict";
        case UNIDICT_FORMAT_UDX:      return "udx";
        case UNIDICT_FORMAT_ZIM:      return "zim";
        case UNIDICT_FORMAT_DSL:      return "dsl";
        default:                      return "unknown";
    }
}
// ============================================================

static bool is_epwing_directory(const char *path) {
    if (!path) {
        return false;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }

    char catalogs_path[PATH_MAX];
    snprintf(catalogs_path, sizeof(catalogs_path), "%s/CATALOGS", path);

    if (stat(catalogs_path, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }

    snprintf(catalogs_path, sizeof(catalogs_path), "%s/catalogs", path);
    if (stat(catalogs_path, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }

    return false;
}

static unidict_format detect_dict_format(const char *file_path) {
    if (!file_path) {
        return UNIDICT_FORMAT_UNKNOWN;
    }

    if (is_epwing_directory(file_path)) {
        return UNIDICT_FORMAT_EPWING;
    }

    const char *ext = strrchr(file_path, '.');
    if (!ext) {
        return UNIDICT_FORMAT_UNKNOWN;
    }

    if (strcasecmp(ext, ".mdx") == 0 || strcasecmp(ext, ".mdd") == 0) {
        return UNIDICT_FORMAT_MDICT;
    }

    if (strcasecmp(ext, ".ifo") == 0) {
        return UNIDICT_FORMAT_STARDICT;
    }

    if (strcasecmp(ext, ".index") == 0) {
        return UNIDICT_FORMAT_DICTD;
    }

    if (strcasecmp(ext, ".lsd") == 0) {
        return UNIDICT_FORMAT_LINGVO;
    }

    if (strcasecmp(ext, ".zim") == 0) {
        return UNIDICT_FORMAT_ZIM;
    }

    if (strcasecmp(ext, ".ld2") == 0 || strcasecmp(ext, ".ldx") == 0) {
        return UNIDICT_FORMAT_LINGOES;
    }

    if (strcasecmp(ext, ".bgl") == 0) {
        return UNIDICT_FORMAT_BABYLON;
    }

    if (strcasecmp(ext, ".udx") == 0) {
        return UNIDICT_FORMAT_UDX;
    }

    // .dsl or .dsl.dz
    size_t len = strlen(file_path);
    if (len >= 7 && strcasecmp(file_path + len - 7, ".dsl.dz") == 0) {
        return UNIDICT_FORMAT_DSL;
    }
    if (strcasecmp(ext, ".dsl") == 0) {
        return UNIDICT_FORMAT_DSL;
    }

    return UNIDICT_FORMAT_UNKNOWN;
}

// ============================================================
// Lifecycle
// ============================================================

unidict_status unidict_open(const char *file_path, const unidict_open_options *options, unidict **out_dict) {
    if (!file_path || !out_dict) {
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_dict = NULL;

    unidict_format format = detect_dict_format(file_path);
    if (format == UNIDICT_FORMAT_UNKNOWN) {
        UD_LOG_ERROR("unrecognized dictionary format: %s", file_path);
        return UNIDICT_ERR_NOT_SUPPORTED;
    }

    unidict *dict = NULL;

    switch (format) {
    case UNIDICT_FORMAT_BABYLON:
        dict = (unidict *)ud_babylon_open(file_path, options);
        break;

    case UNIDICT_FORMAT_DICTD:
        dict = (unidict *)ud_dictd_open(file_path, options);
        break;

    case UNIDICT_FORMAT_EPWING:
        dict = (unidict *)ud_epwing_open(file_path, options);
        break;

    case UNIDICT_FORMAT_LINGOES:
        dict = (unidict *)ud_lingoes_open(file_path, options);
        break;

    case UNIDICT_FORMAT_LINGVO:
        dict = (unidict *)ud_lingvo_open(file_path, options);
        break;

    case UNIDICT_FORMAT_MDICT:
        dict = (unidict *)ud_mdict_open(file_path, options);
        break;

    case UNIDICT_FORMAT_STARDICT:
        dict = (unidict *)ud_stardict_open(file_path, options);
        break;

    case UNIDICT_FORMAT_UDX:
        dict = (unidict *)ud_udx_open(file_path, options);
        break;

    case UNIDICT_FORMAT_ZIM:
        dict = (unidict *)ud_zim_open(file_path, options);
        break;

    case UNIDICT_FORMAT_DSL:
        dict = (unidict *)ud_lingvo_dsl_open(file_path, options);
        break;

    default:
        break;
    }

    if (!dict) {
        return UNIDICT_ERR_INTERNAL;
    }

    *out_dict = dict;
    return UNIDICT_OK;
}

void unidict_close(unidict *dict) {
    if (dict) {
        uobject_release(&dict->obj);
    }
}

unidict_status unidict_prepare(unidict *dict) {
    if (!dict) {
        return UNIDICT_ERR_INVALID_PARAM;
    }

    if (dict->prepared) {
        return UNIDICT_OK;
    }

    if (dict->ops && dict->ops->prepare) {
        unidict_status ret = dict->ops->prepare(dict);
        if (ret != UNIDICT_OK) {
            return ret;
        }
    }

    dict->prepared = true;
    return UNIDICT_OK;
}

unidict_status unidict_index_activate(unidict *dict, unidict_index_type index_type) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    if (!dict->ops || !dict->ops->index_activate) return UNIDICT_ERR_NOT_SUPPORTED;

    switch (index_type) {
    case UNIDICT_INDEX_NONE:
        break;
    case UNIDICT_INDEX_BUILTIN:
        if (!dict->has_builtin_index) return UNIDICT_ERR_NO_INDEX;
        break;
    case UNIDICT_INDEX_EXTERNAL:
        if (!dict->has_external_index) return UNIDICT_ERR_NO_INDEX;
        break;
    default:
        return UNIDICT_ERR_INVALID_PARAM;
    }

    return dict->ops->index_activate(dict, index_type);
}

static void unidict_ensure_prepared(unidict *dict) {
    if (dict && !dict->prepared) {
        unidict_prepare(dict);
    }
}

// ============================================================
// Info
// ============================================================

unidict_status unidict_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict) {
        UD_LOG_ERROR("dict is NULL");
        return UNIDICT_ERR_INVALID_PARAM;
    }
    if (!out_info) {
        return UNIDICT_ERR_INVALID_PARAM;
    }

    unidict_ensure_prepared(dict);

    if (!dict->ops || !dict->ops->info_get) {
        *out_info = malloc(sizeof(unidict_info));
        if (!*out_info) {
            UD_LOG_ERROR("failed to allocate info");
            return UNIDICT_ERR_NOMEM;
        }

        (*out_info)->format = dict->format;
        (*out_info)->title = NULL;
        (*out_info)->description = NULL;
        (*out_info)->author = NULL;
        (*out_info)->email = NULL;
        (*out_info)->creation_date = NULL;
        (*out_info)->source_lang = NULL;
        (*out_info)->target_lang = NULL;
        (*out_info)->word_count = 0;
        (*out_info)->subitem_count = 0;

        return UNIDICT_OK;
    }

    return dict->ops->info_get(dict, out_info);
}

void unidict_info_free(unidict_info *info) {
    if (!info) return;

    free(info->title);
    free(info->subtitle);
    free(info->description);
    free(info->author);
    free(info->email);
    free(info->creation_date);
    free(info->source_lang);
    free(info->target_lang);
    free(info->icon_data);
    free(info->icon_mime_type);
    free(info->edition);
    free(info->format_version);
    free(info);
}

// ============================================================
// Info XML Serialization
// ============================================================

static void xml_set_field(xmlNodePtr parent, const char *name, const char *value) {
    if (!value) return;
    xmlNewTextChild(parent, NULL, BAD_CAST name, BAD_CAST value);
}

static void xml_set_info_field(unidict_info *info, const char *name, const char *value) {
    if (!value) return;
    if (strcmp(name, "title") == 0)              info->title = strdup(value);
    else if (strcmp(name, "subtitle") == 0)      info->subtitle = strdup(value);
    else if (strcmp(name, "description") == 0)    info->description = strdup(value);
    else if (strcmp(name, "author") == 0)         info->author = strdup(value);
    else if (strcmp(name, "email") == 0)          info->email = strdup(value);
    else if (strcmp(name, "creation_date") == 0)  info->creation_date = strdup(value);
    else if (strcmp(name, "source_lang") == 0)    info->source_lang = strdup(value);
    else if (strcmp(name, "target_lang") == 0)    info->target_lang = strdup(value);
    else if (strcmp(name, "format_version") == 0) info->format_version = strdup(value);
    else if (strcmp(name, "edition") == 0)        info->edition = strdup(value);
    else if (strcmp(name, "icon_mime_type") == 0) info->icon_mime_type = strdup(value);
    else if (strcmp(name, "icon_data") == 0)      {
        size_t len = 0;
        uint8_t *decoded = ud_base64_decode(value, &len);
        if (decoded) {
            info->icon_data = decoded;
            info->icon_size = len;
        }
    }
}

char *unidict_info_to_xml(const unidict_info *info) {
    if (!info) return NULL;

    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    xmlNodePtr root = xmlNewNode(NULL, BAD_CAST "info");
    xmlDocSetRootElement(doc, root);

    xml_set_field(root, "title", info->title);
    xml_set_field(root, "subtitle", info->subtitle);
    xml_set_field(root, "description", info->description);
    xml_set_field(root, "author", info->author);
    xml_set_field(root, "email", info->email);
    xml_set_field(root, "creation_date", info->creation_date);
    xml_set_field(root, "source_lang", info->source_lang);
    xml_set_field(root, "target_lang", info->target_lang);
    xml_set_field(root, "format_version", info->format_version);
    xml_set_field(root, "edition", info->edition);

    // Icon (Base64 encoded)
    if (info->icon_data && info->icon_size > 0) {
        xml_set_field(root, "icon_mime_type", info->icon_mime_type);
        char *b64 = ud_base64_encode(info->icon_data, info->icon_size);
        if (b64) {
            xml_set_field(root, "icon_data", b64);
            free(b64);
        }
    }

    xmlChar *buf = NULL;
    int buf_size = 0;
    xmlDocDumpMemory(doc, &buf, &buf_size);
    xmlFreeDoc(doc);

    char *result = NULL;
    if (buf && buf_size > 0) {
        result = malloc((size_t)buf_size + 1);
        if (result) {
            memcpy(result, buf, (size_t)buf_size);
            result[buf_size] = '\0';
        }
    }
    xmlFree(buf);
    return result;
}

unidict_info *unidict_info_from_xml(const char *xml) {
    if (!xml) return NULL;

    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), NULL, NULL, XML_PARSE_NOBLANKS);
    if (!doc) return NULL;

    unidict_info *info = calloc(1, sizeof(unidict_info));
    if (!info) {
        xmlFreeDoc(doc);
        return NULL;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root) {
        for (xmlNodePtr cur = root->children; cur; cur = cur->next) {
            if (cur->type != XML_ELEMENT_NODE) continue;
            xmlChar *content = xmlNodeGetContent(cur);
            if (content && *content) {
                xml_set_info_field(info, (const char *)cur->name, (const char *)content);
            }
            xmlFree(content);
        }
    }

    xmlFreeDoc(doc);
    return info;
}

// ============================================================
// File List
// ============================================================

unidict_file_info_array *unidict_file_infos_from_paths(const char **paths, int count) {
    if (!paths || count <= 0) return NULL;

    unidict_file_info_array *list = calloc(1, sizeof(unidict_file_info_array));
    if (!list) return NULL;

    list->items = calloc(count, sizeof(unidict_file_info));
    if (!list->items) {
        free(list);
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) continue;

        list->items[list->count].path = strdup(paths[i]);
        if (!list->items[list->count].path) continue;

        list->items[list->count].size = (uint64_t)st.st_size;
        list->items[list->count].last_modified = (int64_t)st.st_mtime;
        list->count++;
    }

    if (list->count == 0) {
        free(list->items);
        free(list);
        return NULL;
    }

    return list;
}

bool unidict_detect_external_index(const char *source_path) {
    if (!source_path) return false;

    const char *ext = strrchr(source_path, '.');
    if (!ext) return false;

    size_t base_len = ext - source_path;
    char *udx_path = (char *)malloc(base_len + 5);
    if (!udx_path) return false;

    snprintf(udx_path, base_len + 5, "%.*s.udx", (int)base_len, source_path);

    struct stat st;
    bool exists = (stat(udx_path, &st) == 0);
    free(udx_path);

    return exists;
}

unidict_status unidict_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict || !out_infos) return UNIDICT_ERR_INVALID_PARAM;
    unidict_ensure_prepared(dict);
    if (!dict->ops || !dict->ops->file_infos_get) return UNIDICT_ERR_NOT_SUPPORTED;
    return dict->ops->file_infos_get(dict, out_infos);
}

void unidict_file_info_array_free(unidict_file_info_array *array) {
    if (!array) return;

    for (size_t i = 0; i < array->count; i++) {
        free(array->items[i].path);
    }

    free(array->items);
    free(array);
}

// ============================================================
// Index
// ============================================================

unidict_index_type unidict_index_get_active(unidict *dict) {
    if (!dict) {
        return UNIDICT_INDEX_NONE;
    }
    return dict->active_index;
}

bool unidict_index_has_builtin(unidict *dict) {
    return dict && dict->has_builtin_index;
}

bool unidict_index_has_external(unidict *dict) {
    return dict && dict->has_external_index;
}

unidict_status unidict_index_external_make(unidict *dict, unidict_index_external_make_cb callback, void *user_data) {
    if (!dict) {
        UD_LOG_ERROR("dict is NULL");
        return UNIDICT_ERR_INVALID_PARAM;
    }

    if (!dict->ops || !dict->ops->index_external_make) {
        UD_LOG_ERROR("index_external_make not implemented for %s format", unidict_format_name(dict->format));
        return UNIDICT_ERR_NOT_SUPPORTED;
    }

    unidict_status result = dict->ops->index_external_make(dict, callback, user_data);
    if (result != UNIDICT_OK) {
        UD_LOG_ERROR("failed to build external index");
        return result;
    }

    dict->has_external_index = true;

    return UNIDICT_OK;
}

unidict_status unidict_index_external_delete(unidict *dict) {
    if (!dict) {
        return UNIDICT_ERR_INVALID_PARAM;
    }

    if (!dict->ops || !dict->ops->index_external_delete) {
        return UNIDICT_ERR_NOT_SUPPORTED;
    }

    unidict_status result = dict->ops->index_external_delete(dict);
    if (result == UNIDICT_OK) {
        dict->has_external_index = false;
    }
    return result;
}

// ============================================================
// Lookup
// ============================================================

unidict_status unidict_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key || !out_articles) {
        UD_LOG_ERROR("dict or key is NULL");
        return UNIDICT_ERR_INVALID_PARAM;
    }

    unidict_ensure_prepared(dict);

    if (!dict->ops || !dict->ops->lookup) {
        UD_LOG_ERROR("lookup not implemented for %s format", unidict_format_name(dict->format));
        return UNIDICT_ERR_NOT_SUPPORTED;
    }

    return dict->ops->lookup(dict, key, out_articles);
}

void unidict_article_array_free(unidict_article_array *array) {
    if (!array) {
        return;
    }

    for (size_t i = 0; i < array->count; i++) {
        free(array->items[i].title);
        free(array->items[i].body);
    }

    free(array->items);
    free(array);
}

unidict_status unidict_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key || !out_entries) {
        UD_LOG_ERROR("dict or key is NULL");
        return UNIDICT_ERR_INVALID_PARAM;
    }

    unidict_ensure_prepared(dict);

    if (!dict->ops || !dict->ops->entry_lookup) {
        UD_LOG_ERROR("entry_lookup not implemented for %s format", unidict_format_name(dict->format));
        return UNIDICT_ERR_NOT_SUPPORTED;
    }

    return dict->ops->entry_lookup(dict, key, out_entries);
}

unidict_status unidict_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !out_articles) {
        UD_LOG_ERROR("dict or entry is NULL");
        return UNIDICT_ERR_INVALID_PARAM;
    }

    unidict_ensure_prepared(dict);

    if (!dict->ops || !dict->ops->fetch) {
        UD_LOG_ERROR("lookup_by_entry not implemented for %s format", unidict_format_name(dict->format));
        return UNIDICT_ERR_NOT_SUPPORTED;
    }

    return dict->ops->fetch(dict, entry, out_articles);
}

unidict_status unidict_resource_get(unidict *dict, const char *key, unidict_resource **out_res) {
    if (!dict || !key || !out_res) return UNIDICT_ERR_INVALID_PARAM;
    unidict_ensure_prepared(dict);
    if (!dict->ops || !dict->ops->resource_get) return UNIDICT_ERR_NOT_SUPPORTED;
    return dict->ops->resource_get(dict, key, out_res);
}

void unidict_resource_free(unidict_resource *res) {
    if (!res) return;
    free(res->key);
    free(res->data);
    free(res->mime_type);
    free(res);
}

// ============================================================
// Feature Pages
// ============================================================

unidict_status unidict_feature_pages_list(unidict *dict, unidict_feature_page_array **out_pages) {
    if (!dict || !out_pages) return UNIDICT_ERR_INVALID_PARAM;
    *out_pages = NULL;
    unidict_ensure_prepared(dict);
    if (!dict->ops || !dict->ops->feature_pages_list) {
        unidict_feature_page_array *arr = calloc(1, sizeof(*arr));
        if (!arr) return UNIDICT_ERR_NOMEM;
        *out_pages = arr;
        return UNIDICT_OK;
    }
    return dict->ops->feature_pages_list(dict, out_pages);
}

unidict_status unidict_feature_page_read(unidict *dict, const char *key, char **out_html) {
    if (!dict || !key || !out_html) return UNIDICT_ERR_INVALID_PARAM;
    *out_html = NULL;
    unidict_ensure_prepared(dict);
    if (!dict->ops || !dict->ops->feature_page_read) return UNIDICT_ERR_NOT_SUPPORTED;
    return dict->ops->feature_page_read(dict, key, out_html);
}

void unidict_feature_page_array_free(unidict_feature_page_array *array) {
    if (!array) return;
    for (size_t i = 0; i < array->count; i++) {
        free(array->items[i].key);
        free(array->items[i].name);
    }
    free(array->items);
    free(array);
}

// ============================================================
// Suggest
// ============================================================

unidict_status unidict_suggest(unidict *dict, const char *prefix, int limit, unidict_entry_array **out_entries) {
    if (!dict || !prefix || !out_entries) {
        UD_LOG_ERROR("dict or prefix is NULL");
        return UNIDICT_ERR_INVALID_PARAM;
    }

    unidict_ensure_prepared(dict);

    if (!dict->ops || !dict->ops->suggest) {
        UD_LOG_ERROR("suggest not implemented for %s format", unidict_format_name(dict->format));
        return UNIDICT_ERR_NOT_SUPPORTED;
    }

    return dict->ops->suggest(dict, prefix, limit, out_entries);
}

static void unidict_entry_free(unidict_entry *entry) {
    if (!entry) {
        return;
    }

    if (entry->key) {
        free(entry->key);
        entry->key = NULL;
    }

    if (entry->internal_entry) {
        uobject_release(entry->internal_entry);
        entry->internal_entry = NULL;
    }
}

void unidict_entry_array_free(unidict_entry_array *array) {
    if (!array) {
        return;
    }

    for (size_t i = 0; i < array->count; i++) {
        if (array->items[i]) {
            unidict_entry_free(array->items[i]);
        }
    }

    free(array->items);
    free(array);
}

// ============================================================
// Entry Iterator
// ============================================================

unidict_status unidict_entry_iter_create(unidict *dict, unidict_entry_iter **out_iter) {
    if (!dict || !out_iter) {
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_iter = NULL;
    unidict_ensure_prepared(dict);

    if (!dict->ops || !dict->ops->entry_iter_create) {
        return UNIDICT_ERR_NOT_SUPPORTED;
    }

    unidict_entry_iter *iter = dict->ops->entry_iter_create(dict);
    if (!iter) {
        return UNIDICT_ERR_INTERNAL;
    }

    *out_iter = iter;
    return UNIDICT_OK;
}

unidict_status unidict_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !out_entry) {
        return UNIDICT_ERR_INVALID_PARAM;
    }

    if (!iter->dict || !iter->dict->ops || !iter->dict->ops->entry_iter_next) {
        return UNIDICT_ERR_NOT_SUPPORTED;
    }

    return iter->dict->ops->entry_iter_next(iter, out_entry);
}

void unidict_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) {
        return;
    }

    if (!iter->dict || !iter->dict->ops || !iter->dict->ops->entry_iter_free) {
        free(iter);
        return;
    }

    iter->dict->ops->entry_iter_free(iter);
}

// ============================================================
// Resource Iterator
// ============================================================

unidict_status unidict_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode,
                                            unidict_resource_iter **out_iter) {
    if (!dict || !out_iter) return UNIDICT_ERR_INVALID_PARAM;
    *out_iter = NULL;
    unidict_ensure_prepared(dict);
    if (!dict->ops || !dict->ops->resource_iter_create) return UNIDICT_ERR_NOT_SUPPORTED;

    unidict_resource_iter *iter = dict->ops->resource_iter_create(dict, mode);
    if (!iter) return UNIDICT_ERR_INTERNAL;

    *out_iter = iter;
    return UNIDICT_OK;
}

unidict_status unidict_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res) {
    if (!iter || !out_res) return UNIDICT_ERR_INVALID_PARAM;
    if (!iter->dict || !iter->dict->ops || !iter->dict->ops->resource_iter_next) return UNIDICT_ERR_NOT_SUPPORTED;
    return iter->dict->ops->resource_iter_next(iter, out_res);
}

void unidict_resource_iter_free(unidict_resource_iter *iter) {
    if (!iter) return;
    if (!iter->dict || !iter->dict->ops || !iter->dict->ops->resource_iter_free) {
        free(iter);
        return;
    }
    iter->dict->ops->resource_iter_free(iter);
}
