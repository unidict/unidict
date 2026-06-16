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
