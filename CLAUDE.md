# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build commands

```bash
xcodebuild -project unidict.xcodeproj -scheme unidict build     # Build the library (Release)
xcodebuild -project unidict.xcodeproj -scheme udtests build     # Build + run tests
```

Build outputs go to `build/` — `libunidict.a` is the static library, the test binary runs directly.

## Repository overview

unidict is a C library providing a unified API to read dictionary files across 8 formats (Babylon, DictD, EPWING, Lingoes, Lingvo, MDict, StarDict, ZIM). It has a per-handle, thread-safe design modeled after libcurl: each `unidict` handle is independent and usable by one thread at a time.

## Architecture

**Object system.** All dictionary handles inherit from `uobject` (defined in `deps/uobject/`) — an atomic refcount base similar to Linux kobjects. Always use `uobject_retain`/`uobject_release` for lifecycle management.

**Virtual dispatch.** Each format backend defines a `unidict_ops` vtable (`src/unidict_internal.h:16`) that the core API dispatches through. The `unidict` base struct embeds `uobject` as its first member, plus `ops`, `format`, index state, and `prepared` flags. Backends embed this base struct as their first member and use `uobject_cast()` (container_of) to recover the private struct. The core API in `src/unidict.c` detects the format from the file path, opens the appropriate backend via `ud_<format>_open()`, and delegates all operations to `dict->ops->*`.

**Format backends** (`src/formats/<format>/`): Each is a pair of `ud_<format>.c`/`.h` files. They all follow the same pattern — define a private struct with `unidict base` as first member, implement a static `unidict_ops` vtable, define a `uobject_type` with a `release` destructor, and export an `ud_<format>_open()` that allocates, initializes, and returns the handle.

**Dependencies** are 10 git submodules under `deps/`, each a separate C library for parsing a specific dictionary format (e.g., `cmdx` for MDict, `bgl` for Babylon, `czim` for ZIM). Each has its own Xcode project and CMakeLists.txt but is built as part of the unidict Xcode project.

**Tests** (`tests/main.c`) are a single C file that opens a hardcoded MDX test file and exercises the main API. The test dictionary path is platform-specific — update it if you clone the repo elsewhere.

## Key patterns

- `unidict_*_free()` functions consume ownership of externally-allocated result structs
- All public types are opaque (typedef'd struct pointers)
- The `unidict_ops` vtable has required ops (`lookup`) and optional ops (everything else can be NULL)
- The entry iterator model reuses a single `current` entry across calls — callers must not free the returned pointer
- New format backends: create a subdir under `src/formats/`, implement the ops vtable, add to format detection in `src/unidict.c`, and add to the Xcode project
