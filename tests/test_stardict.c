//
//  test_stardict.c
//  unidict
//
//  StarDict (.ifo) format coverage. Fixture:
//  deps/stardict/tests/data/stardict-xiandaiyinghan-2.4.2/xiandaiyinghan.ifo
//  (40305 entries, "现代英汉词典")
//
#include "unity.h"
#include "test_helper.h"

void test_stardict_open_info(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_STARDICT, NULL);
    unidict_test_assert_format(dict, UNIDICT_FORMAT_STARDICT);

    unidict_info *info = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_info_get(dict, &info));
    TEST_ASSERT_NOT_NULL(info->title);
    TEST_ASSERT_EQUAL_STRING("现代英汉词典", info->title);
    TEST_ASSERT_EQUAL_UINT64(40305, info->word_count);
    unidict_info_free(info);
    unidict_close(dict);
}

void test_stardict_lookup_hello(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_STARDICT, NULL);

    unidict_article_array *arts = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_lookup(dict, "hello", &arts));
    TEST_ASSERT_NOT_NULL(arts);
    TEST_ASSERT_EQUAL_size_t(1, arts->count);
    TEST_ASSERT_NOT_NULL(arts->items[0].body);
    unidict_article_array_free(arts);
    unidict_close(dict);
}

void test_stardict_suggest_h(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_STARDICT, NULL);

    unidict_entry_array *entries = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_suggest(dict, "h", 5, &entries));
    TEST_ASSERT_NOT_NULL(entries);
    TEST_ASSERT_EQUAL_size_t(5, entries->count);
    TEST_ASSERT_EQUAL_STRING("H", entries->items[0]->key);
    unidict_entry_array_free(entries);
    unidict_close(dict);
}

void test_stardict_entry_iter(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_STARDICT, NULL);
    unidict_test_assert_iter_has_entry(dict);
    unidict_close(dict);
}

void run_stardict_tests(void) {
    UnityBegin("test_stardict.c");
    RUN_TEST(test_stardict_open_info);
    RUN_TEST(test_stardict_lookup_hello);
    RUN_TEST(test_stardict_suggest_h);
    RUN_TEST(test_stardict_entry_iter);
    UnityEnd();
}
