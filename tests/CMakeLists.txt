# Unity Test Framework
set(UNITY_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/unity/unity.c)

# Per-format test sources (one suite per supported dictionary format)
set(TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_fixture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_babylon.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_stardict.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dictd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lingvo.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_zim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mdict.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dsl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lingoes.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_epwing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
)

add_executable(unidict_tests ${UNITY_SOURCES} ${TEST_SOURCES})

target_include_directories(unidict_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/unity
        ${CMAKE_SOURCE_DIR}/src
)

# Test fixture paths are resolved at runtime via __FILE__ in test_data.h
# (same trick the deps use), so no compile-time path injection is needed.
# UNIDICT_TEST_DATA_ROOT env var still overrides if set.

target_link_libraries(unidict_tests PRIVATE unidict)

add_test(NAME UnidictTestSuite COMMAND unidict_tests)
