//
//  ud_lingoes.h
//  unidict
//
//  Created by kejinlu on 2026-01-31
//
#ifndef ud_lingoes_h
#define ud_lingoes_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

unidict *ud_lingoes_open(const char *file_path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_lingoes_h */
