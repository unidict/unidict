//
//  ud_stardict.h
//  unidict
//
//  Created by kejinlu on 2026-01-01
//
#ifndef ud_stardict_h
#define ud_stardict_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

unidict *ud_stardict_open(const char *ifo_path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_stardict_h */
