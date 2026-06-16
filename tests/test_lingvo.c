//
//  test_lingvo.c
//  unidict
//
//  Lingvo (.lsd) format coverage. Fixture:
//  deps/lsd/tests/data/system_15_activederu.lsd (v15, De-Ru, 1678 entries)
//
#include "unity.h"
#include "test_helper.h"

void test_lingvo_open_info(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_LSD, NULL);
    unidict_test_assert_format(dict, UNIDICT_FORMAT_LINGVO);

    unidict_info *info = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_info_get(dict, &info));
    TEST_ASSERT_NOT_NULL(info);
    TEST_ASSERT_EQUAL_UINT64(1678, info->word_count);
    unidict_info_free(info);
    unidict_close(dict);
}

void test_lingvo_entry_iter_first2(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_LSD, NULL);

    unidict_entry_iter *iter = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_entry_iter_create(dict, &iter));
    unidict_entry *e = NULL;

    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_entry_iter_next(iter, &e));
    TEST_ASSERT_EQUAL_STRING("'rein", e->key);

    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_entry_iter_next(iter, &e));
    TEST_ASSERT_EQUAL_STRING("Abend", e->key);

    unidict_entry_iter_free(iter);
    unidict_close(dict);
}

void test_lingvo_lookup_abend(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_LSD, NULL);

    unidict_article_array *arts = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_lookup(dict, "Abend", &arts));
    TEST_ASSERT_NOT_NULL(arts);
    TEST_ASSERT_TRUE(arts->count > 0);
    TEST_ASSERT_NOT_NULL(arts->items[0].body);
    unidict_article_array_free(arts);
    unidict_close(dict);
}

void run_lingvo_tests(void) {
    UnityBegin("test_lingvo.c");
    RUN_TEST(test_lingvo_open_info);
    RUN_TEST(test_lingvo_entry_iter_first2);
    RUN_TEST(test_lingvo_lookup_abend);
    UnityEnd();
}
