//
//  test_epwing.c
//  unidict
//
//  EPWING (directory with CATALOGS) format coverage. Fixture:
//  deps/ebcore/tests/data/gks2/Epwing
//
#include "unity.h"
#include "test_helper.h"

void test_epwing_open_info(void) {
    unidict_open_options opts = UNIDICT_EPWING_OPTS;
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_EPWING, &opts);
    unidict_test_assert_format(dict, UNIDICT_FORMAT_EPWING);

    unidict_info *info = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_info_get(dict, &info));
    TEST_ASSERT_NOT_NULL(info->title);
    unidict_info_free(info);
    unidict_close(dict);
}

void test_epwing_lookup_known(void) {
    unidict_open_options opts = UNIDICT_EPWING_OPTS;
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_EPWING, &opts);

    // "5001" is a known entry in this EPWING fixture.
    unidict_article_array *arts = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_lookup(dict, "5001", &arts));
    TEST_ASSERT_NOT_NULL(arts);
    TEST_ASSERT_TRUE(arts->count > 0);
    unidict_article_array_free(arts);
    unidict_close(dict);
}

void test_epwing_feature_pages(void) {
    unidict_open_options opts = UNIDICT_EPWING_OPTS;
    unidict *dict = unidict_test_open(UNIDICT_FIXTURE_EPWING, &opts);

    unidict_feature_page_array *pages = NULL;
    TEST_ASSERT_EQUAL(UNIDICT_OK, unidict_feature_pages_list(dict, &pages));
    TEST_ASSERT_NOT_NULL(pages);
    // EPWING typically exposes menu/copyright pages; list call must succeed.
    unidict_feature_page_array_free(pages);
    unidict_close(dict);
}

void run_epwing_tests(void) {
    UnityBegin("test_epwing.c");
    RUN_TEST(test_epwing_open_info);
    RUN_TEST(test_epwing_lookup_known);
    RUN_TEST(test_epwing_feature_pages);
    UnityEnd();
}
