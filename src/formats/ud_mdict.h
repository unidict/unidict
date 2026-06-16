//
//  ud_mdict.h
//  unidict
//
//  Created by kejinlu on 2025-11-25
//
#ifndef ud_mdict_h
#define ud_mdict_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

unidict *ud_mdict_open(const char *mdx_path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_mdict_h */
