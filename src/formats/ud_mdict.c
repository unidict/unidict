//
//  ud_mdict.c
//  unidict
//
//  Created by kejinlu on 2025-11-25
//
#include "ud_mdict.h"
#include "unidict_internal.h"
#include "cmdx_reader.h"
#include "cmdx_key_section.h"
#include "udx_writer.h"
#include "ud_udx.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _MSC_VER
#include "ud_compat.h"
#else
#include <strings.h>
#include <dirent.h>
#endif
#include <sys/stat.h>
#include <iconv.h>
#include <errno.h>

// ============================================================
// MDD finder helpers
// ============================================================

typedef struct {
    char **paths;
    size_t count;
} ud_file_list;

static void ud_file_list_free(ud_file_list *list) {
    if (!list) return;
    if (list->paths) {
        for (size_t i = 0; i < list->count; i++) free(list->paths[i]);
        free(list->paths);
    }
    free(list);
}

static char *extract_directory_path(const char *file_path) {
    if (!file_path) return NULL;
    const char *last_slash = strrchr(file_path, '/');
    if (!last_slash) return strdup(".");
    size_t dir_len = last_slash - file_path;
    if (dir_len == 0) return strdup(".");
    char *dir_path = malloc(dir_len + 1);
    if (!dir_path) return NULL;
    memcpy(dir_path, file_path, dir_len);
    dir_path[dir_len] = '\0';
    return dir_path;
}

static char *extract_basename_without_ext(const char *file_path) {
    if (!file_path) return NULL;
    const char *filename = strrchr(file_path, '/');
    filename = filename ? filename + 1 : file_path;
    const char *last_dot = strrchr(filename, '.');
    if (!last_dot || last_dot == filename) return strdup(filename);
    size_t basename_len = last_dot - filename;
    char *basename = malloc(basename_len + 1);
    if (basename) {
        memcpy(basename, filename, basename_len);
        basename[basename_len] = '\0';
    }
    return basename;
}

static int check_file_extension(const char *file_path, const char *extension) {
    if (!file_path || !extension) return 0;
    const char *dot = strrchr(file_path, '.');
    if (!dot) return 0;
    dot++;
    while (*dot && *extension) {
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*extension)) return 0;
        dot++;
        extension++;
    }
    return (*dot == '\0' && *extension == '\0');
}

static int is_mdd_filename_match(const char *mdd_basename, const char *mdx_basename) {
    if (!mdd_basename || !mdx_basename) return 0;
    size_t mdx_len = strlen(mdx_basename);
    size_t mdd_len = strlen(mdd_basename);
    if (strcmp(mdd_basename, mdx_basename) == 0) return 1;
    if (mdd_len > mdx_len + 1 && strncmp(mdd_basename, mdx_basename, mdx_len) == 0 && mdd_basename[mdx_len] == '.') {
        int all_digits = 1;
        for (size_t i = mdx_len + 1; i < mdd_len; i++) {
            if (!isdigit((unsigned char)mdd_basename[i])) {
                all_digits = 0;
                break;
            }
        }
        if (all_digits) return 1;
    }
    if (mdd_len > mdx_len + 2 && strncmp(mdd_basename, mdx_basename, mdx_len) == 0 && mdd_basename[mdx_len] == '.' &&
        isalpha((unsigned char)mdd_basename[mdx_len + 1])) {
        int all_digits = 1;
        for (size_t i = mdx_len + 2; i < mdd_len; i++) {
            if (!isdigit((unsigned char)mdd_basename[i])) {
                all_digits = 0;
                break;
            }
        }
        if (all_digits) return 1;
    }
    return 0;
}

static ud_file_list *ud_get_mdd_paths_for_mdx(const char *mdx_path) {
    if (!mdx_path) return NULL;
    char *mdx_basename = extract_basename_without_ext(mdx_path);
    if (!mdx_basename) return NULL;
    char *dir_path = extract_directory_path(mdx_path);
    if (!dir_path) {
        free(mdx_basename);
        return NULL;
    }

    int capacity = 16, count = 0;
    char **paths = malloc(capacity * sizeof(char *));
    if (!paths) {
        free(mdx_basename);
        free(dir_path);
        return NULL;
    }

    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (!check_file_extension(entry->d_name, "mdd")) continue;
            char *mdd_basename = extract_basename_without_ext(entry->d_name);
            if (!mdd_basename) continue;
            if (is_mdd_filename_match(mdd_basename, mdx_basename)) {
                if (count >= capacity) {
                    int new_capacity = capacity * 2;
                    char **new_paths = realloc(paths, new_capacity * sizeof(char *));
                    if (!new_paths) {
                        for (int i = 0; i < count; i++) free(paths[i]);
                        free(paths);
                        free(mdd_basename);
                        free(dir_path);
                        closedir(dir);
                        free(mdx_basename);
                        return NULL;
                    }
                    paths = new_paths;
                    capacity = new_capacity;
                }
                size_t full_path_len = strlen(dir_path) + 1 + strlen(entry->d_name) + 1;
                char *full_path = malloc(full_path_len);
                if (full_path) {
                    snprintf(full_path, full_path_len, "%s/%s", dir_path, entry->d_name);
                    paths[count++] = full_path;
                }
            }
            free(mdd_basename);
        }
        closedir(dir);
    }
    free(mdx_basename);
    free(dir_path);

    if (count == 0) {
        free(paths);
        return NULL;
    }

    ud_file_list *list = malloc(sizeof(ud_file_list));
    if (!list) {
        for (int i = 0; i < count; i++) free(paths[i]);
        free(paths);
        return NULL;
    }
    list->paths = paths;
    list->count = count;
    return list;
}

