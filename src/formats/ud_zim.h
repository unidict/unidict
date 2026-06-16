//
//  ud_zim.h
//  unidict
//
//  Created by kejinlu on 2026-01-22
//
#ifndef ud_zim_h
#define ud_zim_h

#include "unidict.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 打开 ZIM 词典
 * @param path 文件路径
 * @return 词典对象，失败返回 NULL
 */
unidict *ud_zim_open(const char *path, const unidict_open_options *options);

#ifdef __cplusplus
}
#endif

#endif /* ud_zim_h */
