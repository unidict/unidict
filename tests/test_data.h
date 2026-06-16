//
//  test_data.h
//  unidict
//
//  Test fixture data path resolution.
//
//  All dictionary test data lives inside the git submodules under
//  deps/<lib>/tests/data/. The root is resolved at runtime:
//
//    1. the UNIDICT_TEST_DATA_ROOT env var, if set;
//    2. otherwise the repo source root passed in at compile time
//       (UNIDICT_SOURCE_ROOT, defined by CMake).
//
//  Tests must never hardcode absolute paths.
//
#ifndef unidict_test_data_h
#define unidict_test_data_h

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Derive the repository root from this header's location at compile time
// (the same trick the deps use in their test_*.c: __FILE__ is the path of
// the .c file that includes this header, which lives under <root>/tests/).
// Stripping the last two path segments ("tests/<file>") yields <root>.
// No environment variable or compile-time injection needed; works on any
// platform (handles both '/' and '\\') and under both CMake and Xcode.
static const char *unidict_test_root(void) {
    static char root[1024] = {0};
    if (root[0] != '\0') return root;

    // Optional override (e.g. when the source tree is relocated).
    const char *env = getenv("UNIDICT_TEST_DATA_ROOT");
    if (env && env[0]) {
        snprintf(root, sizeof(root), "%s", env);
        return root;
    }

    // __FILE__ = "<root>/tests/<including_file>.c"
    const char *file = __FILE__;
    const char *last2[2] = {NULL, NULL};
    for (const char *p = file; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last2[0] = last2[1];
            last2[1] = p;
        }
    }
    // last2[0] points at the '/' before "tests"; everything before it is <root>.
    if (last2[0]) {
        size_t len = (size_t)(last2[0] - file);
        snprintf(root, sizeof(root), "%.*s", (int)len, file);
    } else {
        snprintf(root, sizeof(root), ".");
    }
    return root;
}

// Resolve a fixture path relative to the data root into a static buffer.
// rel is like "deps/lsd/tests/data/system_15_activederu.lsd".
static const char *unidict_fixture(const char *rel) {
    static char buf[2048];
    snprintf(buf, sizeof(buf), "%s/%s", unidict_test_root(), rel);
    return buf;
}

// ---- Per-format representative fixtures (relative to repo root) ----
//
// One canonical fixture per supported format. Picked to exercise the
// full open -> info -> lookup -> suggest -> iterate path.
#define UNIDICT_FIXTURE_BGL       "deps/bgl/tests/data/english_chinese_s_.bgl"
#define UNIDICT_FIXTURE_MDX       "deps/cmdx/tests/data/english-italian.mdx"
#define UNIDICT_FIXTURE_STARDICT  "deps/stardict/tests/data/stardict-xiandaiyinghan-2.4.2/xiandaiyinghan.ifo"
#define UNIDICT_FIXTURE_DICTD     "deps/stardict/tests/data/dictd-eng-zho/eng-zho.index"
#define UNIDICT_FIXTURE_ZIM       "deps/czim/tests/data/wikipedia_en_100_mini_2026-01.zim"
#define UNIDICT_FIXTURE_LSD       "deps/lsd/tests/data/system_15_activederu.lsd"
#define UNIDICT_FIXTURE_DSL       "deps/lsd/tests/data/en_us_ipa.dsl.dz"
#define UNIDICT_FIXTURE_LINGOES   "deps/ldx/tests/resources/chinese_daily_cookbook.ld2"
#define UNIDICT_FIXTURE_EPWING    "deps/ebcore/tests/data/gks2/Epwing"

// EPWING open options (only field that matters for the smoke tests).
#define UNIDICT_EPWING_OPTS \
    { .epwing_gaiji_mode = UNIDICT_EPWING_GAIJI_BITMAP }

#endif /* unidict_test_data_h */
