//
//  ud_babylon.h
//  unidict
//
//  Created by kejinlu on 2026-02-02
//
#ifndef ud_babylon_h
#define ud_babylon_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

unidict *ud_babylon_open(const char *bgl_path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_babylon_h */
