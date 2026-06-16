//
//  ud_epwing.h
//  unidict
//
//  Created by kejinlu on 2026-01-20
//
#ifndef ud_epwing_h
#define ud_epwing_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

unidict *ud_epwing_open(const char *path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_epwing_h */
