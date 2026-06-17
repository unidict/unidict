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

void test_zim_feature_page_main(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_ZIM, NULL);

    // feature_pages_list must report a "main" page for this archive.
    unidict_feature_page_array *pages = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_feature_pages_list(dict, &pages));
    TEST_ASSERT_NOT_NULL(pages);
    TEST_ASSERT_TRUE(pages->count >= 1);

    bool found_main = false;
    for (size_t i = 0; i < pages->count; i++) {
        if (pages->items[i].key && strcmp(pages->items[i].key, "main") == 0) {
            found_main = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_main, "expected a 'main' feature page");

    // feature_page_read("main") must return non-empty HTML content.
    char *html = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_feature_page_read(dict, "main", &html));
    TEST_ASSERT_NOT_NULL(html);
    TEST_ASSERT_TRUE(strlen(html) > 0);
    free(html);

    unidict_feature_page_array_free(pages);
    unidict_close(dict);
}

void run_zim_tests(void) {
    UnityBegin("test_zim.c");
    RUN_TEST(test_zim_open_info);
    RUN_TEST(test_zim_entry_iter);
    RUN_TEST(test_zim_lookup_known_entry);
    RUN_TEST(test_zim_feature_page_main);
    UnityEnd();
}
