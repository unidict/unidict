//
//  test_zim.c
//  unidict
//
//  ZIM (.zim) format coverage. Fixture:
//  deps/czim/tests/data/wikipedia_en_100_mini_2026-01.zim
//
#include "unity.h"
#include "test_helper.h"

void test_zim_open_info(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_ZIM, NULL);
    unidict_test_assert_format(dict, UNIDICT_FORMAT_ZIM);
    unidict_close(dict);
}

void test_zim_entry_iter(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_ZIM, NULL);
    unidict_test_assert_iter_has_entry(dict);
    unidict_close(dict);
}

void test_zim_lookup_known_entry(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_ZIM, NULL);

    // "African_Americans" is a known article in this mini ZIM.
    unidict_article_array *arts = NULL;
    unidict_status st = unidict_lookup(dict, "African_Americans", &arts);
    TEST_ASSERT_TRUE(st == UNIDICT_OK);
    if (arts) {
        TEST_ASSERT_TRUE(arts->count > 0);
        TEST_ASSERT_NOT_NULL(arts->items[0].body);
        unidict_article_array_free(arts);
    }
    unidict_close(dict);
}

void run_zim_tests(void) {
    UnityBegin("test_zim.c");
    RUN_TEST(test_zim_open_info);
    RUN_TEST(test_zim_entry_iter);
    RUN_TEST(test_zim_lookup_known_entry);
    UnityEnd();
}