// ============================================================
// Struct definition (private to this translation unit)
// ============================================================

typedef struct ud_mdict ud_mdict;

struct ud_mdict {
    unidict base;
    char *mdx_path;
    cmdx_reader *mdx_reader;
    cmdx_reader **mdd_readers;
    int mdd_reader_count;

    unidict *udx_dict;
};

// ============================================================
// UDX path helper
// ============================================================

static char *mdict_get_udx_path(const char *mdx_path) {
    size_t len = strlen(mdx_path);
    const char *dot = strrchr(mdx_path, '.');
    if (dot) len = dot - mdx_path;
    char *udx_path = malloc(len + 5);
    if (!udx_path) return NULL;
    snprintf(udx_path, len + 5, "%.*s.udx", (int)len, mdx_path);
    return udx_path;
}

// UDX value for article: content_logical_offset (8 bytes) + content_size (8 bytes) = 16 bytes
// UDX value for resource: mdd_index (4 bytes) + content_logical_offset (8 bytes) + content_size (8 bytes) = 20 bytes

static void mdict_pack_article_ref(uint64_t offset, uint64_t size, uint8_t out[16]) {
    memcpy(out, &offset, 8);
    memcpy(out + 8, &size, 8);
}

static void mdict_unpack_article_ref(const uint8_t *data, size_t data_size, uint64_t *offset, uint64_t *size) {
    *offset = 0;
    *size = 0;
    if (data_size >= 16) {
        memcpy(offset, data, 8);
        memcpy(size, data + 8, 8);
    }
}

static void mdict_pack_resource_ref(int mdd_index, uint64_t offset, uint64_t size, uint8_t out[20]) {
    uint32_t idx = (uint32_t)mdd_index;
    memcpy(out, &idx, 4);
    memcpy(out + 4, &offset, 8);
    memcpy(out + 12, &size, 8);
}

static void mdict_unpack_resource_ref(const uint8_t *data, size_t data_size, int *mdd_index, uint64_t *offset,
                                      uint64_t *size) {
    *mdd_index = 0;
    *offset = 0;
    *size = 0;
    if (data_size >= 20) {
        uint32_t idx;
        memcpy(&idx, data, 4);
        memcpy(offset, data + 4, 8);
        memcpy(size, data + 12, 8);
        *mdd_index = (int)idx;
    }
}

// ============================================================
// UDX entry ref helpers
// ============================================================

// For article: stores content_logical_offset + content_size
typedef struct {
    uobject obj;
    uint64_t content_offset;
    uint64_t content_size;
} ud_mdict_article_entry;

static void ud_mdict_article_entry_release(uobject *obj) {
    ud_mdict_article_entry *entry = uobject_cast(obj, ud_mdict_article_entry, obj);
    free(entry);
}

static const uobject_type ud_mdict_article_entry_type = {
    .name = "ud_mdict_article_entry",
    .size = sizeof(ud_mdict_article_entry),
    .release = ud_mdict_article_entry_release,
};

static ud_mdict_article_entry *mdict_article_entry_from_udx_value(const udx_value_entry_item *item) {
    if (!item || !item->data || item->size < 16) return NULL;

    ud_mdict_article_entry *ref = calloc(1, sizeof(ud_mdict_article_entry));
    if (ref) {
        uobject_init(&ref->obj, &ud_mdict_article_entry_type, NULL);
        mdict_unpack_article_ref(item->data, item->size, &ref->content_offset, &ref->content_size);
    }
    return ref;
}

// ============================================================
// Key entry wrapper (for unidict_entry.internal_entry)
// ============================================================

typedef struct {
    uobject obj;
    cmdx_key_entry *entry;
} ud_cmdx_key_entry;

static void ud_cmdx_key_entry_release(uobject *obj) {
    if (!obj) return;
    ud_cmdx_key_entry *wrapper = uobject_cast(obj, ud_cmdx_key_entry, obj);
    if (wrapper->entry) uobject_release(&wrapper->entry->obj);
    free(wrapper);
}

static const uobject_type ud_cmdx_key_entry_type = {
    .name = "ud_cmdx_key_entry",
    .size = sizeof(ud_cmdx_key_entry),
    .release = ud_cmdx_key_entry_release,
};

static ud_cmdx_key_entry *ud_cmdx_key_entry_create(cmdx_key_entry *entry) {
    if (!entry) return NULL;
    ud_cmdx_key_entry *wrapper = calloc(1, sizeof(ud_cmdx_key_entry));
    if (!wrapper) return NULL;
    uobject_init(&wrapper->obj, &ud_cmdx_key_entry_type, NULL);
    wrapper->entry = entry;
    uobject_retain(&entry->obj);
    return wrapper;
}

// ============================================================
// Encoding conversion
// ============================================================

static const char *encoding_to_iconv_name(cmdx_encoding enc) {
    switch (enc) {
    case CMDX_ENCODING_UTF16:
        return "UTF-16LE";
    case CMDX_ENCODING_BIG5:
        return "BIG5";
    case CMDX_ENCODING_GBK:
        return "GBK";
    case CMDX_ENCODING_GB2312:
        return "GB2312";
    case CMDX_ENCODING_GB18030:
        return "GB18030";
    default:
        return "UTF-8";
    }
}

