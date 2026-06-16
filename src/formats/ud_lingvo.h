//
//  ud_lingvo.h
//  unidict
//
//  Created by kejinlu on 2026-01-15
//
#ifndef ud_lingvo_h
#define ud_lingvo_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

unidict *ud_lingvo_open(const char *lsd_path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_lingvo_h */
