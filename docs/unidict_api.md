# unidict API reference

This document describes how to use the public `unidict` C API declared in
`src/unidict.h`. It covers lifecycle, metadata, the two-phase lookup model,
iteration, resources, and the external index.

## Status codes

All functions return `unidict_status`:

| Code | Meaning |
|------|---------|
| `UNIDICT_OK` | success |
| `UNIDICT_DONE` | iteration complete (no more items) |
| `UNIDICT_ERR_NOT_FOUND` | key not found |
| `UNIDICT_ERR_NO_INDEX` | index not available |
| `UNIDICT_ERR_IO` | file read/write error |
| `UNIDICT_ERR_NOMEM` | out of memory |
| `UNIDICT_ERR_INVALID_PARAM` | NULL or invalid parameter |
| `UNIDICT_ERR_NOT_SUPPORTED` | operation not supported by this format |
| `UNIDICT_ERR_CORRUPT` | corrupt dictionary data |
| `UNIDICT_ERR_CANCELLED` | operation cancelled by callback |
| `UNIDICT_ERR_INTERNAL` | internal error |

`unidict_strerror(status)` returns a human-readable string.

## Lifecycle

```c
unidict *dict = NULL;
unidict_open_options opts = {0};
unidict_status st = unidict_open("dict.mdx", &opts, &dict);
if (st != UNIDICT_OK) { /* handle error */ }

/* ... use dict ... */

unidict_close(dict);   // drops the refcount and frees when it hits zero
```

- `unidict_open(path, options, &dict)` auto-detects the format from the
  path and opens the matching backend. On failure `*dict` is set to NULL.
- `unidict_close(dict)` is the only way to release a handle; it is safe
  to call with NULL.
- `unidict_prepare(dict)` optionally preloads data; idempotent. Lookups
  call it lazily, so it is only needed to front-load work on a background
  thread.

### Open options (`unidict_open_options`)

| Field | Purpose |
|-------|---------|
| `mdict_device_id` | client id for encrypted MDict dictionaries |
| `index_type` | index to activate at open: `NONE` (auto), `BUILTIN`, `EXTERNAL` |
| `subitem_index` | which sub-item to open (EPWING subbooks) |
| `lingoes_pref_lang` | preferred metadata language for Lingoes |
| `epwing_gaiji_mode` | gaiji rendering: `FALLBACK`, `ASCII_ART`, `BITMAP` (default) |

## Metadata

```c
unidict_info *info = NULL;
unidict_info_get(dict, &info);
printf("%s (%s)\n", info->title, unidict_format_name(info->format));
printf("%llu entries\n", (unsigned long long)info->word_count);
unidict_info_free(info);
```

`unidict_info` carries `title`, `subtitle`, `description`, `author`,
`email`, `creation_date`, `source_lang`, `target_lang`, `word_count`,
`format_version`, `edition`, `subitem_count`, and an optional `icon_*`.
Always free it with `unidict_info_free()`. `unidict_info_to_xml()` /
`unidict_info_from_xml()` round-trip the string fields.

`unidict_file_infos_get()` lists the files that make up the dictionary
(freed with `unidict_file_info_array_free()`).

## Two-phase lookup model

unidict separates **entry references** (lightweight) from **article
content** (heavy). This lets you fetch suggestions/preview lists cheaply
and only read full articles on demand.

```c
/* Phase 1: get entry references */
unidict_entry_array *entries = NULL;
unidict_suggest(dict, "hel", 10, &entries);   /* prefix match */
for (size_t i = 0; i < entries->count; i++) {
    printf("- %s\n", entries->items[i]->key);
}

/* Phase 2: fetch an article from an entry reference */
unidict_article_array *arts = NULL;
unidict_fetch(dict, entries->items[0], &arts);
if (arts && arts->count) {
    printf("%s\n", arts->items[0].body);
}
unidict_article_array_free(arts);
unidict_entry_array_free(entries);   /* entries become invalid after this */
```

For a direct key → article shortcut there is `unidict_lookup()`, and
`unidict_entry_lookup()` for exact key → entry references.

**Important:** entry pointers returned by `suggest()` / `entry_lookup()`
are owned by their `unidict_entry_array` and become invalid once that
array is freed. Call `fetch()` before freeing the array.

## Entry iteration

Walk every entry in the dictionary. Each returned entry is valid until
the next `entry_iter_next()` call or until the iterator is freed — the
caller must **not** free the returned pointer.

```c
unidict_entry_iter *iter = NULL;
unidict_entry_iter_create(dict, &iter);
unidict_entry *e = NULL;
while (unidict_entry_iter_next(iter, &e) == UNIDICT_OK) {
    printf("%s\n", e->key);
}
unidict_entry_iter_free(iter);
```

## Resources

Resources are key → binary data (images, CSS, audio) with no two-phase
lookup. Get directly by key, or iterate keys only (lightweight) vs full
blobs.

```c
unidict_resource *res = NULL;
if (unidict_resource_get(dict, "/m/style.css", &res) == UNIDICT_OK) {
    /* res->data, res->size, res->mime_type */
    unidict_resource_free(res);
}
```

## Index management

Some formats (Babylon, DSL, Lingoes) have no builtin index. For prefix
matches and fast iteration on those, build an external UDX index first:

```c
bool progress_cb(unidict *d, unidict_index_stage s, int pct, void *ud) {
    return true;   /* return false to cancel */
}

unidict_index_external_make(dict, progress_cb, NULL);
unidict_index_activate(dict, UNIDICT_INDEX_EXTERNAL);
/* now suggest() / entry_iter use the UDX index */
```

Query the state with `unidict_index_get_active()`,
`unidict_index_has_builtin()`, `unidict_index_has_external()`. Remove an
external index with `unidict_index_external_delete()`.

## Feature pages

Format-specific special pages (e.g. EPWING menu, copyright). List
available pages, then read by key:

```c
unidict_feature_page_array *pages = NULL;
unidict_feature_pages_list(dict, &pages);
for (size_t i = 0; i < pages->count; i++) {
    char *html = NULL;
    unidict_feature_page_read(dict, pages->items[i].key, &html);
    free(html);
}
unidict_feature_page_array_free(pages);
```

## Thread safety

Per-handle model: each handle is independent and usable by one thread at
a time; no internal locking. To prepare a dictionary on a background
thread, call `unidict_prepare()` from that thread while the main thread
does not touch the same handle.
