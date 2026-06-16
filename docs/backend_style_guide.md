# Format backend style guide

This document defines the canonical code organisation for a unidict format
backend (`src/formats/ud_<format>.c` / `.h`). It is derived from
`ud_lingvo.c`, which is the cleanest reference implementation. Every new
backend should follow the same file layout, section order, and naming so
that backends are uniform and easy to navigate.

## Public header — `ud_<format>.h`

The header is minimal. It only exposes the constructor; everything else
is opaque.

```c
#ifndef ud_<format>_h
#define ud_<format>_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

unidict *ud_<format>_open(const char *path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_<format>_h */
```

Rules:
- One declared symbol only: `ud_<format>_open`.
- Include only `unidict.h` — never leak `uobject.h` or the format's
  underlying parser headers into the public interface.
- `extern "C"` guards so the header is usable from C++.
- No struct definitions, no `unidict_ops`, no internal types.

## Implementation — `ud_<format>.c`

### Section order (top to bottom)

The file is organised into fixed sections in this exact order. Each
section is delimited by a banner comment block:

```c
// ============================================================
// <Section title>
// ============================================================
```

1. **Includes** — `ud_<format>.h`, `unidict_internal.h`, the format's
   parser headers (`<fmt>_reader.h` etc.), `udx_writer.h` + `ud_udx.h`
   (if the format builds a UDX index), then system headers.
2. **Private struct definition** — the backend's `ud_<format>` struct,
   embedding `unidict base` as the first member.
3. **Entry reference struct** *(if two-phase lookup is supported)* —
   `ud_<format>_index`, an internal `uobject` subclass that carries the
   data needed by `fetch()` (e.g. an article offset/reference). Includes
   its `release` destructor and `uobject_type`.
4. **Iterator structs** *(if entry/resource iteration is supported)* —
   `ud_<format>_entry_iter` (embedding `unidict_entry_iter base`) and
   `ud_<format>_resource_iter` (embedding `unidict_resource_iter base`),
   declared together.
5. **Virtual function table** — forward declarations of every op, then
   the `static const unidict_ops <format>_ops` definition, then the
   `static const uobject_type ud_<format>_type` (with the `release`
   destructor). The forward declarations and the op assignments in the
   vtable use the **same order** as `unidict_ops` in
   `unidict_internal.h`.
6. **Release destructor** — `ud_<format>_release(uobject *obj)`:
   frees all owned resources (parser handle, UDX sub-dict, paths) in
   reverse-init order, then `free(self)`.
7. **Constructor** — `ud_<format>_open`: validate path/extension,
   `calloc`, `uobject_init`, set `base.ops` / `base.format` /
   `base.has_builtin_index`, `strdup(path)`, open the parser, detect
   the external index, run `index_activate` with the preset, return
   `&self->base`. On any failure, free what was allocated and return
   NULL.
8. **Helpers** — file-local helpers shared by several ops (e.g.
   `<format>_get_udx_path` to derive the `.udx` path,
   `<format>_ref_from_value_item` to unpack a reference). Grouped here
   so they precede their first use.
9. **Info** — `<format>_info_get`.
10. **File list** — `<format>_file_infos_get`.
11. **Index activate** — `<format>_index_activate`: close any open UDX,
    then activate builtin or external per `index_type`.
12. **Index external make** — `<format>_index_external_make`: build the
    UDX (serialize info metadata, iterate entries, write keys/values,
    report progress via the callback, support cancellation).
13. **Index external delete** — `<format>_index_external_delete`:
    switch back to builtin, `remove()` the `.udx` file.
14. **Lookup** — `<format>_lookup`, `<format>_entry_lookup`,
    `<format>_suggest`, `<format>_lookup_by_entry` (the `fetch` op).
15. **Entry iterator** — `<format>_entry_iter_create` / `_next` /
    `_free`.
16. **Resource operations** *(if supported)* — `resource_get`,
    `resource_iter_create` / `_next` / `_free`.

