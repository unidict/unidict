//
//  test_dsl.c
//  unidict
//
//  Lingvo DSL (.dsl/.dsl.dz) format coverage. Fixture:
//  deps/lsd/tests/data/en_us_ipa.dsl.dz
//
//  DSL has no builtin index: an external UDX index must be built before
//  lookup/suggest.
//
#include "unity.h"
#include "test_helper.h"

void test_dsl_open_info(void) {
    unidict_open_options opts = { .index_type = UNIDICT_INDEX_BUILTIN };
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_DSL, &opts);
    unidict_test_assert_format(dict, UNIDICT_FORMAT_DSL);

    unidict_info *info = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_info_get(dict, &info));
    TEST_ASSERT_NOT_NULL(info->title);
    unidict_info_free(info);
    unidict_close(dict);
}

void test_dsl_external_index_suggest(void) {
    unidict_open_options opts = { .index_type = UNIDICT_INDEX_BUILTIN };
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_DSL, &opts);

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

void run_dsl_tests(void) {
    UnityBegin("test_dsl.c");
    RUN_TEST(test_dsl_open_info);
    RUN_TEST(test_dsl_external_index_suggest);
    UnityEnd();
}