static char *cmdx_data_to_utf8(const uint8_t *data, size_t len, cmdx_encoding encoding) {
    if (!data || len == 0) return strdup("");

    if (encoding == CMDX_ENCODING_UTF8 || encoding == CMDX_ENCODING_UNKNOWN) {
        return strndup((const char *)data, len);
    }

    const char *from = encoding_to_iconv_name(encoding);
    iconv_t cd = iconv_open("UTF-8", from);
    if (cd == (iconv_t)-1) return strndup((const char *)data, len);

    size_t outbuf_size = len * 4;
    char *outbuf = malloc(outbuf_size);
    if (!outbuf) {
        iconv_close(cd);
        return NULL;
    }

    char *inptr = (char *)data;
    char *outptr = outbuf;
    size_t inbytes = len;
    size_t outbytes = outbuf_size;

    size_t ret = iconv(cd, &inptr, &inbytes, &outptr, &outbytes);
    iconv_close(cd);

    if (ret == -1) {
        free(outbuf);
        return strndup((const char *)data, len);
    }

    size_t outlen = outbuf_size - outbytes;
    char *result = realloc(outbuf, outlen + 1);
    if (!result) result = outbuf;
    result[outlen] = '\0';
    return result;
}

// ============================================================
// Release & type
// ============================================================

static void ud_mdict_release(uobject *obj) {
    if (!obj) return;
    ud_mdict *mdict = uobject_cast(obj, ud_mdict, base.obj);

    if (mdict->udx_dict) {
        unidict_close(mdict->udx_dict);
        mdict->udx_dict = NULL;
    }

    if (mdict->mdx_reader) {
        cmdx_reader_close(mdict->mdx_reader);
        mdict->mdx_reader = NULL;
    }

    if (mdict->mdd_readers) {
        for (int i = 0; i < mdict->mdd_reader_count; i++) {
            if (mdict->mdd_readers[i]) cmdx_reader_close(mdict->mdd_readers[i]);
        }
        free(mdict->mdd_readers);
    }

    free(mdict->mdx_path);
    free(mdict);
}

static const uobject_type ud_mdict_type = {
    .name = "ud_mdict",
    .size = sizeof(ud_mdict),
    .release = ud_mdict_release,
};

// ============================================================
// VTable
// ============================================================

static unidict_status mdict_info_get(unidict *dict, unidict_info **out_info);
static unidict_status mdict_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status mdict_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status mdict_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status mdict_suggest(unidict *dict, const char *prefix, size_t limit, unidict_entry_array **out_entries);
static unidict_status mdict_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_status mdict_index_activate(unidict *dict, unidict_index_type index_type);
static unidict_status mdict_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                void *user_data);
static unidict_status mdict_index_external_delete(unidict *dict);
static unidict_entry_iter *mdict_entry_iter_create(unidict *dict);
static unidict_status mdict_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void mdict_entry_iter_free(unidict_entry_iter *iter);
static unidict_status mdict_resource_get(unidict *dict, const char *key, unidict_resource **out_res);
static unidict_resource_iter *mdict_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode);
static unidict_status mdict_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res);
static void mdict_resource_iter_free(unidict_resource_iter *iter);

static const unidict_ops mdict_ops = {
    .prepare = NULL,
    .info_get = mdict_info_get,
    .file_infos_get = mdict_file_infos_get,
    .index_activate = mdict_index_activate,
    .index_external_make = mdict_index_external_make,
    .index_external_delete = mdict_index_external_delete,
    .lookup = mdict_lookup,
    .entry_lookup = mdict_entry_lookup,
    .suggest = mdict_suggest,
    .fetch = mdict_fetch,
    .entry_iter_create = mdict_entry_iter_create,
    .entry_iter_next = mdict_entry_iter_next,
    .entry_iter_free = mdict_entry_iter_free,
    .resource_get = mdict_resource_get,
    .resource_iter_create = mdict_resource_iter_create,
    .resource_iter_next = mdict_resource_iter_next,
    .resource_iter_free = mdict_resource_iter_free,
};

// ============================================================
// Public API
// ============================================================