The op implementations (sections 9–16) appear in the **same order as the
slots in `unidict_ops`** (`info_get, file_infos_get, index_activate,
index_external_make, index_external_delete, lookup, entry_lookup,
suggest, fetch, entry_iter_*, resource_get, resource_iter_*`). Reading
the file top to bottom thus mirrors reading the vtable.

Backends that don't support a capability (e.g. no resources, no external
index) simply omit the corresponding section and leave those slots NULL
in the vtable.

### Naming conventions

| Kind | Convention | Example |
|------|-----------|---------|
| Constructor (public, exported) | `ud_<format>_open` | `ud_lingvo_open` |
| Public struct types | `ud_<format>` | `ud_lingvo` |
| Internal ref/iterator struct | `ud_<format>_<role>` | `ud_lingvo_index`, `ud_lingvo_entry_iter` |
| `uobject_type` constants | `ud_<format>_type`, `ud_<format>_<role>_type` | `ud_lingvo_type` |
| Release destructors | `ud_<format>_release`, `ud_<format>_<role>_release` | `ud_lingvo_release` |
| vtable constant | `<format>_ops` | `lingvo_ops` |
| vtable op implementations (static) | `<format>_<op>` — **no `ud_` prefix** | `lingvo_lookup`, `lingvo_info_get` |
| Helper functions (static) | `<format>_<verb>[_<noun>]` | `lingvo_get_udx_path` |

Rationale for the split: the `ud_` prefix marks **public, exported, or
type-level** identifiers (things a caller or `unidict.c`'s dispatch sees).
Static file-local op implementations carry only the short format name,
because they never leave the translation unit — this keeps the vtable
and the call sites readable.

### Structural rules

- **`unidict base` is always the first member** of `ud_<format>` and of
  every iterator struct (`unidict_entry_iter base` /
  `unidict_resource_iter base`). This is what `uobject_cast()` (a
  `container_of`) relies on.
- **Cast with `uobject_cast(&dict->obj, ud_<format>, base.obj)`** at the
  top of every op to recover the private struct from the public handle.
- **Lifecycle is refcounted** via `uobject`. The constructor returns a
  handle with refcount 1; `unidict_close()` drops it. Use
  `uobject_retain` / `uobject_release` for any sub-object the backend
  keeps a reference to (e.g. a parser entry handed to an
  `unidict_entry.internal_entry`).
- **Embedded sub-objects vs allocated sub-objects**: an
  `unidict_entry.internal_entry` that points to a **heap-allocated**
  `uobject` is `uobject_release()`d by `unidict_entry_array_free()`. An
  `internal_entry` that points to a member **embedded inside the
  iterator** (e.g. `iter->current_idx`) must **not** be released — it
  dies with the iter. See the bug fix in `ud_stardict.c` /
  `ud_lingvo.c` `*_entry_iter_free`.
- **Ownership of returned arrays**: `lookup` / `suggest` / `fetch` /
  `info_get` allocate a result struct on success; the caller frees it
  with the matching `*_free()`. On failure they must free any partial
  allocation and set `*out = NULL`.
- **`index_external_make` must be idempotent and cancellable**: it
  reuses the progress callback, honours `false` return as cancel
  (returning `UNIDICT_ERR_CANCELLED`), and on failure removes the
  half-written `.udx`.

## Adding a new format — checklist

1. Create `src/formats/ud_<format>.c` and `ud_<format>.h` from this
   guide (use `ud_lingvo.c` as the closest template).
2. Add a `UNIDICT_FORMAT_<NAME>` value to the `unidict_format` enum in
   `src/unidict.h` and a case in `unidict_format_name()`.
3. Add detection to `detect_dict_format()` and a dispatch case in
   `unidict_open()` (both in `src/unidict.c`).
4. Add the source files to the build (`unidict.xcodeproj` via Xcode's
   synchronized `src/` group, and they are picked up automatically by
   the top-level `CMakeLists.txt` glob).
5. Add a `tests/test_<format>.c` suite and register it in
   `tests/main.c` + `tests/CMakeLists.txt`, using a fixture from the
   format's `deps/<lib>/tests/data/`.
