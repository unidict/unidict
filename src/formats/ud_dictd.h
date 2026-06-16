//
//  ud_dictd.h
//  unidict
//
//  Created by kejinlu on 2025-11-25
//
#ifndef ud_dictd_h
#define ud_dictd_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

unidict *ud_dictd_open(const char *index_path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_dictd_h */
