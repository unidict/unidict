//
//  ud_compat.h
//  unidict
//
//  Cross-platform shims for POSIX facilities that MSVC/Windows lacks.
//  Fills in strcasecmp / strncasecmp (no <strings.h> on MSVC) and a
//  minimal dirent directory-iteration API (no <dirent.h> on MSVC) so
//  the format backends compile unchanged on Windows.
//
#ifndef UD_COMPAT_H
#define UD_COMPAT_H

#include <string.h>

// ============================================================
// Case-insensitive string comparison
// ============================================================
// POSIX declares strcasecmp / strncasecmp in <strings.h>. MSVC has no
// <strings.h>; it offers _stricmp / _strnicmp in <string.h> instead.
// On Linux/macOS, callers still #include <strings.h> themselves, so we
// only need to map the names on MSVC.
#ifdef _MSC_VER
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

// ============================================================
// PATH_MAX
// ============================================================
// POSIX defines PATH_MAX in <limits.h>. MSVC's <limits.h> lacks it.
#ifdef _MSC_VER
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif
#endif

// ============================================================
// S_ISDIR / S_ISREG
// ============================================================
// POSIX defines these as macros in <sys/stat.h>. MSVC's <sys/stat.h>
// provides _S_IFDIR / _S_IFREG but not the S_IS* convenience macros.
#ifdef _MSC_VER
#include <sys/stat.h>
#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & _S_IFMT) == _S_IFREG)
#endif
#endif

// ============================================================
// strndup
// ============================================================
// POSIX function; MSVC only has strdup (_strdup). Provide a static
// inline implementation backed by the C runtime allocator.
#ifdef _MSC_VER
#include <string.h>
static inline char *ud_strndup(const char *s, size_t n) {
    size_t len = strnlen_s(s, n);
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}
#define strndup ud_strndup
#endif

// ============================================================
// Directory iteration (dirent)
// ============================================================
// MSVC has no <dirent.h>. Provide a minimal, allocation-safe wrapper
// over FindFirstFileA / FindNextFileA so backends can use the standard
// opendir / readdir / closedir trio unchanged. readdir returns a
// pointer into a dirent embedded in DIR (single-entry buffer), which
// matches how glibc's readdir works in practice.
#ifdef _WIN32

#include <windows.h>
#include <stdlib.h>

struct dirent {
    char d_name[MAX_PATH + 1];
};

typedef struct DIR {
    HANDLE           handle;
    WIN32_FIND_DATAA data;
    int              cached;       // 1 = data holds an unread entry
    struct dirent    ent;          // buffer returned by readdir
} DIR;

static inline DIR *opendir(const char *name) {
    if (!name || !*name) return NULL;

    DIR *dir = (DIR *)calloc(1, sizeof(DIR));
    if (!dir) return NULL;

    // Build "<name>\*" search pattern.
    char pattern[MAX_PATH + 4];
    int n = _snprintf(pattern, sizeof(pattern), "%s\\*", name);
    if (n < 0 || n >= (int)sizeof(pattern)) {
        free(dir);
        return NULL;
    }

    dir->handle = FindFirstFileA(pattern, &dir->data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    dir->cached = 1;
    return dir;
}

static inline struct dirent *readdir(DIR *dir) {
    if (!dir) return NULL;
    if (!dir->cached) {
        if (!FindNextFileA(dir->handle, &dir->data)) {
            return NULL;
        }
    }
    dir->cached = 0;
    strncpy(dir->ent.d_name, dir->data.cFileName, MAX_PATH);
    dir->ent.d_name[MAX_PATH] = '\0';
    return &dir->ent;
}

static inline int closedir(DIR *dir) {
    if (!dir) return -1;
    BOOL ok = FindClose(dir->handle);
    free(dir);
    return ok ? 0 : -1;
}

#endif // _WIN32

#endif // UD_COMPAT_H
