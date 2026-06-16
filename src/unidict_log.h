//
//  unidict_log.h
//  unidict
//
//  Created by kejinlu on 2025-12-03
//
#ifndef unidict_log_h
#define unidict_log_h
#include <stdio.h>

// ============================================================
// Log Level
// ============================================================

#define UD_LOG_LEVEL_ERROR 0
#define UD_LOG_LEVEL_WARN  1
#define UD_LOG_LEVEL_INFO  2
#define UD_LOG_LEVEL_DEBUG 3

#ifndef UD_LOG_LEVEL
#define UD_LOG_LEVEL UD_LOG_LEVEL_WARN
#endif

// ============================================================
// Log Macros
// ============================================================

#if UD_LOG_LEVEL >= UD_LOG_LEVEL_ERROR
#define UD_LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#else
#define UD_LOG_ERROR(fmt, ...) ((void)0)
#endif

#if UD_LOG_LEVEL >= UD_LOG_LEVEL_WARN
#define UD_LOG_WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#else
#define UD_LOG_WARN(fmt, ...) ((void)0)
#endif

#if UD_LOG_LEVEL >= UD_LOG_LEVEL_INFO
#define UD_LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)
#else
#define UD_LOG_INFO(fmt, ...) ((void)0)
#endif

#if UD_LOG_LEVEL >= UD_LOG_LEVEL_DEBUG
#define UD_LOG_DEBUG(fmt, ...) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define UD_LOG_DEBUG(fmt, ...) ((void)0)
#endif

#endif /* unidict_log_h */
