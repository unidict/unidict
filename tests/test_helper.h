//
//  test_helper.h
//  unidict
//
//  Shared helpers for per-format test suites.
//
#ifndef unidict_test_helper_h
#define unidict_test_helper_h

#include "test_data.h"
#include "unidict.h"
#include <stdbool.h>

// Index-build callback that never cancels.
static bool unidict_test_index_cb(unidict *dict, unidict_index_stage stage,
                                  int percent, void *user_data) {
    (void)dict; (void)stage; (void)percent; (void)user_data;
    return true;
}

// Open a fixture with default (auto-detect) index type and assert success.
// Caller frees with unidict_close().
static unidict *unidict_test_open(const char *fixture_rel,
                                  const unidict_open_options *opts) {
    unidict *dict = NULL;
    unidict_status st = unidict_open(unidict_fixture(fixture_rel), opts, &dict);
    TEST_ASSERT_EQUAL_MESSAGE(UNIDICT_OK, st, unidict_strerror(st));
    TEST_ASSERT_NOT_NULL(dict);
    return dict;
}

// Common: info_get must succeed and report the expected format.
static void unidict_test_assert_format(unidict *dict, unidict_format expected) {
    unidict_info *info = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_info_get(dict, &info));
    TEST_ASSERT_NOT_NULL(info);
    TEST_ASSERT_EQUAL(expected, info->format);
    unidict_info_free(info);
}

// Common: entry iterator must yield at least one entry.
static void unidict_test_assert_iter_has_entry(unidict *dict) {
    unidict_entry_iter *iter = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_entry_iter_create(dict, &iter));
    TEST_ASSERT_NOT_NULL(iter);
    unidict_entry *e = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_entry_iter_next(iter, &e));
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_NOT_NULL(e->key);
    unidict_entry_iter_free(iter);
}

#endif /* unidict_test_helper_h */
