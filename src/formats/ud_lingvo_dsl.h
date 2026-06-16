//
//  ud_lingvo_dsl.h
//  unidict
//
//  Created by kejinlu on 2026-05-27
//
#ifndef ud_lingvo_dsl_h
#define ud_lingvo_dsl_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

unidict *ud_lingvo_dsl_open(const char *file_path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_lingvo_dsl_h */
