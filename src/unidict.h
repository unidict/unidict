//
//  unidict.h
//  unidict
//
//  Created by kejinlu on 2025-11-25
//
#ifndef unidict_h
#define unidict_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Thread safety
// ============================================================
//
// This library follows the per-handle model (similar to libcurl):
// - Each unidict handle is independent and may be used by one thread
//   at a time; no internal locking is performed.
// - You can safely use different unidict handles from different threads
//   simultaneously.
// - To prepare a dictionary on a background thread, call unidict_prepare()
//   from that thread while the main thread does not access the same handle.

// ============================================================
// Status codes
// ============================================================

typedef enum {
    UNIDICT_OK = 0,
    UNIDICT_DONE,              // iteration complete (no more items)
    UNIDICT_ERR_NOT_FOUND,     // key not found in dictionary
    UNIDICT_ERR_NO_INDEX,      // index not available (build with index_external_make first)
    UNIDICT_ERR_IO,            // file read/write error
    UNIDICT_ERR_NOMEM,         // out of memory
    UNIDICT_ERR_INVALID_PARAM, // NULL or invalid parameter
    UNIDICT_ERR_NOT_SUPPORTED, // operation not supported by this format
    UNIDICT_ERR_CORRUPT,       // dictionary data is corrupt
    UNIDICT_ERR_CANCELLED,     // operation cancelled by callback
    UNIDICT_ERR_INTERNAL,      // internal error
} unidict_status;

const char *unidict_strerror(unidict_status status);

// ============================================================
// Format identifiers
// ============================================================

typedef enum {
    UNIDICT_FORMAT_UNKNOWN,
    UNIDICT_FORMAT_BABYLON,
    UNIDICT_FORMAT_DICTD,
    UNIDICT_FORMAT_EPWING,
    UNIDICT_FORMAT_LINGOES,
    UNIDICT_FORMAT_LINGVO,
    UNIDICT_FORMAT_MDICT,
    UNIDICT_FORMAT_STARDICT,
    UNIDICT_FORMAT_UDX,
    UNIDICT_FORMAT_ZIM,
    UNIDICT_FORMAT_DSL,
} unidict_format;

const char *unidict_format_name(unidict_format format);

// ============================================================
// Types
// ============================================================

typedef struct unidict unidict;
typedef struct uobject uobject;
typedef struct unidict_entry_iter unidict_entry_iter;
typedef struct unidict_resource_iter unidict_resource_iter;

typedef struct {
    unidict_format format; // dictionary format, read-only
    char *title;
    char *subtitle;     // localized title (e.g., per-language display name), NULL if unavailable
    char *description;
    char *author;
    char *email;
    char *creation_date;
    char *source_lang;   // source language code (e.g., "en")
    char *target_lang;   // target language code (e.g., "zh")
    uint64_t word_count; // total number of index entries (may exceed unique keys if duplicates exist)
    char *format_version; // dictionary format version (e.g., "14.0"), NULL if unavailable
    char *edition;              // dictionary content edition (e.g., "3"), NULL if unavailable

    // Number of sub-items (e.g., EPWING subbooks). 0 for single-dictionary formats.
    int subitem_count;

    // Dictionary icon. NULL if unavailable. Owned by info struct, freed by unidict_info_free.
    uint8_t *icon_data;
    size_t icon_size;
    char *icon_mime_type;
} unidict_info;

// Release an info struct. Must be called when the info is no longer needed.
void unidict_info_free(unidict_info *info);

// Serialize info to XML string. Caller must free the returned string.
// Returns NULL on failure. Only serializes string fields (title, description,
// author, email, creation_date, source_lang, target_lang).
char *unidict_info_to_xml(const unidict_info *info);

// Deserialize info from XML string. Caller must free with unidict_info_free.
// Returns NULL on failure. format, word_count, format_version, subitem_count are not restored.
unidict_info *unidict_info_from_xml(const char *xml);

typedef struct {
    char *path;            // file absolute path
    uint64_t size;         // file size in bytes
    int64_t last_modified; // last modified time (Unix timestamp)
} unidict_file_info;

typedef struct {
    unidict_file_info *items;
    size_t count;
} unidict_file_info_array;

// Release a file info array. Must be called when the array is no longer needed.
void unidict_file_info_array_free(unidict_file_info_array *array);

// A lightweight reference to a dictionary entry.
// Returned by unidict_suggest(), unidict_entry_iter_next(), etc.
// Use unidict_fetch() to retrieve the full article content.
// Ownership: belongs to the parent unidict_entry_array; freed when the array is freed.
typedef struct {
    char *key;               // display headword
    uobject *internal_entry; // opaque backend handle, do not modify
} unidict_entry;

typedef struct {
    unidict_entry **items;
    size_t count;
} unidict_entry_array;

// Release an entry array and all entries it contains.
// Must be called when the array is no longer needed.
// All unidict_entry pointers from this array become invalid after this call.
void unidict_entry_array_free(unidict_entry_array *array);

