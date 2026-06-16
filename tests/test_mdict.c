//
//  test_mdict.c
//  unidict
//
//  MDict (.mdx) format coverage. Fixture:
//  deps/cmdx/tests/data/english-italian.mdx (hello -> ciao)
//
#include "unity.h"
#include "test_helper.h"

void test_mdict_open_info(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_MDX, NULL);
    unidict_test_assert_format(dict, UNIDICT_FORMAT_MDICT);
    unidict_close(dict);
}

void test_mdict_entry_iter(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_MDX, NULL);
    unidict_test_assert_iter_has_entry(dict);
    unidict_close(dict);
}

void test_mdict_lookup_hello(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_MDX, NULL);

    unidict_article_array *arts = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_lookup(dict, "hello", &arts));
    TEST_ASSERT_NOT_NULL(arts);
    TEST_ASSERT_TRUE(arts->count > 0);
    TEST_ASSERT_NOT_NULL(arts->items[0].body);
    unidict_article_array_free(arts);
    unidict_close(dict);
}

void test_mdict_suggest(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_MDX, NULL);

    unidict_entry_array *entries = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_suggest(dict, "h", 10, &entries));
    TEST_ASSERT_NOT_NULL(entries);
    TEST_ASSERT_TRUE(entries->count > 0);
    unidict_entry_array_free(entries);
    unidict_close(dict);
}

void run_mdict_tests(void) {
    UnityBegin("test_mdict.c");
    RUN_TEST(test_mdict_open_info);
    RUN_TEST(test_mdict_entry_iter);
    RUN_TEST(test_mdict_lookup_hello);
    RUN_TEST(test_mdict_suggest);
    UnityEnd();
}
