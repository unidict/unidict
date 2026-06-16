# unidict

A C library providing a single, unified, thread-safe API for reading
dictionary files across many formats — Babylon (BGL), DictD, EPWING,
Lingoes (LD2/LDX), Lingvo (LSD), MDict (MDX/MDD), StarDict, ZIM, and
Lingvo DSL — plus its own fast external index format (UDX).

It follows the per-handle model (similar to libcurl): each `unidict`
handle is independent and may be used by one thread at a time, with no
internal locking. Different handles can be used concurrently from
different threads.

## Supported formats

| Format | Extension | Backend | Notes |
|--------|-----------|---------|-------|
| Babylon | `.bgl` | deps/bgl | No builtin index; builds UDX for lookup |
| DictD | `.index` | deps/stardict | `.index` + `.dict` / `.dict.dz` |
| EPWING | directory (`CATALOGS`) | deps/ebcore | Japanese dictionary standard; gaiji rendering |
| Lingoes | `.ld2` / `.ldx` | deps/ldx | No builtin index; builds UDX |
| Lingvo | `.lsd` | deps/lsd | v11–v15 system/user/abbreviation |
| MDict | `.mdx` / `.mdd` | deps/cmdx | v2/v3, encrypted, GBK/UTF-16/UTF-8 |
| StarDict | `.ifo` | deps/stardict | `.ifo` + `.idx` + `.dict(.dz)` |
| ZIM | `.zim` | deps/czim | Wikipedia/Kiwix archive format |
| DSL | `.dsl` / `.dsl.dz` | deps/lsd | Lingvo DSL source format |
| UDX | `.udx` | (built-in) | unidict's external index; built from any of the above |

Format is auto-detected from the file path extension / structure.

## Build

unidict builds with **either** Xcode **or** CMake.

### Prerequisites

The deps pull in these system libraries (installed transitively where
possible):

- zlib, libxml2, iconv — core
- zstd, liblzma — ZIM (deps/czim)
- ICU — MDict (deps/cmdx)
- libvorbis, libunistring — Lingvo (deps/lsd)

First, initialise the git submodules (every dependency lives under
`deps/`):

```bash
git submodule update --init --recursive
```

### CMake (cross-platform)

```bash
cmake -B build -DUNIDICT_BUILD_TESTS=ON
cmake --build build --config Release
```

This produces `libunidict.a` (or the shared library if
`-DBUILD_SHARED_LIBS=ON`).

### Xcode (macOS)

```bash
xcodebuild -project unidict.xcodeproj -scheme unidict build     # library
xcodebuild -project unidict.xcodeproj -scheme udtests build     # library + tests
```

Build outputs go to `build/`.

## Tests

The test suite (Unity-based) covers every supported format through the
public `unidict_*` API: open, info, lookup, suggest, entry iteration,
and external index build. Test fixtures are the real dictionary files
shipped with each dependency under `deps/*/tests/data/`, so they are
available as soon as submodules are initialised.

```bash
ctest --test-dir build --output-on-failure      # after cmake build
```

Fixture paths are resolved from the repository root (compile-time
`UNIDICT_SOURCE_ROOT`, overridable at runtime via the
`UNIDICT_TEST_DATA_ROOT` environment variable) — tests never hardcode
absolute paths.

## Architecture

- **Object system.** All dictionary handles inherit from `uobject`
  (deps/uobject) — an atomic refcount base similar to Linux kobjects.
  Always use `uobject_retain` / `uobject_release` for lifecycle.
- **Virtual dispatch.** Each format backend defines a `unidict_ops`
  vtable (`src/unidict_internal.h`) that the core API dispatches
  through. The `unidict` base struct embeds `uobject` plus `ops`,
  `format`, and index state; backends embed this base as their first
  member and recover the private struct with `uobject_cast()`
  (`container_of`).
- **Format backends** live in `src/formats/ud_<format>.c`. Each
  implements the vtable, a `uobject_type` with a release destructor,
  and an exported `ud_<format>_open()`.
- **External index (UDX).** Formats without a fast builtin index
  (Babylon, Lingvo DSL, Lingoes) build a UDX file via
  `unidict_index_external_make()` for prefix/suggest lookups.
- **Dependencies** are 10 git submodules under `deps/`, each an
  independent C library (its own Xcode project and CMakeLists).

See `CLAUDE.md` for contributor guidance and `docs/` for format
specifications.

## License

See the project files for license details.