typedef struct {
    char *title; // original headword (may differ from lookup key), may be NULL
    char *body;  // article body / definition content
} unidict_article;

typedef struct {
    unidict_article *items;
    size_t count;
} unidict_article_array;

// Release an article array and all articles it contains.
// Must be called when the array is no longer needed.
void unidict_article_array_free(unidict_article_array *array);

typedef struct {
    char *key;       // resource key
    uint8_t *data;   // resource data
    size_t size;     // resource data size in bytes
    char *mime_type; // MIME type, may be NULL
} unidict_resource;

// Release a resource. Must be called when the resource is no longer needed.
void unidict_resource_free(unidict_resource *res);

// Index types (used by open options and index API)
typedef enum {
    UNIDICT_INDEX_NONE = 0,

    // for formats without a builtin index (DSL, Babylon),
    // BUILTIN means "raw mode without external index"
    UNIDICT_INDEX_BUILTIN = 1,

    UNIDICT_INDEX_EXTERNAL = 2,
} unidict_index_type;

// Gaiji (custom character) rendering mode for EPWING dictionaries.
// Controls how gaiji characters are rendered in article text output.
typedef enum {
    UNIDICT_EPWING_GAIJI_FALLBACK = 0,  // output nothing
    UNIDICT_EPWING_GAIJI_ASCII_ART = 1, // # and . bitmap pattern
    UNIDICT_EPWING_GAIJI_BITMAP = 2,    // <img src="unidict://epwing/gaiji/..."> (default)
} unidict_epwing_gaiji_mode;

// ============================================================
// Lifecycle
// ============================================================

typedef struct {
    // client identifier for encrypted MDict dictionaries, may be NULL
    const char *mdict_device_id;

    // index type to activate at open time (NONE = auto-detect: external > builtin)
    unidict_index_type index_type;

    // which sub-item to open (0 = first, default). Only meaningful for
    // collection formats like EPWING; ignored by single-dictionary formats.
    int subitem_index;

    // preferred language for Lingoes metadata (e.g., "zh-CN"). Lingoes dictionaries
    // may contain multi-language metadata items; this selects the preferred one.
    // NULL uses the default (first available). Ignored by other formats.
    const char *lingoes_pref_lang;

    // gaiji (custom character) rendering mode for EPWING dictionaries.
    // Default is UNIDICT_GAIJI_BITMAP. Ignored by other formats.
    unidict_epwing_gaiji_mode epwing_gaiji_mode;
} unidict_open_options;

// Open a dictionary file. Format is auto-detected from the file extension.
// On success, *out_dict is set and UNIDICT_OK is returned.
// On failure, *out_dict is set to NULL and an error code is returned.
// Call unidict_close() when the handle is no longer needed.
unidict_status unidict_open(const char *file_path, const unidict_open_options *options, unidict **out_dict);

// Close a dictionary handle and free associated resources.
void unidict_close(unidict *dict);

// Preload dictionary data. Optional — data is loaded lazily if skipped.
// Idempotent: calling multiple times is safe and returns UNIDICT_OK.
// May be slow; consider calling from a background thread.
unidict_status unidict_prepare(unidict *dict);

// ============================================================
// Info
// ============================================================

// Get dictionary metadata. Caller must free with unidict_info_free().
unidict_status unidict_info_get(unidict *dict, unidict_info **out_info);

// Get the list of files that make up this dictionary.
// Caller must free with unidict_file_info_array_free().
unidict_status unidict_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);

// ============================================================
// Index
// ============================================================
//
//   active_index  — currently active index, updated by index_activate()
//
//   has_builtin_index / has_external_index — capability flags:
//       - detected at open time
//       - updated by index_external_make() and index_external_delete()

unidict_index_type unidict_index_get_active(unidict *dict);

// Activate the specified index type. Usually not needed — open with options.index_type
// is sufficient. Use this to switch or reload index after open.
//
// Behavior by index_type:
//   - EXTERNAL: load the external index (e.g. UDX); fails if not available
//   - BUILTIN:  load from the source dictionary file; fails if not available
//   - NONE:     prefer external if available, else builtin, else fallback (format-specific)
//
// To reload the current index, call:
//   unidict_index_activate(dict, unidict_index_get_active(dict));
//
// Common scenarios:
//   - No builtin index:  index_external_make → unidict_index_activate(dict, EXTERNAL)
//   - Switch index:      unidict_index_activate(dict, BUILTIN) or unidict_index_activate(dict, EXTERNAL)
unidict_status unidict_index_activate(unidict *dict, unidict_index_type index_type);

bool unidict_index_has_builtin(unidict *dict);
bool unidict_index_has_external(unidict *dict);

typedef enum {
    UNIDICT_INDEX_STAGE_ARTICLES,
    UNIDICT_INDEX_STAGE_RESOURCES,
} unidict_index_stage;