static unidict_status mdict_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    cmdx_key_entry_list *key_list = cmdx_get_key_entries_by_key(mdict->mdx_reader, (char *)key, 100, false);

    if (!key_list || key_list->count == 0) {
        if (key_list) cmdx_key_entry_list_free(key_list);
        *out_entries = NULL;
        return UNIDICT_OK;
    }

    unidict_entry_array *res = malloc(sizeof(unidict_entry_array));
    if (!res) {
        cmdx_key_entry_list_free(key_list);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->count = key_list->count;
    res->items = calloc(key_list->count, sizeof(unidict_entry *));
    if (!res->items) {
        free(res);
        cmdx_key_entry_list_free(key_list);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    for (size_t i = 0; i < key_list->count; i++) {
        cmdx_key_entry *entry = key_list->items[i];
        if (!entry) continue;

        ud_cmdx_key_entry *wrapper = ud_cmdx_key_entry_create(entry);
        if (!wrapper) continue;

        unidict_entry *entry_info = calloc(1, sizeof(unidict_entry));
        if (!entry_info) {
            uobject_release(&wrapper->obj);
            continue;
        }

        entry_info->key = strdup(cmdx_key_entry_get_key(entry));
        entry_info->internal_entry = &wrapper->obj;
        res->items[i] = entry_info;
    }

    cmdx_key_entry_list_free(key_list);
    *out_entries = res;
    return UNIDICT_OK;
}

static unidict_status mdict_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    cmdx_data_list *list = cmdx_get_content_records_by_key(mdict->mdx_reader, (char *)key, 0, false);
    if (!list || list->count == 0) {
        if (list) cmdx_data_list_free(list);
        *out_articles = NULL;
        return UNIDICT_OK;
    }

    unidict_article_array *res = malloc(sizeof(unidict_article_array));
    if (!res) {
        cmdx_data_list_free(list);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->count = list->count;
    res->items = calloc(list->count, sizeof(unidict_article));
    if (!res->items) {
        free(res);
        cmdx_data_list_free(list);
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    cmdx_encoding enc = cmdx_reader_get_meta(mdict->mdx_reader)->encoding;
    for (size_t i = 0; i < list->count; i++) {
        res->items[i].title = NULL;
        if (list->items[i] && list->items[i]->data) {
            res->items[i].body = cmdx_data_to_utf8(list->items[i]->data, list->items[i]->length, enc);
        }
    }

    cmdx_data_list_free(list);
    *out_articles = res;
    return UNIDICT_OK;
}

static unidict_status mdict_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict) {
        *out_info = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    const cmdx_meta *meta = cmdx_reader_get_meta(mdict->mdx_reader);
    if (!meta) {
        *out_info = NULL;
        return UNIDICT_OK;
    }

    unidict_info *res = calloc(1, sizeof(unidict_info));
    if (!res) {
        *out_info = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->format = dict->format;
    res->title = meta->title ? strdup(meta->title) : NULL;
    res->description = meta->description ? strdup(meta->description) : NULL;
    res->author = NULL;
    res->creation_date = meta->creation_date ? strdup(meta->creation_date) : NULL;
    res->source_lang = NULL;
    res->target_lang = NULL;
    res->word_count = (uint64_t)cmdx_reader_get_key_count(mdict->mdx_reader);

    *out_info = res;
    return UNIDICT_OK;
}

static unidict_status mdict_suggest(unidict *dict, const char *prefix, size_t limit,
                                    unidict_entry_array **out_entries) {
    if (!dict || !prefix) {
        *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    size_t count = limit > 0 ? limit : 100;
    cmdx_key_entry_list *key_list = cmdx_get_key_entries_by_key(mdict->mdx_reader, (char *)prefix, count, true);

    if (!key_list || key_list->count == 0) {
        if (key_list) cmdx_key_entry_list_free(key_list);
        *out_entries = NULL;
        return UNIDICT_OK;
    }

    unidict_entry_array *res = malloc(sizeof(unidict_entry_array));
    if (!res) {
        cmdx_key_entry_list_free(key_list);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    res->count = key_list->count;
    res->items = calloc(key_list->count, sizeof(unidict_entry *));
    if (!res->items) {
        free(res);
        cmdx_key_entry_list_free(key_list);
        *out_entries = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    for (size_t i = 0; i < key_list->count; i++) {
        cmdx_key_entry *entry = key_list->items[i];
        if (!entry) continue;

        ud_cmdx_key_entry *wrapper = ud_cmdx_key_entry_create(entry);
        if (!wrapper) continue;

        unidict_entry *entry_info = calloc(1, sizeof(unidict_entry));
        if (!entry_info) {
            uobject_release(&wrapper->obj);
            continue;
        }

        entry_info->key = strdup(cmdx_key_entry_get_key(entry));
        entry_info->internal_entry = &wrapper->obj;

        res->items[i] = entry_info;
    }

    cmdx_key_entry_list_free(key_list);

    *out_entries = res;
    return UNIDICT_OK;
}

static unidict_status mdict_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry) {
        *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    cmdx_data *record = NULL;

    if (mdict->udx_dict) {
        // UDX mode: use offset + size
        ud_mdict_article_entry *ref = uobject_cast(entry->internal_entry, ud_mdict_article_entry, obj);
        if (ref->content_size > 0) {
            record = cmdx_get_content_by_offset(mdict->mdx_reader, ref->content_offset, ref->content_size);
        }
    } else {
        // Builtin mode: use cmdx key entry
        ud_cmdx_key_entry *wrapper = uobject_cast(entry->internal_entry, ud_cmdx_key_entry, obj);
        record = cmdx_get_content_record_by_key_entry(mdict->mdx_reader, wrapper->entry);
    }
    if (!record || !record->data) {
        *out_articles = NULL;
        return UNIDICT_OK;
    }

    cmdx_encoding enc = cmdx_reader_get_meta(mdict->mdx_reader)->encoding;
    char *body = cmdx_data_to_utf8(record->data, record->length, enc);
    cmdx_data_free_shallow(record);

    if (!body) {
        *out_articles = NULL;
        return UNIDICT_ERR_NOMEM;
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
// File List
// ============================================================

static unidict_status mdict_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict) {
        *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);
    if (!mdict->mdx_path) {
        *out_infos = NULL;
        return UNIDICT_OK;
    }

    ud_file_list *mdd_list = ud_get_mdd_paths_for_mdx(mdict->mdx_path);
    int mdd_count = mdd_list ? (int)mdd_list->count : 0;
    int total = 1 + mdd_count;

    const char **paths = malloc(total * sizeof(const char *));
    if (!paths) {
        if (mdd_list) ud_file_list_free(mdd_list);
        *out_infos = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    paths[0] = mdict->mdx_path;
    for (int i = 0; i < mdd_count; i++) {
        paths[1 + i] = mdd_list->paths[i];
    }

    unidict_file_info_array *res = unidict_file_infos_from_paths(paths, total);

    free(paths);
    if (mdd_list) ud_file_list_free(mdd_list);

    *out_infos = res;
    return UNIDICT_OK;
}

// ============================================================
// Index activate
// ============================================================

static unidict_status mdict_index_activate(unidict *dict, unidict_index_type index_type) {
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    if (mdict->udx_dict) {
        unidict_close(mdict->udx_dict);
        mdict->udx_dict = NULL;
    }
    dict->active_index = UNIDICT_INDEX_BUILTIN;

    if (index_type == UNIDICT_INDEX_EXTERNAL || index_type == UNIDICT_INDEX_NONE) {
        char *udx_path = mdict_get_udx_path(mdict->mdx_path);
        if (udx_path) {
            unidict *udx_dict = ud_udx_open(udx_path, NULL);
            free(udx_path);
            if (udx_dict) {
                mdict->udx_dict = udx_dict;
                dict->active_index = UNIDICT_INDEX_EXTERNAL;
                return UNIDICT_OK;
            }
        }
        if (index_type == UNIDICT_INDEX_EXTERNAL) return UNIDICT_ERR_IO;
    }

    return UNIDICT_OK;
}

// ============================================================
// Index external make
// ============================================================

static unidict_status mdict_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                void *user_data) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    // Close existing UDX before overwriting
    if (mdict->udx_dict) {
        unidict_close(mdict->udx_dict);
        mdict->udx_dict = NULL;
        dict->active_index = UNIDICT_INDEX_NONE;
    }

    char *udx_path = mdict_get_udx_path(mdict->mdx_path);
    if (!udx_path) return UNIDICT_ERR_INTERNAL;

    udx_writer *writer = udx_writer_open(udx_path);
    if (!writer) {
        free(udx_path);
        return UNIDICT_ERR_IO;
    }

    unidict_status ret = UNIDICT_ERR_INTERNAL;

    int entry_count = 0;
    int last_pct = 0;
    uint64_t total_keys = cmdx_reader_get_key_count(mdict->mdx_reader);

    // Build article db from MDX
    udx_db_builder *art_builder = udx_db_builder_create(writer, "article");
    if (!art_builder) {
        udx_writer_close(writer);
        goto fail;
    }

    cmdx_entry_iter *cmdx_iter = cmdx_reader_iter_create(mdict->mdx_reader);
    if (!cmdx_iter) {
        udx_db_builder_finalize(art_builder);
        udx_writer_close(writer);
        goto fail;
    }

    while (cmdx_iter_next(cmdx_iter)) {
        cmdx_key_entry *ke = cmdx_iter_current(cmdx_iter);
        if (!ke || !ke->key || ke->key[0] == '\0') continue;

        uint64_t content_size = ke->next ? (ke->next->content_logical_offset - ke->content_logical_offset)
                                         : 0; // last entry: size unknown, will be handled in fetch

        uint8_t ref[16];
        mdict_pack_article_ref(ke->content_logical_offset, content_size, ref);
        udx_db_builder_add_entry(art_builder, ke->key, ref, 16);
        entry_count++;

        if (callback && total_keys > 0 && (entry_count % 500) == 0) {
            int pct = (int)((uint64_t)entry_count * 50 / total_keys);
            if (pct > 50) pct = 50;
            if (pct > last_pct) {
                last_pct = pct;
                if (!callback(dict, UNIDICT_INDEX_STAGE_ARTICLES, pct, user_data)) {
                    cmdx_iter_free(cmdx_iter);
                    udx_db_builder_finalize(art_builder);
                    udx_writer_close(writer);
                    ret = UNIDICT_ERR_CANCELLED;
                    goto fail;
                }
            }
        }
    }
    cmdx_iter_free(cmdx_iter);

    udx_status err = udx_db_builder_finalize(art_builder);
    if (err != UDX_OK) {
        udx_writer_close(writer);
        ret = UNIDICT_ERR_IO;
        goto fail;
    }

    // Build resource db from all MDDs
    if (mdict->mdd_reader_count > 0) {
        udx_db_builder *res_builder = udx_db_builder_create(writer, "resource");
        if (!res_builder) {
            udx_writer_close(writer);
            goto fail;
        }

        int res_count = 0;
        uint64_t res_total = 0;
        for (int i = 0; i < mdict->mdd_reader_count; i++) {
            res_total += cmdx_reader_get_key_count(mdict->mdd_readers[i]);
        }

        for (int i = 0; i < mdict->mdd_reader_count; i++) {
            cmdx_entry_iter *mdd_iter = cmdx_reader_iter_create(mdict->mdd_readers[i]);
            if (!mdd_iter) continue;

            while (cmdx_iter_next(mdd_iter)) {
                cmdx_key_entry *ke = cmdx_iter_current(mdd_iter);
                if (!ke || !ke->key || ke->key[0] == '\0') continue;

                uint64_t content_size = ke->next ? (ke->next->content_logical_offset - ke->content_logical_offset) : 0;

                uint8_t ref[20];
                mdict_pack_resource_ref(i, ke->content_logical_offset, content_size, ref);
                udx_db_builder_add_entry(res_builder, ke->key, ref, 20);
                res_count++;

                if (callback && res_total > 0 && (res_count % 500) == 0) {
                    int pct = 50 + (int)((uint64_t)res_count * 50 / res_total);
                    if (pct > 100) pct = 100;
                    if (pct > last_pct) {
                        last_pct = pct;
                        if (!callback(dict, UNIDICT_INDEX_STAGE_RESOURCES, pct, user_data)) {
                            cmdx_iter_free(mdd_iter);
                            udx_db_builder_finalize(res_builder);
                            udx_writer_close(writer);
                            ret = UNIDICT_ERR_CANCELLED;
                            goto fail;
                        }
                    }
                }
            }
            cmdx_iter_free(mdd_iter);
        }

        err = udx_db_builder_finalize(res_builder);
        if (err != UDX_OK) {
            udx_writer_close(writer);
            ret = UNIDICT_ERR_IO;
            goto fail;
        }
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

static unidict_status mdict_index_external_delete(unidict *dict) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    if (mdict->udx_dict) {
        unidict_close(mdict->udx_dict);
        mdict->udx_dict = NULL;
    }
    dict->active_index = UNIDICT_INDEX_BUILTIN;

    char *udx_path = mdict_get_udx_path(mdict->mdx_path);
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
// Entry iterator
// ============================================================

typedef struct {
    unidict_entry_iter base;
    cmdx_entry_iter *cmdx_iter;
    unidict_entry_iter *udx_iter;
} ud_mdict_entry_iter;

static unidict_entry_iter *mdict_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    ud_mdict_entry_iter *iter = calloc(1, sizeof(ud_mdict_entry_iter));
    if (!iter) return NULL;
    iter->base.dict = dict;

    // UDX mode: use UDX iterator
    if (mdict->udx_dict) {
        if (!mdict->udx_dict->ops->entry_iter_create) {
            free(iter);
            return NULL;
        }
        iter->udx_iter = mdict->udx_dict->ops->entry_iter_create(mdict->udx_dict);
        if (!iter->udx_iter) {
            free(iter);
            return NULL;
        }
        return (unidict_entry_iter *)iter;
    }

    iter->cmdx_iter = cmdx_reader_iter_create(mdict->mdx_reader);
    if (!iter->cmdx_iter) {
        free(iter);
        return NULL;
    }
    return (unidict_entry_iter *)iter;
}

static unidict_status mdict_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict) {
        if (out_entry) *out_entry = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_mdict_entry_iter *mdict_iter = (ud_mdict_entry_iter *)iter;
    ud_mdict *mdict = uobject_cast(&iter->dict->obj, ud_mdict, base.obj);

    free(iter->current.key);
    iter->current.key = NULL;
    if (iter->current.internal_entry) {
        uobject_release(iter->current.internal_entry);
        iter->current.internal_entry = NULL;
    }

    if (mdict->udx_dict) {
        if (!mdict_iter->udx_iter) {
            *out_entry = NULL;
            return UNIDICT_DONE;
        }

        unidict_entry *udx_entry = NULL;
        if (!mdict->udx_dict->ops->entry_iter_next) {
            *out_entry = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_status st = mdict->udx_dict->ops->entry_iter_next(mdict_iter->udx_iter, &udx_entry);
        if (st != UNIDICT_OK || !udx_entry) {
            *out_entry = NULL;
            return UNIDICT_DONE;
        }

        udx_db_value_entry *ve = ud_udx_raw_fetch(mdict->udx_dict, udx_entry);
        if (!ve || ve->items.count == 0 || !ve->items.elements[0].data) {
            if (ve) udx_db_value_entry_free(ve);
            *out_entry = NULL;
            return UNIDICT_DONE;
        }

        ud_mdict_article_entry *ref = mdict_article_entry_from_udx_value(&ve->items.elements[0]);
        udx_db_value_entry_free(ve);
        if (!ref) {
            *out_entry = NULL;
            return UNIDICT_DONE;
        }

        iter->current.key = strdup(udx_entry->key);
        iter->current.internal_entry = &ref->obj;
        *out_entry = &iter->current;
        return UNIDICT_OK;
    }

    // Builtin mode: delegate to cmdx
    if (!mdict_iter->cmdx_iter) {
        *out_entry = NULL;
        return UNIDICT_DONE;
    }

    if (!cmdx_iter_next(mdict_iter->cmdx_iter)) {
        *out_entry = NULL;
        return UNIDICT_DONE;
    }

    cmdx_key_entry *ke = cmdx_iter_current(mdict_iter->cmdx_iter);
    if (!ke) {
        *out_entry = NULL;
        return UNIDICT_DONE;
    }

    ud_cmdx_key_entry *wrapper = ud_cmdx_key_entry_create(ke);
    if (!wrapper) {
        *out_entry = NULL;
        return UNIDICT_ERR_NOMEM;
    }

    iter->current.key = strdup(cmdx_key_entry_get_key(ke));
    iter->current.internal_entry = &wrapper->obj;
    *out_entry = &iter->current;
    return UNIDICT_OK;
}

static void mdict_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;
    ud_mdict_entry_iter *mdict_iter = (ud_mdict_entry_iter *)iter;
    if (mdict_iter->cmdx_iter) cmdx_iter_free(mdict_iter->cmdx_iter);
    if (mdict_iter->udx_iter) {
        ud_mdict *mdict = uobject_cast(&iter->dict->obj, ud_mdict, base.obj);
        if (mdict->udx_dict && mdict->udx_dict->ops->entry_iter_free)
            mdict->udx_dict->ops->entry_iter_free(mdict_iter->udx_iter);
    }
    free(iter->current.key);
    if (iter->current.internal_entry) uobject_release(iter->current.internal_entry);
    free(iter);
}

// ============================================================
// Resource get
// ============================================================

static unidict_status mdict_resource_get(unidict *dict, const char *key, unidict_resource **out_res) {
    if (!dict || !key || !out_res) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_res = NULL;
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    // UDX mode: lookup in resource db
    if (mdict->udx_dict) {
        udx_db_value_entry *ve = ud_udx_raw_resource_get(mdict->udx_dict, key);
        if (ve && ve->items.count > 0) {
            int mdd_idx = 0;
            uint64_t offset = 0;
            uint64_t size = 0;
            mdict_unpack_resource_ref(ve->items.elements[0].data, ve->items.elements[0].size, &mdd_idx, &offset, &size);
            udx_db_value_entry_free(ve);

            if (mdd_idx < 0 || mdd_idx >= mdict->mdd_reader_count || size == 0) return UNIDICT_OK;

            cmdx_data *record = cmdx_get_content_by_offset(mdict->mdd_readers[mdd_idx], offset, size);
            if (record && record->data) {
                unidict_resource *res = calloc(1, sizeof(unidict_resource));
                if (!res) {
                    cmdx_data_free_shallow(record);
                    return UNIDICT_ERR_NOMEM;
                }
                res->key = strdup(key);
                res->data = record->data;
                res->size = record->length;
                record->data = NULL;
                cmdx_data_free_shallow(record);
                *out_res = res;
                return UNIDICT_OK;
            }
            if (record) cmdx_data_free_shallow(record);
            return UNIDICT_OK;
        }
        return UNIDICT_OK;
    }

    // Builtin mode: search each MDD
    for (int i = 0; i < mdict->mdd_reader_count; i++) {
        cmdx_data_list *list = cmdx_get_content_records_by_key(mdict->mdd_readers[i], (char *)key, 1, false);
        if (list && list->count > 0 && list->items[0] && list->items[0]->data) {
            unidict_resource *res = calloc(1, sizeof(unidict_resource));
            if (!res) {
                cmdx_data_list_free(list);
                return UNIDICT_ERR_NOMEM;
            }
            res->key = strdup(key);
            res->data = list->items[0]->data;
            res->size = list->items[0]->length;
            list->items[0]->data = NULL;
            list->items[0]->length = 0;
            cmdx_data_list_free(list);
            *out_res = res;
            return UNIDICT_OK;
        }
        if (list) cmdx_data_list_free(list);
    }

    return UNIDICT_OK;
}

// ============================================================
// Resource iterator
// ============================================================

typedef struct {
    unidict_resource_iter base;
    cmdx_entry_iter **mdd_iters;
    int mdd_iter_count;
    int current_mdd;
    ud_udx_raw_res_iter *udx_iter;
} ud_mdict_resource_iter;

static unidict_resource_iter *mdict_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode) {
    if (!dict) return NULL;
    ud_mdict *mdict = uobject_cast(&dict->obj, ud_mdict, base.obj);

    ud_mdict_resource_iter *iter = calloc(1, sizeof(ud_mdict_resource_iter));
    if (!iter) return NULL;
    iter->base.dict = dict;
    (void)mode;

    if (mdict->udx_dict) {
        iter->udx_iter = ud_udx_raw_res_iter_create(mdict->udx_dict);
        if (!iter->udx_iter) {
            free(iter);
            return NULL;
        }
        return (unidict_resource_iter *)iter;
    }

    // Builtin: create iterators for all MDDs
    if (mdict->mdd_reader_count > 0) {
        iter->mdd_iters = calloc(mdict->mdd_reader_count, sizeof(cmdx_entry_iter *));
        if (!iter->mdd_iters) {
            free(iter);
            return NULL;
        }
        iter->mdd_iter_count = mdict->mdd_reader_count;

        for (int i = 0; i < mdict->mdd_reader_count; i++) {
            iter->mdd_iters[i] = cmdx_reader_iter_create(mdict->mdd_readers[i]);
            if (!iter->mdd_iters[i]) {
                for (int j = 0; j < i; j++) cmdx_iter_free(iter->mdd_iters[j]);
                free(iter->mdd_iters);
                free(iter);
                return NULL;
            }
        }
    }

    return (unidict_resource_iter *)iter;
}

static unidict_status mdict_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res) {
    if (!iter || !iter->dict) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_mdict_resource_iter *res_iter = (ud_mdict_resource_iter *)iter;
    ud_mdict *mdict = uobject_cast(&iter->dict->obj, ud_mdict, base.obj);

    free(iter->current.key);
    iter->current.key = NULL;
    if (iter->current.data) {
        free(iter->current.data);
        iter->current.data = NULL;
    }
    iter->current.size = 0;
    free(iter->current.mime_type);
    iter->current.mime_type = NULL;

    // UDX mode
    if (res_iter->udx_iter) {
        udx_db_value_entry *ve = ud_udx_raw_res_iter_next(res_iter->udx_iter);
        if (!ve || ve->items.count == 0 || !ve->items.elements[0].data) {
            if (ve) udx_db_value_entry_free(ve);
            *out_res = NULL;
            return UNIDICT_DONE;
        }

        int mdd_idx = 0;
        uint64_t offset = 0, size = 0;
        mdict_unpack_resource_ref(ve->items.elements[0].data, ve->items.elements[0].size, &mdd_idx, &offset, &size);

        const char *res_key = ve->items.elements[0].original_key ? ve->items.elements[0].original_key : "";
        udx_db_value_entry_free(ve);

        if (mdd_idx < 0 || mdd_idx >= mdict->mdd_reader_count || size == 0) {
            // Key-only mode: just return the key
            iter->current.key = strdup(res_key);
            *out_res = &iter->current;
            return UNIDICT_OK;
        }

        cmdx_data *record = cmdx_get_content_by_offset(mdict->mdd_readers[mdd_idx], offset, size);
        if (record && record->data) {
            iter->current.key = strdup(res_key);
            iter->current.data = record->data;
            iter->current.size = record->length;
            record->data = NULL;
            cmdx_data_free_shallow(record);
            *out_res = &iter->current;
            return UNIDICT_OK;
        }
        if (record) cmdx_data_free_shallow(record);

        iter->current.key = strdup(res_key);
        *out_res = &iter->current;
        return UNIDICT_OK;
    }

    // Builtin mode: iterate MDDs sequentially
    while (res_iter->current_mdd < res_iter->mdd_iter_count) {
        cmdx_entry_iter *mdd_iter = res_iter->mdd_iters[res_iter->current_mdd];
        if (cmdx_iter_next(mdd_iter)) {
            cmdx_key_entry *ke = cmdx_iter_current(mdd_iter);
            if (ke && ke->key) {
                iter->current.key = strdup(ke->key);
                *out_res = &iter->current;
                return UNIDICT_OK;
            }
        }
        res_iter->current_mdd++;
    }

    *out_res = NULL;
    return UNIDICT_DONE;
}

static void mdict_resource_iter_free(unidict_resource_iter *iter) {
    if (!iter) return;
    ud_mdict_resource_iter *res_iter = (ud_mdict_resource_iter *)iter;
    if (res_iter->udx_iter) ud_udx_raw_res_iter_free(res_iter->udx_iter);
    if (res_iter->mdd_iters) {
        for (int i = 0; i < res_iter->mdd_iter_count; i++) {
            if (res_iter->mdd_iters[i]) cmdx_iter_free(res_iter->mdd_iters[i]);
        }
        free(res_iter->mdd_iters);
    }
    free(iter->current.key);
    if (iter->current.data) free(iter->current.data);
    free(iter->current.mime_type);
    free(iter);
}

// ============================================================
// Constructor
// ============================================================

unidict *ud_mdict_open(const char *mdx_path, const unidict_open_options *options) {
    if (!mdx_path) return NULL;
    if (!check_file_extension(mdx_path, "mdx")) return NULL;

    const char *device_id = (options && options->mdict_device_id) ? options->mdict_device_id : NULL;

    // Discover companion MDD files
    ud_file_list *mdd_list = ud_get_mdd_paths_for_mdx(mdx_path);
    int mdd_count = mdd_list ? (int)mdd_list->count : 0;

    ud_mdict *mdict = calloc(1, sizeof(ud_mdict));
    if (!mdict) {
        ud_file_list_free(mdd_list);
        return NULL;
    }

    uobject_init(&mdict->base.obj, &ud_mdict_type, NULL);
    mdict->base.ops = &mdict_ops;
    mdict->base.format = UNIDICT_FORMAT_MDICT;
    mdict->base.has_builtin_index = true;

    mdict->mdx_path = strdup(mdx_path);
    if (!mdict->mdx_path) {
        ud_file_list_free(mdd_list);
        free(mdict);
        return NULL;
    }

    // Open MDX reader
    mdict->mdx_reader = cmdx_reader_open(mdict->mdx_path, device_id);
    if (!mdict->mdx_reader) {
        ud_file_list_free(mdd_list);
        free(mdict->mdx_path);
        free(mdict);
        return NULL;
    }

    // Open MDD readers
    if (mdd_count > 0) {
        mdict->mdd_readers = malloc(mdd_count * sizeof(cmdx_reader *));
        if (!mdict->mdd_readers) {
            ud_file_list_free(mdd_list);
            cmdx_reader_close(mdict->mdx_reader);
            free(mdict->mdx_path);
            free(mdict);
            return NULL;
        }

        for (int i = 0; i < mdd_count; i++) {
            cmdx_reader *r = cmdx_reader_open(mdd_list->paths[i], device_id);
            if (!r) {
                for (int j = 0; j < mdict->mdd_reader_count; j++) {
                    cmdx_reader_close(mdict->mdd_readers[j]);
                }
                free(mdict->mdd_readers);
                ud_file_list_free(mdd_list);
                cmdx_reader_close(mdict->mdx_reader);
                free(mdict->mdx_path);
                free(mdict);
                return NULL;
            }
            mdict->mdd_readers[mdict->mdd_reader_count++] = r;
        }
    }

    ud_file_list_free(mdd_list);
    return &mdict->base;
}
