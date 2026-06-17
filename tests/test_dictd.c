//
//  test_dictd.c
//  unidict
//
//  DictD (.index) format coverage. Fixture:
//  deps/stardict/tests/data/dictd-eng-zho/eng-zho.index
//  (English-Chinese dictionary with 00-database-* metadata entries)
//
#include "unity.h"
#include "test_helper.h"

void test_dictd_open_info(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_DICTD, NULL);
    unidict_test_assert_format(dict, UNIDICT_FORMAT_DICTD);

    // info_get must succeed; the short name comes from 00-database-short.
    unidict_info *info = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_info_get(dict, &info));
    TEST_ASSERT_NOT_NULL(info);
    TEST_ASSERT_EQUAL(UNIDICT_FORMAT_DICTD, info->format);
    TEST_ASSERT_NOT_NULL(info->title);
    unidict_info_free(info);
    unidict_close(dict);
}

void test_dictd_entry_iter(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_DICTD, NULL);
    unidict_test_assert_iter_has_entry(dict);
    unidict_close(dict);
}

void test_dictd_feature_page_meta(void) {
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_DICTD, NULL);

    // feature_pages_list must report a "meta" page for this archive
    // (the fixture has six 00-database-* entries).
    unidict_feature_page_array *pages = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_feature_pages_list(dict, &pages));
    TEST_ASSERT_NOT_NULL(pages);
    TEST_ASSERT_TRUE(pages->count >= 1);

    bool found_meta = false;
    for (size_t i = 0; i < pages->count; i++) {
        if (pages->items[i].key && strcmp(pages->items[i].key, "meta") == 0) {
            found_meta = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_meta, "expected a 'meta' feature page");

    // feature_page_read("meta") must return non-empty HTML containing the
    // metadata table and at least one known 00-database-* entry name.
    char *html = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_feature_page_read(dict, "meta", &html));
    TEST_ASSERT_NOT_NULL(html);
    TEST_ASSERT_TRUE(strlen(html) > 0);
    // Real HTML markup must be present (not escaped), and user data
    // (00databaseshort) must appear verbatim.
    TEST_ASSERT_NOT_NULL(strstr(html, "<!DOCTYPE html>"));
    TEST_ASSERT_NULL(strstr(html, "&lt;DOCTYPE"));
    TEST_ASSERT_NOT_NULL(strstr(html, "00databaseshort"));
    free(html);

    unidict_feature_page_array_free(pages);
    unidict_close(dict);
}

void run_dictd_tests(void) {
    UnityBegin("test_dictd.c");
    RUN_TEST(test_dictd_open_info);
    RUN_TEST(test_dictd_entry_iter);
    RUN_TEST(test_dictd_feature_page_meta);
    UnityEnd();
}