// Returns true to continue, false to cancel.
// When cancelled, unidict_index_external_make returns UNIDICT_ERR_CANCELLED.
// Called on the same thread as unidict_index_external_make.
// Only called for slow stages with meaningful progress; fast stages are skipped.
//
// To cancel from another thread, use an atomic flag in user_data:
//
//   typedef struct { atomic_bool cancel; } build_ctx;
//
//   bool my_cb(unidict *d, unidict_index_stage s, int pct, void *ud) {
//       return !atomic_load(&((build_ctx *)ud)->cancel);
//   }
//
//   // From another thread:
//   atomic_store(&ctx.cancel, true);
typedef bool (*unidict_index_external_make_cb)(unidict *dict, unidict_index_stage stage, int percent, void *user_data);

// Build an external index for faster lookups. Progress is reported via callback.
// After success, call unidict_index_activate(dict, EXTERNAL) to load the new index.
unidict_status unidict_index_external_make(unidict *dict, unidict_index_external_make_cb callback, void *user_data);
// Delete the external index files and reset index state.
unidict_status unidict_index_external_delete(unidict *dict);

// ============================================================
// Article
// ============================================================
//
// Two-phase lookup model:
//   Get lightweight entry references via suggest() / entry_lookup(),
//   then fetch full article content via fetch() by passing the entry.
// Direct key→article is available via lookup().
//
// Entry references (unidict_entry) carry an opaque backend handle so
// fetch() can short-circuit to the data without re-scanning the index.
//
// IMPORTANT: entries from suggest()/entry_lookup() are owned by the
// returned unidict_entry_array. You must call fetch() before freeing
// the array, as all entry pointers become invalid after the array is freed.

// Combined lookup: key directly → articles. Caller must free with unidict_article_array_free().
unidict_status unidict_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);

// Exact key → entry references. Caller must free with unidict_entry_array_free().
unidict_status unidict_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);

// Prefix match → entry suggestions. Caller must free with unidict_entry_array_free().
unidict_status unidict_suggest(unidict *dict, const char *prefix, int limit, unidict_entry_array **out_entries);

// Fetch full article content from an entry reference.
// The entry must come from a still-valid unidict_entry_array (not yet freed).
// Caller must free with unidict_article_array_free().
unidict_status unidict_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);

// Iterate all entries in the dictionary.
// Caller must free the iterator with unidict_entry_iter_free().
// Each returned entry is valid until the next call to entry_iter_next() or iterator free.
// Returns UNIDICT_OK when an entry is available, UNIDICT_DONE when no more entries.
unidict_status unidict_entry_iter_create(unidict *dict, unidict_entry_iter **out_iter);
unidict_status unidict_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
void unidict_entry_iter_free(unidict_entry_iter *iter);

// ============================================================
// Resource
// ============================================================
//
// Resources are key → binary data (e.g., images, CSS, audio) with no
// suggest/preview and no two-phase lookup. Direct get by key, plus a
// key-only iteration mode for listing without loading data upfront.

// Get a resource by key. Caller must free with unidict_resource_free().
unidict_status unidict_resource_get(unidict *dict, const char *key, unidict_resource **out_res);

typedef enum {
    UNIDICT_RESOURCE_ITER_KEY = 0,  // only fill key (lightweight)
    UNIDICT_RESOURCE_ITER_FULL = 1, // fill key, data, size, mime_type
} unidict_resource_iter_mode;

// Iterate all resources in the dictionary.
// Caller must free the iterator with unidict_resource_iter_free().
// Each returned resource is valid until the next call to resource_iter_next() or iterator free.
// Returns UNIDICT_OK when a resource is available, UNIDICT_DONE when no more resources.
unidict_status unidict_resource_iter_create(unidict *dict, unidict_resource_iter_mode mode,
                                            unidict_resource_iter **out_iter);
unidict_status unidict_resource_iter_next(unidict_resource_iter *iter, unidict_resource **out_res);
void unidict_resource_iter_free(unidict_resource_iter *iter);

// ============================================================
// Unidict Pages
// ============================================================
//
// Format-specific special pages (e.g., EPWING menu, copyright).
// feature_pages_list() discovers available pages; feature_page_get()
// fetches their HTML content by key (including sub-page navigation).

typedef struct {
    char *key;    // page key, e.g. "menu", "copyright" — pass directly to feature_page_get
    char *name;   // display name, e.g. "Menu"
} unidict_feature_page;

typedef struct {
    unidict_feature_page *items;
    size_t count;
} unidict_feature_page_array;

// Release a feature page array. Must be called when the array is no longer needed.
void unidict_feature_page_array_free(unidict_feature_page_array *array);

// List available feature pages. Returns empty list for unsupported formats.
// Caller must free with unidict_feature_page_array_free().
unidict_status unidict_feature_pages_list(unidict *dict, unidict_feature_page_array **out_pages);

// Read page HTML content by key. The key comes from feature_pages_list() or is constructed
// by appending query parameters: "menu?page=123&offset=456". Caller must free() the result.
unidict_status unidict_feature_page_read(unidict *dict, const char *key, char **out_html);

#ifdef __cplusplus
}
#endif

#endif /* unidict_h */
