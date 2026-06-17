//
//  main.c
//  unidict tests
//
//  Unity runner: aggregates every per-format test suite.
//
#include "unity.h"

// Per-format suite declarations
void run_babylon_tests(void);
void run_stardict_tests(void);
void run_dictd_tests(void);
void run_lingvo_tests(void);
void run_zim_tests(void);
void run_mdict_tests(void);
void run_dsl_tests(void);
void run_lingoes_tests(void);
void run_epwing_tests(void);

int main(void) {
    run_babylon_tests();
    run_stardict_tests();
    run_dictd_tests();
    run_lingvo_tests();
    run_zim_tests();
    run_mdict_tests();
    run_dsl_tests();
    run_lingoes_tests();
    run_epwing_tests();
    return 0;
}
