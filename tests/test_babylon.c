//
//  test_babylon.c
//  unidict
//
//  Babylon (.bgl) format coverage via the public unidict API.
//  Fixture: deps/bgl/tests/data/english_chinese_s_.bgl (70919 entries)
//
//  Babylon has no builtin index: raw mode reads entries directly; for
//  lookup/suggest an external UDX index must be built first.
//
#include "unity.h"
#include "test_helper.h"

void test_bgl_open_info(void) {
    unidict_open_options opts = { .index_type = UNIDICT_INDEX_BUILTIN };
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_BGL, &opts);
    unidict_test_assert_format(dict, UNIDICT_FORMAT_BABYLON);

    unidict_info *info = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_info_get(dict, &info));
    TEST_ASSERT_NOT_NULL(info->title);
    TEST_ASSERT_EQUAL_STRING("Babylon English-Chinese (S)", info->title);
    unidict_info_free(info);
    unidict_close(dict);
}

void test_bgl_entry_iter_first(void) {
    unidict_open_options opts = { .index_type = UNIDICT_INDEX_BUILTIN };
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_BGL, &opts);

    // Babylon has no builtin index: entry iteration requires an external UDX.
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_index_external_make(dict, unidict_test_index_cb, NULL));
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_index_activate(dict, UNIDICT_INDEX_EXTERNAL));

    unidict_entry_iter *iter = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_entry_iter_create(dict, &iter));
    unidict_entry *e = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_entry_iter_next(iter, &e));
    TEST_ASSERT_NOT_NULL(e->key);
    unidict_entry_iter_free(iter);
    unidict_close(dict);
}

void test_bgl_external_index_suggest(void) {
    unidict_open_options opts = { .index_type = UNIDICT_INDEX_BUILTIN };
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_BGL, &opts);

    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_index_external_make(dict, unidict_test_index_cb, NULL));
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_index_activate(dict, UNIDICT_INDEX_EXTERNAL));
    TEST_ASSERT_TRUE(unidict_index_has_external(dict));

    unidict_entry_array *entries = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_suggest(dict, "a", 10, &entries));
    TEST_ASSERT_NOT_NULL(entries);
    TEST_ASSERT_TRUE(entries->count > 0);
    unidict_entry_array_free(entries);
    unidict_close(dict);
}

void run_babylon_tests(void) {
    UnityBegin("test_babylon.c");
    RUN_TEST(test_bgl_open_info);
    RUN_TEST(test_bgl_entry_iter_first);
    RUN_TEST(test_bgl_external_index_suggest);
    UnityEnd();
}
