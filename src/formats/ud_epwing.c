//
//  ud_epwing.c
//  unidict
//
//  Created by kejinlu on 2026-01-20
//
#include "ud_epwing.h"
#include "unidict_internal.h"
#include "udx_writer.h"
#include "ud_udx.h"
#include <eb.h>
#include <text.h>
#include <appendix.h>
#include <binary.h>
#include <font.h>
#include <error.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <iconv.h>

// ============================================================
// Private struct
// ============================================================

typedef struct ud_epwing ud_epwing;

typedef struct {
    int page;
    int offset;
} epwing_cand_ref;

typedef struct {
    char *title;
    int page;
    int offset;
} epwing_menu_item;

struct ud_epwing {
    unidict base;
    char *path;
    EB_Book book;
    EB_Appendix appendix;
    EB_Hookset hookset_heading;
    EB_Hookset hookset_text;
    EB_Hookset hookset_menu;
    unidict_epwing_gaiji_mode gaiji_mode;
    bool subbook_set;
    EB_Subbook_Code current_subbook;
    int subbook_count;
    char *title;
    unidict *udx_dict;
    iconv_t cd_euc;  // EUC-JP → UTF-8
    iconv_t cd_iso;  // ISO-8859-1 → UTF-8
    iconv_t cd_gb;   // GB2312 → UTF-8
    iconv_t cd_sjis; // Shift-JIS → UTF-8
    // Candidate reference list (placeholder → real URL resolution)
    epwing_cand_ref *cand_refs;
    int cand_ref_count;
    int cand_ref_capacity;
    // Cross-reference link list (placeholder → real URL resolution)
    epwing_cand_ref *ref_refs;
    int ref_ref_count;
    int ref_ref_capacity;
    // Menu item collection (for recursive menu expansion)
    epwing_menu_item *menu_items;
    int menu_item_count;
    int menu_item_capacity;
};

// ============================================================
// Type definition
// ============================================================

static void ud_epwing_release(uobject *obj);

static const uobject_type ud_epwing_type = {
    .name = "ud_epwing",
    .size = sizeof(ud_epwing),
    .release = ud_epwing_release,
};

// ============================================================
// Entry Ref (suggest → fetch fast path)
// ============================================================

typedef struct {
    uobject obj;
    EB_Hit hit;
} ud_epwing_entry_ref;

static void ud_epwing_entry_ref_release(uobject *obj) {
    ud_epwing_entry_ref *ref = uobject_cast(obj, ud_epwing_entry_ref, obj);
    free(ref);
}

static const uobject_type ud_epwing_entry_ref_type = {
    .name = "ud_epwing_entry_ref",
    .size = sizeof(ud_epwing_entry_ref),
    .release = ud_epwing_entry_ref_release,
};

// ============================================================
// EB helpers
// ============================================================

static ssize_t epwing_read_heading(ud_epwing *epwing, const EB_Position *position, char *buffer, size_t buffer_size) {
    if (eb_seek_text(&epwing->book, position) != EB_SUCCESS) return -1;
    ssize_t length;
    if (eb_read_heading(&epwing->book, NULL, &epwing->hookset_heading, epwing, buffer_size - 1, buffer, &length) != EB_SUCCESS) return -1;
    buffer[length] = '\0';
    return length;
}

static void resolve_refs(const char *placeholder_prefix, epwing_cand_ref *refs, int count,
                          char **buf, ssize_t *length, size_t *cap) {
    for (int i = 0; i < count; i++) {
        char placeholder[64], real_url[128];
        snprintf(placeholder, sizeof(placeholder), "%s%d#", placeholder_prefix, i);
        snprintf(real_url, sizeof(real_url), "menu?page=%d&amp;offset=%d",
                 refs[i].page, refs[i].offset);

        size_t find_len = strlen(placeholder);
        size_t repl_len = strlen(real_url);
        ssize_t cur_len = *length;
        char *p = *buf;

        while ((p = strstr(p, placeholder)) != NULL) {
            size_t needed = cur_len - find_len + repl_len + 1;
            if (needed > *cap) {
                size_t new_cap = *cap;
                while (new_cap < needed) new_cap *= 2;
                size_t offset = p - *buf;
                *buf = realloc(*buf, new_cap);
                if (!*buf) return;
                *cap = new_cap;
                p = *buf + offset;
            }
            size_t pos = p - *buf;
            size_t remaining = cur_len - pos - find_len;
            memmove(p + repl_len, p + find_len, remaining + 1);
            memcpy(p, real_url, repl_len);
            cur_len = cur_len - (ssize_t)find_len + (ssize_t)repl_len;
            p += repl_len;
        }
        *length = cur_len;
    }
}

static ssize_t epwing_read_text(ud_epwing *epwing, const EB_Position *position, char *buffer, size_t buffer_size) {
    epwing->cand_ref_count = 0;
    epwing->ref_ref_count = 0;
    if (eb_seek_text(&epwing->book, position) != EB_SUCCESS) return -1;
    ssize_t length;
    if (eb_read_text(&epwing->book, NULL, &epwing->hookset_text, epwing, buffer_size - 1, buffer, &length) != EB_SUCCESS) return -1;
    buffer[length] = '\0';
    return length;
}

// ============================================================
// Hook callbacks (HTML output for all text formatting)
// ============================================================

// --- Iconv helper ---

static void write_utf8(EB_Book *book, iconv_t cd, const char *src, size_t src_len) {
    if (src_len == 0) return;
    char outbuf[16];
    char *inp = (char *)src;
    char *outp = outbuf;
    size_t out_left = sizeof(outbuf);
    iconv(cd, &inp, &src_len, &outp, &out_left);
    size_t written = sizeof(outbuf) - out_left;
    if (written > 0) eb_write_text(book, outbuf, written);
}

// --- Character encoding hooks ---

static EB_Error_Code hook_iso8859_1(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    ud_epwing *epwing = container;
    char buf = (char)argv[0];
    write_utf8(book, epwing->cd_iso, &buf, 1);
    return EB_SUCCESS;
}

// --- Full-width to half-width conversion (NARROW_JISX0208) ---
//
// EPWING text uses BEGIN_NARROW/END_NARROW control codes to mark regions that
// should be displayed in half-width. Characters in these regions arrive via
// EB_HOOK_NARROW_JISX0208. Although encoded as full-width JIS X 0208, their
// semantic intent is half-width (narrow) display.
//
// For digits and letters the EUC-JP→ASCII mapping is arithmetic (row 0xA3:
// euc_code - 0xA380 = ASCII char). For punctuation and katakana the mapping
// is irregular (e.g. ガ → ｶﾞ is two characters), so a lookup table is needed.
// (Reference: qolibri data/euc-wide-to-utf-narrow)

static bool write_narrow(EB_Book *book, unsigned int code) {
    // Fast range checks: digits, uppercase, lowercase
    if (code >= 0xA3B0 && code <= 0xA3B9) {
        char c = '0' + (char)(code - 0xA3B0);
        eb_write_text(book, &c, 1);
        return true;
    }
    if (code >= 0xA3C1 && code <= 0xA3DA) {
        char c = 'A' + (char)(code - 0xA3C1);
        eb_write_text(book, &c, 1);
        return true;
    }
    if (code >= 0xA3E1 && code <= 0xA3FA) {
        char c = 'a' + (char)(code - 0xA3E1);
        eb_write_text(book, &c, 1);
        return true;
    }

    // Lookup table for punctuation, symbols, and katakana
    static const struct { unsigned int code; const char *str; } narrow_table[] = {
        // Punctuation
        { 0xA1A1, " " },   { 0xA1A2, "\xEF\xBD\xA4" }, { 0xA1A3, "\xEF\xBD\xA1" },
        { 0xA1A4, "," },   { 0xA1A5, "." },   { 0xA1A6, "\xEF\xBD\xA5" },
        { 0xA1A7, ":" },   { 0xA1A8, ";" },   { 0xA1A9, "?" },
        { 0xA1AA, "!" },   { 0xA1AB, "\xEF\xBE\x9E" }, { 0xA1AC, "\xEF\xBE\x9F" },
        { 0xA1B1, "\xC2\xAF" }, { 0xA1B2, "_" },  { 0xA1BF, "/" },
        { 0xA1C0, "\\" },  { 0xA1C1, "~" },   { 0xA1C3, "|" },
        { 0xA1CA, "(" },   { 0xA1CB, ")" },   { 0xA1CE, "[" },
        { 0xA1CF, "]" },   { 0xA1D0, "{" },   { 0xA1D1, "}" },
        { 0xA1D4, "\xE2\x9F\xAA" }, { 0xA1D5, "\xE2\x9F\xAB" },
        { 0xA1D6, "\xEF\xBD\xA2" }, { 0xA1D7, "\xEF\xBD\xA3" },
        { 0xA1DC, "+" },   { 0xA1DD, "-" },   { 0xA1E1, "=" },
        { 0xA1E3, "&lt;" },{ 0xA1E4, "&gt;" },{ 0xA1EF, "\xC2\xA5" },
        { 0xA1F0, "$" },   { 0xA1F3, "%" },   { 0xA1F4, "#" },
        { 0xA1F5, "&amp;" },{ 0xA1F6, "*" },  { 0xA1F7, "@" },
        { 0xA1F8, "\xC2\xA7" },
        // Katakana
        { 0xA5A1, "\xEF\xBD\xA7" }, { 0xA5A2, "\xEF\xBD\xB1" }, { 0xA5A3, "\xEF\xBD\xA8" },
        { 0xA5A4, "\xEF\xBD\xB2" }, { 0xA5A5, "\xEF\xBD\xA9" }, { 0xA5A6, "\xEF\xBD\xB3" },
        { 0xA5A7, "\xEF\xBD\xAA" }, { 0xA5A8, "\xEF\xBD\xB4" }, { 0xA5A9, "\xEF\xBD\xAB" },
        { 0xA5AA, "\xEF\xBD\xB5" }, { 0xA5AB, "\xEF\xBD\xB6" },
        { 0xA5AC, "\xEF\xBD\xB6\xEF\xBE\x9E" },
        { 0xA5AD, "\xEF\xBD\xB7" },
        { 0xA5AE, "\xEF\xBD\xB7\xEF\xBE\x9E" },
        { 0xA5AF, "\xEF\xBD\xB8" },
        { 0xA5B0, "\xEF\xBD\xB8\xEF\xBE\x9E" },
        { 0xA5B1, "\xEF\xBD\xB9" },
        { 0xA5B2, "\xEF\xBD\xB9\xEF\xBE\x9E" },
        { 0xA5B3, "\xEF\xBD\xBA" },
        { 0xA5B4, "\xEF\xBD\xBA\xEF\xBE\x9E" },
        { 0xA5B5, "\xEF\xBD\xBB" },
        { 0xA5B6, "\xEF\xBD\xBB\xEF\xBE\x9E" },
        { 0xA5B7, "\xEF\xBD\xBC" },
        { 0xA5B8, "\xEF\xBD\xBC\xEF\xBE\x9E" },
        { 0xA5B9, "\xEF\xBD\xBD" },
        { 0xA5BA, "\xEF\xBD\xBD\xEF\xBE\x9E" },
        { 0xA5BB, "\xEF\xBD\xBE" },
        { 0xA5BC, "\xEF\xBD\xBE\xEF\xBE\x9E" },
        { 0xA5BD, "\xEF\xBD\xBF" },
        { 0xA5BE, "\xEF\xBD\xBF\xEF\xBE\x9E" },
        { 0xA5BF, "\xEF\xBE\x80" },
        { 0xA5C0, "\xEF\xBE\x80\xEF\xBE\x9E" },
        { 0xA5C1, "\xEF\xBE\x81" },
        { 0xA5C2, "\xEF\xBE\x81\xEF\xBE\x9E" },
        { 0xA5C3, "\xEF\xBD\xAF" }, { 0xA5C4, "\xEF\xBE\x82" },
        { 0xA5C5, "\xEF\xBE\x82\xEF\xBE\x9E" },
        { 0xA5C6, "\xEF\xBE\x83" },
        { 0xA5C7, "\xEF\xBE\x83\xEF\xBE\x9E" },
        { 0xA5C8, "\xEF\xBE\x84" },
        { 0xA5C9, "\xEF\xBE\x84\xEF\xBE\x9E" },
        { 0xA5CA, "\xEF\xBE\x85" }, { 0xA5CB, "\xEF\xBE\x86" },
        { 0xA5CC, "\xEF\xBE\x87" }, { 0xA5CD, "\xEF\xBE\x88" },
        { 0xA5CE, "\xEF\xBE\x89" }, { 0xA5CF, "\xEF\xBE\x8A" },
        { 0xA5D0, "\xEF\xBE\x8A\xEF\xBE\x9E" },
        { 0xA5D1, "\xEF\xBE\x8A\xEF\xBE\x9F" },
        { 0xA5D2, "\xEF\xBE\x8B" },
        { 0xA5D3, "\xEF\xBE\x8B\xEF\xBE\x9E" },
        { 0xA5D4, "\xEF\xBE\x8B\xEF\xBE\x9F" },
        { 0xA5D5, "\xEF\xBE\x8C" },
        { 0xA5D6, "\xEF\xBE\x8C\xEF\xBE\x9E" },
        { 0xA5D7, "\xEF\xBE\x8C\xEF\xBE\x9F" },
        { 0xA5D8, "\xEF\xBE\x8D" },
        { 0xA5D9, "\xEF\xBE\x8D\xEF\xBE\x9E" },
        { 0xA5DA, "\xEF\xBE\x8D\xEF\xBE\x9F" },
        { 0xA5DB, "\xEF\xBE\x8E" },
        { 0xA5DC, "\xEF\xBE\x8E\xEF\xBE\x9E" },
        { 0xA5DD, "\xEF\xBE\x8E\xEF\xBE\x9F" },
        { 0xA5DE, "\xEF\xBE\x8F" }, { 0xA5DF, "\xEF\xBE\x90" },
        { 0xA5E0, "\xEF\xBE\x91" }, { 0xA5E1, "\xEF\xBE\x92" },
        { 0xA5E2, "\xEF\xBE\x93" },
        { 0xA5E3, "\xEF\xBD\xAC" }, { 0xA5E4, "\xEF\xBE\x94" },
        { 0xA5E5, "\xEF\xBD\xAD" }, { 0xA5E6, "\xEF\xBE\x95" },
        { 0xA5E7, "\xEF\xBD\xAE" }, { 0xA5E8, "\xEF\xBE\x96" },
        { 0xA5E9, "\xEF\xBE\x97" }, { 0xA5EA, "\xEF\xBE\x98" },
        { 0xA5EB, "\xEF\xBE\x99" }, { 0xA5EC, "\xEF\xBE\x9A" },
        { 0xA5ED, "\xEF\xBE\x9B" }, { 0xA5EF, "\xEF\xBE\x9C" },
        { 0xA5F2, "\xEF\xBD\xA6" }, { 0xA5F3, "\xEF\xBE\x9D" },
        { 0xA5F4, "\xEF\xBD\xB3\xEF\xBE\x9E" },
    };

    for (int i = 0; i < (int)(sizeof(narrow_table) / sizeof(narrow_table[0])); i++) {
        if (narrow_table[i].code == code) {
            eb_write_text_string(book, narrow_table[i].str);
            return true;
        }
    }
    return false;
}

static EB_Error_Code hook_narrow_jisx0208(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    if (write_narrow(book, argv[0])) return EB_SUCCESS;
    // Fallback: iconv EUC-JP → UTF-8
    ud_epwing *epwing = container;
    char buf[2] = { (char)(argv[0] >> 8), (char)(argv[0] & 0xFF) };
    write_utf8(book, epwing->cd_euc, buf, 2);
    return EB_SUCCESS;
}

static EB_Error_Code hook_wide_jisx0208(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    ud_epwing *epwing = container;
    char buf[2] = { (char)(argv[0] >> 8), (char)(argv[0] & 0xFF) };
    write_utf8(book, epwing->cd_euc, buf, 2);
    return EB_SUCCESS;
}

static EB_Error_Code hook_gb2312(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    ud_epwing *epwing = container;
    char buf[2] = { (char)(argv[0] >> 8), (char)(argv[0] & 0xFF) };
    write_utf8(book, epwing->cd_gb, buf, 2);
    return EB_SUCCESS;
}

// --- Newline ---
// ============================================================

// --- Newline ---

static EB_Error_Code hook_newline(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "<br>\n");
    return EB_SUCCESS;
}

// --- Subscript / Superscript ---

static EB_Error_Code hook_begin_subscript(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "<sub>");
    return EB_SUCCESS;
}

static EB_Error_Code hook_end_subscript(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "</sub>");
    return EB_SUCCESS;
}

static EB_Error_Code hook_begin_superscript(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "<sup>");
    return EB_SUCCESS;
}

static EB_Error_Code hook_end_superscript(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "</sup>");
    return EB_SUCCESS;
}

// --- Emphasis ---

static EB_Error_Code hook_begin_emphasis(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "<em>");
    return EB_SUCCESS;
}

static EB_Error_Code hook_end_emphasis(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "</em>");
    return EB_SUCCESS;
}

// --- No-newline (nowrap) ---

static EB_Error_Code hook_begin_no_newline(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "<span style=\"white-space:nowrap\">");
    return EB_SUCCESS;
}

static EB_Error_Code hook_end_no_newline(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "</span>");
    return EB_SUCCESS;
}

// --- Keyword ---

static EB_Error_Code hook_begin_keyword(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "<strong>");
    return EB_SUCCESS;
}

static EB_Error_Code hook_end_keyword(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "</strong>");
    return EB_SUCCESS;
}

// --- Cross-reference ---
// Uses ref_refs placeholder (#ref-url-N#) resolved to menu?page=X&offset=Y.

static EB_Error_Code hook_begin_reference(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    ud_epwing *epwing = container;
    int idx = epwing->ref_ref_count;
    if (idx >= epwing->ref_ref_capacity) {
        int new_cap = epwing->ref_ref_capacity ? epwing->ref_ref_capacity * 2 : 8;
        epwing_cand_ref *new_refs = realloc(epwing->ref_refs, new_cap * sizeof(epwing_cand_ref));
        if (!new_refs) return EB_SUCCESS;
        epwing->ref_refs = new_refs;
        epwing->ref_ref_capacity = new_cap;
    }
    epwing->ref_refs[idx].page = 0;
    epwing->ref_refs[idx].offset = 0;
    epwing->ref_ref_count++;
    char tag[128];
    snprintf(tag, sizeof(tag), "<a class=\"epwing-ref\" href=\"#ref-url-%d#\">", idx);
    eb_write_text_string(book, tag);
    return EB_SUCCESS;
}

static EB_Error_Code hook_end_reference(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    ud_epwing *epwing = container;
    int idx = epwing->ref_ref_count - 1;
    if (idx >= 0 && argc >= 3) {
        epwing->ref_refs[idx].page = (int)argv[1];
        epwing->ref_refs[idx].offset = (int)argv[2];
    }
    eb_write_text_string(book, "</a>");
    return EB_SUCCESS;
}

// --- Decoration ---

static EB_Error_Code hook_begin_decoration(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    switch (argv[1]) {
    case 0x01:
    case 0x1101: eb_write_text_string(book, "<i>"); break;
    case 0x03:
    case 0x1103: eb_write_text_string(book, "<b>"); break;
    default:     eb_write_text_string(book, "<span>"); break;
    }
    return EB_SUCCESS;
}

static EB_Error_Code hook_end_decoration(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "</span>");
    return EB_SUCCESS;
}

// --- Gaiji (custom characters) ---

static EB_Error_Code render_gaiji(EB_Book *book, void *container, int char_number, bool is_wide) {
    ud_epwing *epwing = container;

    if (!epwing || char_number < 0) return EB_SUCCESS;

    switch (epwing->gaiji_mode) {
    case UNIDICT_EPWING_GAIJI_BITMAP: {
        char url[64];
        snprintf(url, sizeof(url), "unidict://epwing/gaiji/%s/%d", is_wide ? "w" : "n", char_number);
        eb_write_text_string(book, "<img src=\"");
        eb_write_text_string(book, url);
        eb_write_text_string(book, "\">");
        break;
    }
    case UNIDICT_EPWING_GAIJI_ASCII_ART: {
        char bitmap[64];
        int width = is_wide ? 16 : 8;
        int height = 16;
        EB_Error_Code ec = is_wide
            ? eb_wide_font_character_bitmap(book, char_number, bitmap)
            : eb_narrow_font_character_bitmap(book, char_number, bitmap);
        if (ec != EB_SUCCESS) return EB_SUCCESS;
        eb_font_height(book, &height);
        int bpr = (width + 7) / 8;
        char row_buf[20];
        eb_write_text_string(book, "\n");
        for (int r = 0; r < height; r++) {
            int pos = 0;
            for (int c = 0; c < width; c++) {
                int byte_idx = r * bpr + (c / 8);
                int bit = (bitmap[byte_idx] >> (7 - (c % 8))) & 1;
                row_buf[pos++] = bit ? '#' : '.';
            }
            row_buf[pos++] = '\n';
            row_buf[pos] = '\0';
            eb_write_text_string(book, row_buf);
        }
        break;
    }
    default:
        // UNIDICT_EPWING_GAIJI_FALLBACK: output nothing
        break;
    }
    return EB_SUCCESS;
}

static EB_Error_Code hook_narrow_gaiji(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    return render_gaiji(book, container, (argc >= 1) ? (int)argv[0] : -1, false);
}

static EB_Error_Code hook_wide_gaiji(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    return render_gaiji(book, container, (argc >= 1) ? (int)argv[0] : -1, true);
}

// --- Mono graphic ---

static EB_Error_Code hook_begin_mono_graphic(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    char url[128];
    snprintf(url, sizeof(url), "unidict://epwing/g/m/%u/%u", argv[2], argv[3]);
    eb_write_text_string(book, "<img src=\"");
    eb_write_text_string(book, url);
    eb_write_text_string(book, "\">");
    return EB_SUCCESS;
}

// --- Color graphic (BMP / JPEG / inline variants) ---

static EB_Error_Code hook_begin_color_graphic(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    const char *type;
    switch (hook_code) {
    case EB_HOOK_BEGIN_COLOR_BMP:     type = "c";  break;
    case EB_HOOK_BEGIN_COLOR_JPEG:    type = "j";  break;
    case EB_HOOK_BEGIN_IN_COLOR_BMP:  type = "ic"; break;
    default:                          type = "ij"; break;
    }
    char url[128];
    snprintf(url, sizeof(url), "unidict://epwing/g/%s/%u/%u", type, argv[2], argv[3]);
    eb_write_text_string(book, "<img src=\"");
    eb_write_text_string(book, url);
    eb_write_text_string(book, "\">");
    return EB_SUCCESS;
}

// --- Graphic reference ---

static EB_Error_Code hook_graphic_reference(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    char url[128];
    snprintf(url, sizeof(url), "unidict://epwing/g/r/%u/%u", argv[1], argv[2]);
    eb_write_text_string(book, "<img src=\"");
    eb_write_text_string(book, url);
    eb_write_text_string(book, "\">");
    return EB_SUCCESS;
}

// --- Wave audio ---

static EB_Error_Code hook_begin_wave(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    char url[160];
    snprintf(url, sizeof(url), "unidict://epwing/w/%u/%u/%u/%u",
             argv[2], argv[3], argv[4], argv[5]);
    eb_write_text_string(book, "<audio src=\"");
    eb_write_text_string(book, url);
    eb_write_text_string(book, "\"></audio>");
    return EB_SUCCESS;
}

// --- MPEG video ---

static EB_Error_Code hook_begin_mpeg(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    char url[200];
    snprintf(url, sizeof(url), "unidict://epwing/v/%u/%u/%u/%u/%u",
             argv[1], argv[2], argv[3], argv[4], argv[5]);
    eb_write_text_string(book, "<video src=\"");
    eb_write_text_string(book, url);
    eb_write_text_string(book, "\"></video>");
    return EB_SUCCESS;
}

// --- Candidate (display mode: placeholder href, resolved by post-processing) ---

static EB_Error_Code hook_begin_candidate(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    ud_epwing *epwing = container;
    int idx = epwing->cand_ref_count;
    // Grow array if needed
    if (idx >= epwing->cand_ref_capacity) {
        int new_cap = epwing->cand_ref_capacity ? epwing->cand_ref_capacity * 2 : 8;
        epwing_cand_ref *new_refs = realloc(epwing->cand_refs, new_cap * sizeof(epwing_cand_ref));
        if (!new_refs) return EB_SUCCESS;
        epwing->cand_refs = new_refs;
        epwing->cand_ref_capacity = new_cap;
    }
    epwing->cand_refs[idx].page = 0;
    epwing->cand_refs[idx].offset = 0;
    epwing->cand_ref_count++;
    char tag[128];
    snprintf(tag, sizeof(tag), "<a class=\"epwing-cand\" href=\"#cand-url-%d#\">", idx);
    eb_write_text_string(book, tag);
    return EB_SUCCESS;
}

static EB_Error_Code hook_end_candidate_group(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    ud_epwing *epwing = container;
    int idx = epwing->cand_ref_count - 1;
    if (idx >= 0) {
        epwing->cand_refs[idx].page = (int)argv[1];
        epwing->cand_refs[idx].offset = (int)argv[2];
    }
    eb_write_text_string(book, "</a>");
    return EB_SUCCESS;
}

static EB_Error_Code hook_end_candidate_leaf(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    eb_write_text_string(book, "</a>");
    return EB_SUCCESS;
}

// --- Candidate collection hook (for recursive menu expansion, like qolibri's hookEndCandidateGroupMENU) ---

static void epwing_fullwidth_to_halfwidth(char *s) {
    char *p = s, *w = s;
    while (*p) {
        unsigned char b0 = (unsigned char)p[0];
        if ((b0 == 0xEF && (unsigned char)p[1] == 0xBC) ||
            (b0 == 0xEF && (unsigned char)p[1] == 0xBD)) {
            unsigned char b2 = (unsigned char)p[2];
            unsigned int cp = ((unsigned char)p[1] == 0xBC) ? (0xFF00 | (b2 & 0x3F)) : (0xFF40 | (b2 & 0x3F));
            if (cp >= 0xFF01 && cp <= 0xFF5E) {
                *w++ = (char)(cp - 0xFEE0);
                p += 3;
                continue;
            }
        }
        *w++ = *p++;
    }
    *w = '\0';
}

static char *epwing_euc_to_utf8(ud_epwing *epwing, const char *src) {
    if (!src || !*src) return strdup("");
    size_t src_len = strlen(src);
    size_t out_size = src_len * 3 + 1;
    char *out = malloc(out_size);
    if (!out) return strdup("");
    char *inp = (char *)src;
    char *outp = out;
    size_t out_left = out_size - 1;
    iconv(epwing->cd_euc, &inp, &src_len, &outp, &out_left);
    *outp = '\0';
    epwing_fullwidth_to_halfwidth(out);
    return out;
}

static EB_Error_Code hook_end_candidate_group_menu(EB_Book *book, EB_Appendix *appendix,
    void *container, EB_Hook_Code hook_code, int argc, const unsigned int *argv) {
    ud_epwing *epwing = container;
    const char *cand = eb_current_candidate(book);
    int idx = epwing->menu_item_count;
    if (idx >= epwing->menu_item_capacity) {
        int new_cap = epwing->menu_item_capacity ? epwing->menu_item_capacity * 2 : 16;
        epwing_menu_item *new_items = realloc(epwing->menu_items, new_cap * sizeof(epwing_menu_item));
        if (!new_items) return EB_SUCCESS;
        epwing->menu_items = new_items;
        epwing->menu_item_capacity = new_cap;
    }
    epwing->menu_items[idx].title = epwing_euc_to_utf8(epwing, cand);
    epwing->menu_items[idx].page = (int)argv[1];
    epwing->menu_items[idx].offset = (int)argv[2];
    epwing->menu_item_count++;
    return EB_SUCCESS;
}

// ============================================================
// Forward declarations
// ============================================================

static unidict_status epwing_info_get(unidict *dict, unidict_info **out_info);
static unidict_status epwing_file_infos_get(unidict *dict, unidict_file_info_array **out_infos);
static unidict_status epwing_lookup(unidict *dict, const char *key, unidict_article_array **out_articles);
static unidict_status epwing_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries);
static unidict_status epwing_suggest(unidict *dict, const char *prefix, size_t limit,
                                     unidict_entry_array **out_entries);
static unidict_status epwing_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles);
static unidict_status epwing_index_activate(unidict *dict, unidict_index_type index_type);
static unidict_status epwing_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                 void *user_data);
static unidict_status epwing_index_external_delete(unidict *dict);
static unidict_entry_iter *epwing_entry_iter_create(unidict *dict);
static unidict_status epwing_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry);
static void epwing_entry_iter_free(unidict_entry_iter *iter);
static unidict_status epwing_resource_get(unidict *dict, const char *key, unidict_resource **out_res);

// ============================================================
// Feature Pages
// ============================================================

#define EPWING_RES_PREFIX "unidict://epwing/"

static char *epwing_read_text_html(ud_epwing *epwing, const EB_Position *pos) {
    size_t cap = 65536;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    ssize_t len = epwing_read_text(epwing, pos, buf, cap);
    if (len < 0) { free(buf); return NULL; }
    resolve_refs("#cand-url-", epwing->cand_refs, epwing->cand_ref_count, &buf, &len, &cap);
    resolve_refs("#ref-url-", epwing->ref_refs, epwing->ref_ref_count, &buf, &len, &cap);
    epwing->cand_ref_count = 0;
    epwing->ref_ref_count = 0;
    return buf;
}

// Read text at position using hookset_menu (collection mode):
// returns HTML text in buffer AND collects candidate items.
static ssize_t epwing_read_menu_text(ud_epwing *epwing, const EB_Position *pos,
                                      char *buffer, size_t buffer_size) {
    epwing->menu_item_count = 0;
    epwing->cand_ref_count = 0;
    epwing->ref_ref_count = 0;
    if (eb_seek_text(&epwing->book, pos) != EB_SUCCESS) return -1;
    ssize_t length;
    if (eb_read_text(&epwing->book, NULL, &epwing->hookset_menu, epwing,
                     buffer_size - 1, buffer, &length) != EB_SUCCESS) return -1;
    buffer[length] = '\0';
    return length;
}

#define MENU_APPEND(...) do { \
    int _n = snprintf(*buf + *len, *cap - *len, __VA_ARGS__); \
    if (_n > 0 && (size_t)_n >= *cap - *len) { \
        *cap *= 2; \
        *buf = realloc(*buf, *cap); \
        _n = snprintf(*buf + *len, *cap - *len, __VA_ARGS__); \
    } \
    if (_n > 0) *len += _n; \
} while(0)

// Recursively build full menu HTML (like qolibri's getMenus + composeHLine).
// Reads text at pos using hookset_menu, collects candidates.
// If candidates found: outputs <hN> headings and recurses into each.
// If no candidates (leaf): outputs text content wrapped in <pre>.
static void epwing_build_full_menu(ud_epwing *epwing, const EB_Position *pos,
                                    int depth, char **buf, size_t *len, size_t *cap) {
    char text_buf[65536];
    ssize_t text_len = epwing_read_menu_text(epwing, pos, text_buf, sizeof(text_buf));

    int item_count = epwing->menu_item_count;
    epwing_menu_item *items = epwing->menu_items;
    epwing->menu_item_count = 0;
    epwing->menu_items = NULL;
    epwing->menu_item_capacity = 0;

    if (item_count > 0) {
        for (int i = 0; i < item_count; i++) {
            int h = (depth < 6) ? depth : 6;
            if (items[i].page > 0 || items[i].offset > 0) {
                MENU_APPEND("<h%d>%s</h%d>\n", h, items[i].title, h);
                EB_Position sub_pos = { .page = items[i].page, .offset = items[i].offset };
                epwing_build_full_menu(epwing, &sub_pos, depth + 1, buf, len, cap);
            } else {
                MENU_APPEND("<h%d>%s</h%d>\n", h, items[i].title, h);
            }
            free(items[i].title);
        }
        free(items);
    } else {
        free(items);
        if (text_len > 0) {
            const char *text = text_buf;
            if (depth > 1) {
                const char *nl = strchr(text_buf, '\n');
                if (nl) text = nl + 1;
            }
            if (*text) {
                // Resolve ref/cand placeholders in a dynamic buffer, then append
                size_t tcap = (size_t)text_len * 2 + 256;
                char *tbuf = malloc(tcap);
                if (tbuf) {
                    ssize_t tlen = (ssize_t)strlen(text);
                    memcpy(tbuf, text, tlen + 1);
                    resolve_refs("#cand-url-", epwing->cand_refs, epwing->cand_ref_count, &tbuf, &tlen, &tcap);
                    resolve_refs("#ref-url-", epwing->ref_refs, epwing->ref_ref_count, &tbuf, &tlen, &tcap);
                    MENU_APPEND("<pre>%s</pre>\n", tbuf);
                    free(tbuf);
                } else {
                    MENU_APPEND("<pre>%s</pre>\n", text);
                }
                epwing->cand_ref_count = 0;
                epwing->ref_ref_count = 0;
            }
        }
    }
}

#undef MENU_APPEND

static unidict_status epwing_feature_pages_list(unidict *dict, unidict_feature_page_array **out_pages) {
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    if (!epwing->subbook_set) return UNIDICT_ERR_NOT_SUPPORTED;

    unidict_feature_page *items = calloc(2, sizeof(*items));
    if (!items) return UNIDICT_ERR_NOMEM;

    int count = 0;
    items[count].key = strdup("info");
    items[count].name = strdup("Book Information");
    count++;
    if (eb_have_menu(&epwing->book)) {
        items[count].key = strdup("menu");
        items[count].name = strdup("Menu");
        count++;
    }

    unidict_feature_page_array *arr = calloc(1, sizeof(*arr));
    if (!arr) {
        for (int i = 0; i < count; i++) { free(items[i].key); free(items[i].name); }
        free(items);
        return UNIDICT_ERR_NOMEM;
    }
    arr->items = items;
    arr->count = count;
    *out_pages = arr;
    return UNIDICT_OK;
}

static char *epwing_build_info_page(ud_epwing *epwing) {
    size_t cap = 8192;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

#define APPEND(...) do { \
    int _n = snprintf(buf + len, cap - len, __VA_ARGS__); \
    if (_n > 0 && (size_t)_n < cap - len) len += _n; \
} while(0)

    APPEND("<table>");

    // Title — reuse cached title if available, otherwise convert now
    if (epwing->title) {
        APPEND("<tr><td>Title</td><td>%s</td></tr>", epwing->title);
    }

    // Search methods
    APPEND("<tr><td>Search Methods</td><td>");
    {
        bool first = true;
        if (eb_have_text(&epwing->book)) { APPEND("%sText", first ? "" : ", "); first = false; }
        if (eb_have_word_search(&epwing->book)) { APPEND("%sForward", first ? "" : ", "); first = false; }
        if (eb_have_endword_search(&epwing->book)) { APPEND("%sBackward", first ? "" : ", "); first = false; }
        if (eb_have_keyword_search(&epwing->book)) { APPEND("%sKeyword", first ? "" : ", "); first = false; }
        if (eb_have_cross_search(&epwing->book)) { APPEND("%sCross", first ? "" : ", "); first = false; }
    }
    APPEND("</td></tr>");

    // Character code
    {
        EB_Character_Code ccode;
        if (eb_character_code(&epwing->book, &ccode) == EB_SUCCESS) {
            const char *cc_str = (ccode == EB_CHARCODE_ISO8859_1) ? "ISO 8859-1"
                               : (ccode == EB_CHARCODE_JISX0208_GB2312) ? "JIS X 0208 + GB 2312"
                               : "JIS X 0208";
            APPEND("<tr><td>Character Code</td><td>%s</td></tr>", cc_str);
        }
    }

    APPEND("</table>");

    // Appendix
    {
        char apath[EB_MAX_PATH_LENGTH + 1];
        if (eb_appendix_path(&epwing->appendix, apath) == EB_SUCCESS) {
            char adir[EB_MAX_DIRECTORY_NAME_LENGTH + 1];
            eb_appendix_subbook_directory(&epwing->appendix, adir);

            APPEND("<h3>Appendix</h3>");
            APPEND("<table>");
            APPEND("<tr><td>Path</td><td>%s/%s</td></tr>", apath, adir);

            int sc;
            if (eb_stop_code(&epwing->appendix, &sc) == EB_SUCCESS) {
                APPEND("<tr><td>Stop Code</td><td>0x%X</td></tr>", sc);
            }
            if (eb_have_narrow_alt(&epwing->appendix)) {
                int start, end;
                eb_narrow_alt_start(&epwing->appendix, &start);
                eb_narrow_alt_end(&epwing->appendix, &end);
                APPEND("<tr><td>Narrow Alt Font</td><td>%d - %d</td></tr>", start, end);
            }
            if (eb_have_wide_alt(&epwing->appendix)) {
                int start, end;
                eb_wide_alt_start(&epwing->appendix, &start);
                eb_wide_alt_end(&epwing->appendix, &end);
                APPEND("<tr><td>Wide Alt Font</td><td>%d - %d</td></tr>", start, end);
            }
            APPEND("</table>");
        }
    }

    // Copyright
    if (eb_have_copyright(&epwing->book)) {
        EB_Position pos;
        if (eb_copyright(&epwing->book, &pos) == EB_SUCCESS) {
            char *text = epwing_read_text_html(epwing, &pos);
            if (text) {
                APPEND("<h3>Copyright</h3><pre>%s</pre>", text);
                free(text);
            }
        }
    }

    // README / text files (READM*, *.TXT, *.HTM, *.HTML, COPYRIGHT, VERSION, PREFACE)
    {
        DIR *dir = opendir(epwing->path);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                size_t nlen = strlen(ent->d_name);

                // Check if filename matches any of the patterns
                bool match = false;
                if (strncasecmp(ent->d_name, "READM", 5) == 0) match = true;
                if (!match && nlen >= 4 && strcasecmp(ent->d_name + nlen - 4, ".TXT") == 0) match = true;
                if (!match && nlen >= 5 && strcasecmp(ent->d_name + nlen - 5, ".HTML") == 0) match = true;
                if (!match && nlen >= 4 && strcasecmp(ent->d_name + nlen - 4, ".HTM") == 0) match = true;
                if (!match && strcasecmp(ent->d_name, "COPYRIGHT") == 0) match = true;
                if (!match && strcasecmp(ent->d_name, "VERSION") == 0) match = true;
                if (!match && strcasecmp(ent->d_name, "PREFACE") == 0) match = true;
                if (!match) continue;

                char full_path[PATH_MAX];
                snprintf(full_path, sizeof(full_path), "%s/%s", epwing->path, ent->d_name);
                struct stat st;
                if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

                FILE *f = fopen(full_path, "rb");
                if (!f) continue;

                char *content = malloc(st.st_size + 1);
                if (!content) { fclose(f); continue; }
                size_t nread = fread(content, 1, st.st_size, f);
                fclose(f);

                // Convert Shift-JIS → UTF-8 (like qolibri)
                char *decoded;
                if (epwing->cd_sjis != (iconv_t)-1 && nread > 0) {
                    size_t out_size = nread * 3 + 1;
                    decoded = malloc(out_size);
                    if (decoded) {
                        char *inp = content;
                        char *outp = decoded;
                        size_t in_left = nread;
                        size_t out_left = out_size - 1;
                        iconv(epwing->cd_sjis, &inp, &in_left, &outp, &out_left);
                        *outp = '\0';
                        free(content);
                    } else {
                        decoded = content;
                        decoded[nread] = '\0';
                    }
                } else {
                    decoded = content;
                    decoded[nread] = '\0';
                }

                // Strip CR
                char *w = decoded;
                for (char *r = decoded; *r; r++) {
                    if (*r != '\r') *w++ = *r;
                }
                *w = '\0';

                bool is_html = (nlen >= 5 && strcasecmp(ent->d_name + nlen - 5, ".HTML") == 0) ||
                               (nlen >= 4 && strcasecmp(ent->d_name + nlen - 4, ".HTM") == 0);
                if (is_html) {
                    APPEND("<h3>%s</h3>%s", ent->d_name, decoded);
                } else {
                    APPEND("<h3>%s</h3><pre>%s</pre>", ent->d_name, decoded);
                }
                free(decoded);
            }
            closedir(dir);
        }
    }

#undef APPEND
    return buf;
}

static unidict_status epwing_feature_page_read(unidict *dict, const char *key, char **out_html) {
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    if (!epwing->subbook_set) return UNIDICT_ERR_NOT_SUPPORTED;

    // Split base key and query params: "menu?page=123&offset=456"
    const char *query = strchr(key, '?');
    size_t base_len = query ? (size_t)(query - key) : strlen(key);

    if (base_len == 4 && strncmp(key, "menu", 4) == 0) {
        if (query) {
            // Sub-page: "menu?page=123&offset=456"
            int page = -1, offset = -1;
            const char *p = query + 1;
            while (*p) {
                if (strncmp(p, "page=", 5) == 0) {
                    page = (int)strtol(p + 5, (char **)&p, 10);
                } else if (strncmp(p, "offset=", 7) == 0) {
                    offset = (int)strtol(p + 7, (char **)&p, 10);
                } else {
                    p = strchr(p, '&');
                    if (!p) break;
                }
                if (*p == '&') p++;
            }
            if (page < 0 || offset < 0) return UNIDICT_ERR_NOT_FOUND;
            EB_Position pos = { .page = page, .offset = offset };
            char *html = epwing_read_text_html(epwing, &pos);
            if (!html) return UNIDICT_ERR_IO;
            *out_html = html;
            return UNIDICT_OK;
        }
        // Top-level menu: recursively expand entire menu tree
        if (!eb_have_menu(&epwing->book)) return UNIDICT_ERR_NOT_FOUND;
        EB_Position pos;
        if (eb_menu(&epwing->book, &pos) != EB_SUCCESS) return UNIDICT_ERR_NOT_FOUND;
        size_t html_cap = 65536;
        size_t html_len = 0;
        char *html = malloc(html_cap);
        if (!html) return UNIDICT_ERR_NOMEM;
        epwing_build_full_menu(epwing, &pos, 1, &html, &html_len, &html_cap);
        if (html_len == 0) { free(html); return UNIDICT_ERR_NOT_FOUND; }
        html[html_len] = '\0';
        *out_html = html;
        return UNIDICT_OK;
    }
    if (base_len == 4 && strncmp(key, "info", 4) == 0 && !query) {
        char *html = epwing_build_info_page(epwing);
        if (!html) return UNIDICT_ERR_IO;
        *out_html = html;
        return UNIDICT_OK;
    }

    return UNIDICT_ERR_NOT_FOUND;
}

// ============================================================
// Virtual function table
// ============================================================

static const unidict_ops epwing_ops = {
    .prepare = NULL,
    .info_get = epwing_info_get,
    .file_infos_get = epwing_file_infos_get,
    .index_activate = epwing_index_activate,
    .index_external_make = epwing_index_external_make,
    .index_external_delete = epwing_index_external_delete,
    .lookup = epwing_lookup,
    .entry_lookup = epwing_entry_lookup,
    .suggest = epwing_suggest,
    .fetch = epwing_fetch,
    .entry_iter_create = epwing_entry_iter_create,
    .entry_iter_next = epwing_entry_iter_next,
    .entry_iter_free = epwing_entry_iter_free,
    .resource_get = epwing_resource_get,
    .resource_iter_create = NULL,
    .resource_iter_next = NULL,
    .resource_iter_free = NULL,
    .feature_pages_list = epwing_feature_pages_list,
    .feature_page_read = epwing_feature_page_read,
};

// ============================================================
// Release
// ============================================================

static void ud_epwing_release(uobject *obj) {
    if (!obj) return;
    ud_epwing *epwing = uobject_cast(obj, ud_epwing, base.obj);

    if (epwing->udx_dict) {
        unidict_close(epwing->udx_dict);
        epwing->udx_dict = NULL;
    }

    free(epwing->title);
    eb_finalize_book(&epwing->book);
    eb_finalize_appendix(&epwing->appendix);
    eb_finalize_hookset(&epwing->hookset_text);
    eb_finalize_hookset(&epwing->hookset_heading);
    eb_finalize_hookset(&epwing->hookset_menu);
    if (epwing->cd_euc != (iconv_t)-1) iconv_close(epwing->cd_euc);
    if (epwing->cd_iso != (iconv_t)-1) iconv_close(epwing->cd_iso);
    if (epwing->cd_gb != (iconv_t)-1) iconv_close(epwing->cd_gb);
    if (epwing->cd_sjis != (iconv_t)-1) iconv_close(epwing->cd_sjis);
    free(epwing->cand_refs);
    free(epwing->ref_refs);
    if (epwing->menu_items) {
        for (int i = 0; i < epwing->menu_item_count; i++) free(epwing->menu_items[i].title);
        free(epwing->menu_items);
    }
    free(epwing->path);
    free(epwing);
}

// ============================================================
// Constructor
// ============================================================

unidict *ud_epwing_open(const char *path, const unidict_open_options *options) {
    if (!path) return NULL;

    int sub_idx = (options) ? options->subitem_index : 0;
    unidict_epwing_gaiji_mode gaiji_mode = (options) ? options->epwing_gaiji_mode : UNIDICT_EPWING_GAIJI_BITMAP;

    eb_initialize_library();

    ud_epwing *epwing = calloc(1, sizeof(ud_epwing));
    if (!epwing) return NULL;

    uobject_init(&epwing->base.obj, &ud_epwing_type, NULL);
    epwing->base.ops = &epwing_ops;
    epwing->base.format = UNIDICT_FORMAT_EPWING;
    epwing->base.has_builtin_index = true;
    epwing->gaiji_mode = gaiji_mode;
    epwing->path = strdup(path);

    eb_initialize_book(&epwing->book);
    eb_initialize_appendix(&epwing->appendix);
    eb_initialize_hookset(&epwing->hookset_text);
    eb_initialize_hookset(&epwing->hookset_heading);
    eb_initialize_hookset(&epwing->hookset_menu);

    epwing->cd_euc = iconv_open("UTF-8", "EUC-JP");
    epwing->cd_iso = iconv_open("UTF-8", "ISO-8859-1");
    epwing->cd_gb = iconv_open("UTF-8", "GB2312");
    epwing->cd_sjis = iconv_open("UTF-8", "SHIFT-JIS");

    // Register text hooks
    EB_Hook text_hooks[] = {
        // Character encoding
        { EB_HOOK_ISO8859_1,               hook_iso8859_1 },
        { EB_HOOK_NARROW_JISX0208,         hook_narrow_jisx0208 },
        { EB_HOOK_WIDE_JISX0208,           hook_wide_jisx0208 },
        { EB_HOOK_GB2312,                  hook_gb2312 },
        // Text formatting
        { EB_HOOK_NEWLINE,                 hook_newline },
        { EB_HOOK_BEGIN_SUBSCRIPT,         hook_begin_subscript },
        { EB_HOOK_END_SUBSCRIPT,           hook_end_subscript },
        { EB_HOOK_BEGIN_SUPERSCRIPT,       hook_begin_superscript },
        { EB_HOOK_END_SUPERSCRIPT,         hook_end_superscript },
        { EB_HOOK_BEGIN_EMPHASIS,          hook_begin_emphasis },
        { EB_HOOK_END_EMPHASIS,            hook_end_emphasis },
        { EB_HOOK_BEGIN_NO_NEWLINE,        hook_begin_no_newline },
        { EB_HOOK_END_NO_NEWLINE,          hook_end_no_newline },
        { EB_HOOK_BEGIN_KEYWORD,           hook_begin_keyword },
        { EB_HOOK_END_KEYWORD,             hook_end_keyword },
        { EB_HOOK_BEGIN_REFERENCE,         hook_begin_reference },
        { EB_HOOK_END_REFERENCE,           hook_end_reference },
        { EB_HOOK_BEGIN_DECORATION,        hook_begin_decoration },
        { EB_HOOK_END_DECORATION,          hook_end_decoration },
        // Images
        { EB_HOOK_BEGIN_MONO_GRAPHIC,      hook_begin_mono_graphic },
        { EB_HOOK_BEGIN_COLOR_BMP,         hook_begin_color_graphic },
        { EB_HOOK_BEGIN_COLOR_JPEG,        hook_begin_color_graphic },
        { EB_HOOK_BEGIN_IN_COLOR_BMP,      hook_begin_color_graphic },
        { EB_HOOK_BEGIN_IN_COLOR_JPEG,     hook_begin_color_graphic },
        { EB_HOOK_BEGIN_GRAPHIC_REFERENCE, hook_graphic_reference },
        { EB_HOOK_GRAPHIC_REFERENCE,       hook_graphic_reference },
        // Audio
        { EB_HOOK_BEGIN_WAVE,              hook_begin_wave },
        // Video
        { EB_HOOK_BEGIN_MPEG,              hook_begin_mpeg },
        // Candidate
        { EB_HOOK_BEGIN_CANDIDATE,         hook_begin_candidate },
        { EB_HOOK_END_CANDIDATE_GROUP,     hook_end_candidate_group },
        { EB_HOOK_END_CANDIDATE_LEAF,      hook_end_candidate_leaf },
        // Gaiji
        { EB_HOOK_NARROW_FONT,             hook_narrow_gaiji },
        { EB_HOOK_WIDE_FONT,               hook_wide_gaiji },
        { EB_HOOK_NULL,                    NULL },
    };
    eb_set_hooks(&epwing->hookset_text, text_hooks);

    // Heading hookset: only encoding conversion, no HTML/gaiji output
    EB_Hook heading_hooks[] = {
        { EB_HOOK_ISO8859_1,       hook_iso8859_1 },
        { EB_HOOK_NARROW_JISX0208, hook_narrow_jisx0208 },
        { EB_HOOK_WIDE_JISX0208,   hook_wide_jisx0208 },
        { EB_HOOK_GB2312,          hook_gb2312 },
        { EB_HOOK_NULL,            NULL },
    };
    eb_set_hooks(&epwing->hookset_heading, heading_hooks);

    // Menu hookset: full HTML formatting + candidate collection (like qolibri's hooks_cand).
    // BEGIN_CANDIDATE uses default (no HTML output). END_CANDIDATE_GROUP collects items.
    // All other formatting hooks produce HTML for leaf-node text content.
    EB_Hook menu_hooks[] = {
        { EB_HOOK_ISO8859_1,               hook_iso8859_1 },
        { EB_HOOK_NARROW_JISX0208,         hook_narrow_jisx0208 },
        { EB_HOOK_WIDE_JISX0208,           hook_wide_jisx0208 },
        { EB_HOOK_GB2312,                  hook_gb2312 },
        { EB_HOOK_NEWLINE,                 hook_newline },
        { EB_HOOK_BEGIN_SUBSCRIPT,         hook_begin_subscript },
        { EB_HOOK_END_SUBSCRIPT,           hook_end_subscript },
        { EB_HOOK_BEGIN_SUPERSCRIPT,       hook_begin_superscript },
        { EB_HOOK_END_SUPERSCRIPT,         hook_end_superscript },
        { EB_HOOK_BEGIN_EMPHASIS,          hook_begin_emphasis },
        { EB_HOOK_END_EMPHASIS,            hook_end_emphasis },
        { EB_HOOK_BEGIN_NO_NEWLINE,        hook_begin_no_newline },
        { EB_HOOK_END_NO_NEWLINE,          hook_end_no_newline },
        { EB_HOOK_BEGIN_KEYWORD,           hook_begin_keyword },
        { EB_HOOK_END_KEYWORD,             hook_end_keyword },
        { EB_HOOK_BEGIN_REFERENCE,         hook_begin_reference },
        { EB_HOOK_END_REFERENCE,           hook_end_reference },
        { EB_HOOK_BEGIN_DECORATION,        hook_begin_decoration },
        { EB_HOOK_END_DECORATION,          hook_end_decoration },
        { EB_HOOK_BEGIN_MONO_GRAPHIC,      hook_begin_mono_graphic },
        { EB_HOOK_BEGIN_COLOR_BMP,         hook_begin_color_graphic },
        { EB_HOOK_BEGIN_COLOR_JPEG,        hook_begin_color_graphic },
        { EB_HOOK_BEGIN_IN_COLOR_BMP,      hook_begin_color_graphic },
        { EB_HOOK_BEGIN_IN_COLOR_JPEG,     hook_begin_color_graphic },
        { EB_HOOK_BEGIN_GRAPHIC_REFERENCE, hook_graphic_reference },
        { EB_HOOK_GRAPHIC_REFERENCE,       hook_graphic_reference },
        { EB_HOOK_BEGIN_WAVE,              hook_begin_wave },
        { EB_HOOK_BEGIN_MPEG,              hook_begin_mpeg },
        { EB_HOOK_END_CANDIDATE_GROUP,     hook_end_candidate_group_menu },
        { EB_HOOK_NARROW_FONT,             hook_narrow_gaiji },
        { EB_HOOK_WIDE_FONT,               hook_wide_gaiji },
        { EB_HOOK_NULL,                    NULL },
    };
    eb_set_hooks(&epwing->hookset_menu, menu_hooks);

    if (eb_bind(&epwing->book, path) != EB_SUCCESS) {
        ud_epwing_release(&epwing->base.obj);
        return NULL;
    }

    eb_bind_appendix(&epwing->appendix, path);

    if (eb_load_all_subbooks(&epwing->book) != EB_SUCCESS) {
        ud_epwing_release(&epwing->base.obj);
        return NULL;
    }

    epwing->subbook_count = epwing->book.subbook_count;

    // Activate the requested subbook
    if (epwing->subbook_count > 0) {
        if (sub_idx >= epwing->subbook_count) sub_idx = 0;

        EB_Subbook_Code *codes = calloc(epwing->subbook_count, sizeof(EB_Subbook_Code));
        if (codes) {
            int count;
            eb_subbook_list(&epwing->book, codes, &count);
            eb_set_subbook(&epwing->book, codes[sub_idx]);
            epwing->current_subbook = codes[sub_idx];
            epwing->subbook_set = true;

            eb_set_appendix_subbook(&epwing->appendix, codes[sub_idx]);

            // Set font for gaiji rendering (prefer smallest available)
            static const EB_Font_Code font_prefs[] = { EB_FONT_16, EB_FONT_24, EB_FONT_30, EB_FONT_48 };
            for (int fi = 0; fi < 4; fi++) {
                if (eb_have_font(&epwing->book, font_prefs[fi])) {
                    eb_set_font(&epwing->book, font_prefs[fi]);
                    break;
                }
            }

            char title[EB_MAX_TITLE_LENGTH + 1];
            if (eb_subbook_title(&epwing->book, title) == EB_SUCCESS) {
                // Convert EUC-JP title to UTF-8
                size_t title_len = strlen(title);
                size_t out_size = title_len * 2 + 1;
                char *utf8_title = malloc(out_size);
                if (utf8_title) {
                    char *inp = title;
                    char *outp = utf8_title;
                    size_t out_left = out_size - 1;
                    iconv(epwing->cd_euc, &inp, &title_len, &outp, &out_left);
                    *outp = '\0';
                    epwing_fullwidth_to_halfwidth(utf8_title);
                    epwing->title = utf8_title;
                } else {
                    epwing->title = strdup(title);
                }
            }
            free(codes);
        }
    }

    return &epwing->base;
}

// ============================================================
// Info
// ============================================================

static unidict_status epwing_info_get(unidict *dict, unidict_info **out_info) {
    if (!dict || !out_info) {
        if (out_info) *out_info = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_info = NULL;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);

    // External index mode: delegate to UDX info_get
    if (epwing->udx_dict && epwing->udx_dict->ops->info_get) {
        unidict_status st = epwing->udx_dict->ops->info_get(epwing->udx_dict, out_info);
        if (st == UNIDICT_OK && *out_info) {
            (*out_info)->format = dict->format;
            (*out_info)->subitem_count = epwing->subbook_count;
        }
        return st;
    }

    unidict_info *res = calloc(1, sizeof(unidict_info));
    if (!res) return UNIDICT_ERR_NOMEM;

    res->format = dict->format;
    res->title = epwing->title ? strdup(epwing->title) : NULL;
    res->subitem_count = epwing->subbook_count;

    *out_info = res;
    return UNIDICT_OK;
}

// ============================================================
// File list
// ============================================================

static unidict_status epwing_file_infos_get(unidict *dict, unidict_file_info_array **out_infos) {
    if (!dict || !out_infos) {
        if (out_infos) *out_infos = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_infos = NULL;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    if (!epwing->path) return UNIDICT_ERR_INVALID_PARAM;

    DIR *dir = opendir(epwing->path);
    if (!dir) return UNIDICT_ERR_IO;

    size_t capacity = 32;
    char **paths = malloc(capacity * sizeof(char *));
    if (!paths) {
        closedir(dir);
        return UNIDICT_ERR_NOMEM;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", epwing->path, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if ((size_t)count >= capacity) {
            capacity *= 2;
            char **new_paths = realloc(paths, capacity * sizeof(char *));
            if (!new_paths) break;
            paths = new_paths;
        }

        paths[count] = strdup(full_path);
        if (paths[count]) count++;
    }

    closedir(dir);

    if (count == 0) {
        free(paths);
        return UNIDICT_OK;
    }

    *out_infos = unidict_file_infos_from_paths((const char **)paths, count);

    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);

    return UNIDICT_OK;
}

// ============================================================
// UDX path helper
// ============================================================

static char *epwing_get_udx_path(const char *dir_path) {
    size_t len = strlen(dir_path);
    // Remove trailing slash
    while (len > 1 && dir_path[len - 1] == '/') len--;
    char *udx_path = malloc(len + 5);
    if (!udx_path) return NULL;
    snprintf(udx_path, len + 5, "%.*s.udx", (int)len, dir_path);
    return udx_path;
}

// Pack/unpack EB_Positions from UDX value (heading_page + heading_offset + text_page + text_offset = 16 bytes)

static void epwing_pack_positions(const EB_Position *heading, const EB_Position *text, uint8_t out[16]) {
    memcpy(out, &heading->page, 4);
    memcpy(out + 4, &heading->offset, 4);
    memcpy(out + 8, &text->page, 4);
    memcpy(out + 12, &text->offset, 4);
}

static void epwing_unpack_positions(const uint8_t *data, size_t size, EB_Position *heading, EB_Position *text) {
    if (size < 16) {
        memset(heading, 0, sizeof(*heading));
        memset(text, 0, sizeof(*text));
        return;
    }
    memcpy(&heading->page, data, 4);
    memcpy(&heading->offset, data + 4, 4);
    memcpy(&text->page, data + 8, 4);
    memcpy(&text->offset, data + 12, 4);
}

// Helper: extract EB_Positions from UDX value entry item
static ud_epwing_entry_ref *epwing_ref_from_value_item(const udx_value_entry_item *item) {
    if (!item || !item->data || item->size < 16) return NULL;
    ud_epwing_entry_ref *ref = calloc(1, sizeof(ud_epwing_entry_ref));
    if (ref) {
        uobject_init(&ref->obj, &ud_epwing_entry_ref_type, NULL);
        epwing_unpack_positions(item->data, item->size, &ref->hit.heading, &ref->hit.text);
    }
    return ref;
}

// ============================================================
// Index activate
// ============================================================

static unidict_status epwing_index_activate(unidict *dict, unidict_index_type index_type) {
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);

    if (epwing->udx_dict) {
        unidict_close(epwing->udx_dict);
        epwing->udx_dict = NULL;
    }
    dict->active_index = UNIDICT_INDEX_BUILTIN;

    if (index_type == UNIDICT_INDEX_EXTERNAL || index_type == UNIDICT_INDEX_NONE) {
        char *udx_path = epwing_get_udx_path(epwing->path);
        if (udx_path) {
            unidict *udx_dict = ud_udx_open(udx_path, NULL);
            free(udx_path);

            if (udx_dict) {
                epwing->udx_dict = udx_dict;
                dict->active_index = UNIDICT_INDEX_EXTERNAL;
                return UNIDICT_OK;
            }
        }
        if (index_type == UNIDICT_INDEX_EXTERNAL) return UNIDICT_ERR_IO;
    }

    return UNIDICT_OK;
}

// ============================================================
// Index external make
// ============================================================

static unidict_status epwing_index_external_make(unidict *dict, unidict_index_external_make_cb callback,
                                                 void *user_data) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    if (!epwing->subbook_set) return UNIDICT_ERR_NOT_SUPPORTED;

    char *udx_path = epwing_get_udx_path(epwing->path);
    if (!udx_path) return UNIDICT_ERR_INTERNAL;

    udx_writer *writer = udx_writer_open(udx_path);
    if (!writer) {
        free(udx_path);
        return UNIDICT_ERR_IO;
    }

    // Build info metadata
    unidict_info meta = {0};
    meta.title = epwing->title;
    char *meta_xml = unidict_info_to_xml(&meta);

    udx_db_builder *builder = NULL;
    if (meta_xml) {
        builder = udx_db_builder_create_with_metadata(writer, "article",
                    (const uint8_t *)meta_xml, (uint32_t)strlen(meta_xml));
        free(meta_xml);
    } else {
        builder = udx_db_builder_create(writer, "article");
    }
    if (!builder) {
        udx_writer_close(writer);
        free(udx_path);
        return UNIDICT_ERR_INTERNAL;
    }

    // Iterate entries via eb_forward_text
    EB_Position pos;
    eb_text(&epwing->book, &pos);
    eb_seek_text(&epwing->book, &pos);
    // Forward to first article, then backward to get heading position
    if (eb_forward_text(&epwing->book, &epwing->appendix) == EB_SUCCESS) {
        eb_backward_text(&epwing->book, &epwing->appendix);
    }
    eb_tell_text(&epwing->book, &pos);

    int entry_count = 0;
    int last_pct = 0;

    for (;;) {
        char heading[1024];
        ssize_t hlen = epwing_read_heading(epwing, &pos, heading, sizeof(heading));
        if (hlen <= 0 || heading[0] == '\0') {
            // Try next
            eb_seek_text(&epwing->book, &pos);
            if (eb_forward_text(&epwing->book, &epwing->appendix) != EB_SUCCESS) break;
            eb_tell_text(&epwing->book, &pos);
            continue;
        }

        // Get text position (after heading)
        EB_Position text_pos;
        eb_tell_text(&epwing->book, &text_pos);

        uint8_t value[16];
        epwing_pack_positions(&pos, &text_pos, value);
        udx_value_address addr = udx_db_builder_add_value(builder, value, 16);
        if (addr != UDX_INVALID_ADDRESS) {
            udx_db_builder_add_key_entry(builder, heading, addr, 16);
            entry_count++;
        }

        if (callback && (entry_count % 200) == 0) {
            int pct = (entry_count * 100) / (entry_count + 1000);
            if (pct > 100) pct = 100;
            if (pct > last_pct) {
                last_pct = pct;
                if (!callback(dict, UNIDICT_INDEX_STAGE_ARTICLES, pct, user_data)) {
                    udx_db_builder_finalize(builder);
                    udx_writer_close(writer);
                    free(udx_path);
                    return UNIDICT_ERR_CANCELLED;
                }
            }
        }

        eb_seek_text(&epwing->book, &pos);
        if (eb_forward_text(&epwing->book, &epwing->appendix) != EB_SUCCESS) break;
        eb_tell_text(&epwing->book, &pos);
    }

    udx_status err = udx_db_builder_finalize(builder);
    if (err != UDX_OK) {
        udx_writer_close(writer);
        free(udx_path);
        return UNIDICT_ERR_IO;
    }

    err = udx_writer_close(writer);
    free(udx_path);
    return err == UDX_OK ? UNIDICT_OK : UNIDICT_ERR_IO;
}

// ============================================================
// Index external delete
// ============================================================

static unidict_status epwing_index_external_delete(unidict *dict) {
    if (!dict) return UNIDICT_ERR_INVALID_PARAM;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);

    // Switch to builtin mode (closes udx_dict)
    unidict_status st = epwing_index_activate(dict, UNIDICT_INDEX_BUILTIN);
    if (st != UNIDICT_OK) return st;

    char *udx_path = epwing_get_udx_path(epwing->path);
    if (!udx_path) return UNIDICT_ERR_INTERNAL;
    if (remove(udx_path) != 0 && errno != ENOENT) {
        free(udx_path);
        return UNIDICT_ERR_IO;
    }
    free(udx_path);

    dict->has_external_index = false;
    return UNIDICT_OK;
}

// ============================================================
// UDX-aware lookup / entry_lookup / suggest
// ============================================================

static unidict_status epwing_entry_lookup(unidict *dict, const char *key, unidict_entry_array **out_entries) {
    if (!dict || !key || !out_entries) {
        if (out_entries) *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_entries = NULL;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    if (!epwing->subbook_set) return UNIDICT_OK;

    if (epwing->udx_dict) {
        udx_db_value_entry *ve = ud_udx_raw_lookup(epwing->udx_dict, key);
        if (!ve || ve->items.count == 0) {
            if (ve) udx_db_value_entry_free(ve);
            return UNIDICT_OK;
        }

        unidict_entry_array *res = calloc(1, sizeof(unidict_entry_array));
        if (!res) {
            udx_db_value_entry_free(ve);
            return UNIDICT_ERR_NOMEM;
        }
        res->count = ve->items.count;
        res->items = calloc(res->count, sizeof(unidict_entry *));
        if (!res->items) {
            free(res);
            udx_db_value_entry_free(ve);
            return UNIDICT_ERR_NOMEM;
        }

        for (size_t i = 0; i < ve->items.count; i++) {
            ud_epwing_entry_ref *ref = epwing_ref_from_value_item(&ve->items.elements[i]);
            if (!ref) continue;

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (!entry) {
                uobject_release(&ref->obj);
                continue;
            }
            entry->key =
                ve->items.elements[i].original_key ? strdup(ve->items.elements[i].original_key) : strdup(key);
            entry->internal_entry = &ref->obj;
            res->items[i] = entry;
        }
        udx_db_value_entry_free(ve);
        *out_entries = res;
        return UNIDICT_OK;
    }

    if (eb_search_exactword(&epwing->book, key) != EB_SUCCESS) return UNIDICT_OK;

    EB_Hit hits[100];
    int hit_count;
    if (eb_hit_list(&epwing->book, 100, hits, &hit_count) != EB_SUCCESS || hit_count <= 0) return UNIDICT_OK;

    unidict_entry_array *res = malloc(sizeof(unidict_entry_array));
    if (!res) return UNIDICT_ERR_NOMEM;

    res->count = hit_count;
    res->items = calloc(hit_count, sizeof(unidict_entry *));
    if (!res->items) {
        free(res);
        return UNIDICT_ERR_NOMEM;
    }

    for (int i = 0; i < hit_count; i++) {
        char heading[1024];
        ssize_t hlen = epwing_read_heading(epwing, &hits[i].heading, heading, sizeof(heading));
        if (hlen < 0) continue;

        ud_epwing_entry_ref *ref = calloc(1, sizeof(ud_epwing_entry_ref));
        if (!ref) break;
        uobject_init(&ref->obj, &ud_epwing_entry_ref_type, NULL);
        ref->hit = hits[i];

        unidict_entry *entry = calloc(1, sizeof(unidict_entry));
        if (!entry) {
            uobject_release(&ref->obj);
            break;
        }
        entry->key = strdup(heading);
        entry->internal_entry = &ref->obj;
        res->items[i] = entry;
    }

    *out_entries = res;
    return UNIDICT_OK;
}

static unidict_status epwing_lookup(unidict *dict, const char *key, unidict_article_array **out_articles) {
    if (!dict || !key || !out_articles) {
        if (out_articles) *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_articles = NULL;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    if (!epwing->subbook_set) return UNIDICT_OK;

    if (epwing->udx_dict) {
        unidict_entry_array *entries = NULL;
        unidict_status st = epwing_entry_lookup(dict, key, &entries);
        if (st != UNIDICT_OK || !entries || entries->count == 0) {
            if (entries) unidict_entry_array_free(entries);
            return st == UNIDICT_OK ? UNIDICT_OK : st;
        }

        unidict_article_array *res = calloc(1, sizeof(unidict_article_array));
        if (!res) {
            unidict_entry_array_free(entries);
            return UNIDICT_ERR_NOMEM;
        }
        res->count = entries->count;
        res->items = calloc(res->count, sizeof(unidict_article));
        if (!res->items) {
            free(res);
            unidict_entry_array_free(entries);
            return UNIDICT_ERR_NOMEM;
        }

        for (size_t i = 0; i < entries->count; i++) {
            unidict_article_array *single = NULL;
            epwing_fetch(dict, entries->items[i], &single);
            if (single && single->count > 0) {
                res->items[i].title = entries->items[i]->key ? strdup(entries->items[i]->key) : NULL;
                res->items[i].body = single->items[0].body;
                single->items[0].body = NULL;
            }
            if (single) unidict_article_array_free(single);
        }
        unidict_entry_array_free(entries);
        *out_articles = res;
        return UNIDICT_OK;
    }

    if (eb_search_exactword(&epwing->book, key) != EB_SUCCESS) return UNIDICT_OK;

    EB_Hit hits[100];
    int hit_count;
    if (eb_hit_list(&epwing->book, 100, hits, &hit_count) != EB_SUCCESS || hit_count <= 0) return UNIDICT_OK;

    unidict_article_array *res = malloc(sizeof(unidict_article_array));
    if (!res) return UNIDICT_ERR_NOMEM;

    res->count = hit_count;
    res->items = calloc(hit_count, sizeof(unidict_article));
    if (!res->items) {
        free(res);
        return UNIDICT_ERR_NOMEM;
    }

    for (int i = 0; i < hit_count; i++) {
        char heading[1024];
        ssize_t hlen = epwing_read_heading(epwing, &hits[i].heading, heading, sizeof(heading));
        if (hlen >= 0) res->items[i].title = strdup(heading);

        char text[4096];
        ssize_t tlen = epwing_read_text(epwing, &hits[i].text, text, sizeof(text));
        if (tlen >= 0) res->items[i].body = strdup(text);
    }

    *out_articles = res;
    return UNIDICT_OK;
}

static unidict_status epwing_suggest(unidict *dict, const char *prefix, size_t limit,
                                     unidict_entry_array **out_entries) {
    if (!dict || !prefix || !out_entries) {
        if (out_entries) *out_entries = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_entries = NULL;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    if (!epwing->subbook_set) return UNIDICT_OK;

    if (epwing->udx_dict) {
        if (!epwing->udx_dict->ops->suggest) {
            *out_entries = NULL;
            return UNIDICT_ERR_NOT_SUPPORTED;
        }
        unidict_entry_array *udx_entries = NULL;
        unidict_status st = epwing->udx_dict->ops->suggest(epwing->udx_dict, prefix, limit, &udx_entries);
        if (st != UNIDICT_OK || !udx_entries) {
            *out_entries = NULL;
            return st;
        }

        unidict_entry_array *res = calloc(1, sizeof(unidict_entry_array));
        if (!res) {
            unidict_entry_array_free(udx_entries);
            return UNIDICT_ERR_NOMEM;
        }
        res->count = udx_entries->count;
        res->items = calloc(udx_entries->count, sizeof(unidict_entry *));
        if (!res->items) {
            free(res);
            unidict_entry_array_free(udx_entries);
            return UNIDICT_ERR_NOMEM;
        }

        for (size_t i = 0; i < udx_entries->count; i++) {
            unidict_entry *udx_entry = udx_entries->items[i];
            if (!udx_entry) continue;

            udx_db_value_entry *ve = ud_udx_raw_fetch(epwing->udx_dict, udx_entry);
            if (!ve || ve->items.count == 0) {
                if (ve) udx_db_value_entry_free(ve);
                continue;
            }

            ud_epwing_entry_ref *ref = epwing_ref_from_value_item(&ve->items.elements[0]);
            udx_db_value_entry_free(ve);
            if (!ref) continue;

            unidict_entry *entry = calloc(1, sizeof(unidict_entry));
            if (!entry) {
                uobject_release(&ref->obj);
                continue;
            }
            entry->key = strdup(udx_entry->key);
            entry->internal_entry = &ref->obj;
            res->items[i] = entry;
        }

        unidict_entry_array_free(udx_entries);
        *out_entries = res;
        return UNIDICT_OK;
    }

    if (eb_search_word(&epwing->book, prefix) != EB_SUCCESS) return UNIDICT_OK;

    int max_hits = (limit > 0) ? (int)limit : 100;
    EB_Hit *hits = malloc(sizeof(EB_Hit) * max_hits);
    if (!hits) return UNIDICT_ERR_NOMEM;

    int hit_count;
    if (eb_hit_list(&epwing->book, max_hits, hits, &hit_count) != EB_SUCCESS || hit_count <= 0) {
        free(hits);
        return UNIDICT_OK;
    }

    unidict_entry_array *res = malloc(sizeof(unidict_entry_array));
    if (!res) {
        free(hits);
        return UNIDICT_ERR_NOMEM;
    }

    res->count = hit_count;
    res->items = calloc(hit_count, sizeof(unidict_entry *));
    if (!res->items) {
        free(res);
        free(hits);
        return UNIDICT_ERR_NOMEM;
    }

    for (int i = 0; i < hit_count; i++) {
        char heading[1024];
        ssize_t hlen = epwing_read_heading(epwing, &hits[i].heading, heading, sizeof(heading));
        if (hlen < 0) continue;

        ud_epwing_entry_ref *ref = calloc(1, sizeof(ud_epwing_entry_ref));
        if (!ref) break;
        uobject_init(&ref->obj, &ud_epwing_entry_ref_type, NULL);
        ref->hit = hits[i];

        unidict_entry *entry = calloc(1, sizeof(unidict_entry));
        if (!entry) {
            uobject_release(&ref->obj);
            break;
        }
        entry->key = strdup(heading);
        entry->internal_entry = &ref->obj;
        res->items[i] = entry;
    }

    free(hits);
    *out_entries = res;
    return UNIDICT_OK;
}

// ============================================================
// Fetch
// ============================================================

static unidict_status epwing_fetch(unidict *dict, unidict_entry *entry, unidict_article_array **out_articles) {
    if (!dict || !entry || !entry->internal_entry || !out_articles) {
        if (out_articles) *out_articles = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_articles = NULL;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    ud_epwing_entry_ref *ref = uobject_cast(entry->internal_entry, ud_epwing_entry_ref, obj);

    unidict_article_array *res = malloc(sizeof(unidict_article_array));
    if (!res) return UNIDICT_ERR_NOMEM;

    res->count = 1;
    res->items = calloc(1, sizeof(unidict_article));
    if (!res->items) {
        free(res);
        return UNIDICT_ERR_NOMEM;
    }

    char heading[1024];
    ssize_t hlen = epwing_read_heading(epwing, &ref->hit.heading, heading, sizeof(heading));
    if (hlen >= 0) res->items[0].title = strdup(heading);

    char text[4096];
    ssize_t tlen = epwing_read_text(epwing, &ref->hit.text, text, sizeof(text));
    if (tlen >= 0) res->items[0].body = strdup(text);

    *out_articles = res;
    return UNIDICT_OK;
}

// ============================================================
// Entry iterator (eb_forward_text based)
// ============================================================

typedef struct {
    unidict_entry_iter base;
    EB_Position pos;
    bool done;
} ud_epwing_entry_iter;

static unidict_entry_iter *epwing_entry_iter_create(unidict *dict) {
    if (!dict) return NULL;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    if (!epwing->subbook_set) return NULL;

    ud_epwing_entry_iter *iter = calloc(1, sizeof(ud_epwing_entry_iter));
    if (!iter) return NULL;

    iter->base.dict = dict;
    iter->done = false;

    // Position at start of text area
    EB_Position text_start;
    eb_text(&epwing->book, &text_start);
    eb_seek_text(&epwing->book, &text_start);

    // Forward to first article, backward to get heading
    if (eb_forward_text(&epwing->book, &epwing->appendix) == EB_SUCCESS) {
        eb_backward_text(&epwing->book, &epwing->appendix);
    }
    eb_tell_text(&epwing->book, &iter->pos);

    return (unidict_entry_iter *)iter;
}

static unidict_status epwing_entry_iter_next(unidict_entry_iter *iter, unidict_entry **out_entry) {
    if (!iter || !iter->dict) {
        if (out_entry) *out_entry = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    ud_epwing_entry_iter *epwing_iter = (ud_epwing_entry_iter *)iter;
    ud_epwing *epwing = uobject_cast(&iter->dict->obj, ud_epwing, base.obj);

    if (epwing_iter->done) {
        *out_entry = NULL;
        return UNIDICT_DONE;
    }

    free(iter->current.key);
    iter->current.key = NULL;
    if (iter->current.internal_entry) {
        uobject_release(iter->current.internal_entry);
        iter->current.internal_entry = NULL;
    }

    // Read heading at current position
    EB_Position heading_pos = epwing_iter->pos;
    char heading[1024];
    ssize_t hlen = epwing_read_heading(epwing, &heading_pos, heading, sizeof(heading));

    // Get text position (after heading)
    EB_Position text_pos;
    eb_tell_text(&epwing->book, &text_pos);

    // Advance to next entry for subsequent calls
    eb_seek_text(&epwing->book, &heading_pos);
    if (eb_forward_text(&epwing->book, &epwing->appendix) != EB_SUCCESS) {
        epwing_iter->done = true;
    } else {
        eb_tell_text(&epwing->book, &epwing_iter->pos);
    }

    // Skip empty headings
    if (hlen <= 0 || heading[0] == '\0') {
        if (epwing_iter->done) {
            *out_entry = NULL;
            return UNIDICT_DONE;
        }
        return epwing_entry_iter_next(iter, out_entry);
    }

    ud_epwing_entry_ref *ref = calloc(1, sizeof(ud_epwing_entry_ref));
    if (!ref) {
        *out_entry = NULL;
        return UNIDICT_ERR_NOMEM;
    }
    uobject_init(&ref->obj, &ud_epwing_entry_ref_type, NULL);
    ref->hit.heading = heading_pos;
    ref->hit.text = text_pos;

    iter->current.key = strdup(heading);
    iter->current.internal_entry = &ref->obj;

    *out_entry = &iter->current;
    return UNIDICT_OK;
}

static void epwing_entry_iter_free(unidict_entry_iter *iter) {
    if (!iter) return;
    free(iter->current.key);
    if (iter->current.internal_entry) {
        uobject_release(iter->current.internal_entry);
    }
    free(iter);
}

// ============================================================
// Resource get (images and audio)
// ============================================================

static unidict_status epwing_resource_get(unidict *dict, const char *key, unidict_resource **out_res) {
    if (!dict || !key || !out_res) {
        if (out_res) *out_res = NULL;
        return UNIDICT_ERR_INVALID_PARAM;
    }

    *out_res = NULL;
    ud_epwing *epwing = uobject_cast(&dict->obj, ud_epwing, base.obj);
    if (!epwing->subbook_set) return UNIDICT_ERR_NOT_SUPPORTED;

    size_t prefix_len = strlen(EPWING_RES_PREFIX);
    if (strncmp(key, EPWING_RES_PREFIX, prefix_len) != 0) return UNIDICT_ERR_NOT_FOUND;

    const char *path = key + prefix_len;
    EB_Error_Code ec = EB_SUCCESS;

    // Gaiji resource: gaiji/{n|w}/{char_number}
    if (strncmp(path, "gaiji/", 6) == 0) {
        const char *gp = path + 6;
        bool is_wide;
        if (gp[0] == 'n' && gp[1] == '/') {
            is_wide = false;
        } else if (gp[0] == 'w' && gp[1] == '/') {
            is_wide = true;
        } else {
            return UNIDICT_ERR_NOT_FOUND;
        }
        int char_number = (int)strtol(gp + 2, NULL, 10);

        char bitmap[64];
        ec = is_wide
            ? eb_wide_font_character_bitmap(&epwing->book, char_number, bitmap)
            : eb_narrow_font_character_bitmap(&epwing->book, char_number, bitmap);
        if (ec != EB_SUCCESS) return UNIDICT_ERR_NOT_FOUND;

        int width = is_wide ? 16 : 8;
        int height = 16;
        if (is_wide) eb_wide_font_width(&epwing->book, &width);
        else eb_narrow_font_width(&epwing->book, &width);
        eb_font_height(&epwing->book, &height);

        char bmp[446]; // max: EB_SIZE_WIDE_FONT_48_BMP
        size_t bmp_length = 0;
        eb_bitmap_to_bmp(bitmap, width, height, bmp, &bmp_length);
        if (bmp_length == 0) return UNIDICT_ERR_NOT_FOUND;

        unidict_resource *res = calloc(1, sizeof(unidict_resource));
        if (!res) return UNIDICT_ERR_NOMEM;

        res->key = strdup(key);
        res->data = malloc(bmp_length);
        if (!res->data) {
            free(res->key);
            free(res);
            return UNIDICT_ERR_NOMEM;
        }
        memcpy(res->data, bmp, bmp_length);
        res->size = bmp_length;
        res->mime_type = strdup("image/bmp");

        *out_res = res;
        return UNIDICT_OK;
    }

    if (path[0] == 'g' && path[1] == '/') {
        // Image resource: g/{type}/{page}/{offset}
        const char *type_start = path + 2;
        const char *slash1 = strchr(type_start, '/');
        if (!slash1) return UNIDICT_ERR_NOT_FOUND;

        int type_len = (int)(slash1 - type_start);
        char type[4] = {0};
        if (type_len > 3) type_len = 3;
        memcpy(type, type_start, type_len);

        char *endp;
        int page = (int)strtol(slash1 + 1, &endp, 10);
        if (*endp != '/') return UNIDICT_ERR_NOT_FOUND;
        int offset = (int)strtol(endp + 1, &endp, 10);

        EB_Position pos = { .page = page, .offset = offset };

        if (strcmp(type, "m") == 0) {
            ec = eb_set_binary_mono_graphic(&epwing->book, &pos, 0, 0);
        } else {
            ec = eb_set_binary_color_graphic(&epwing->book, &pos);
        }
        if (ec != EB_SUCCESS) return UNIDICT_ERR_IO;

    } else if (path[0] == 'w' && path[1] == '/') {
        // Audio resource: w/{start_page}/{start_offset}/{end_page}/{end_offset}
        char *endp;
        int start_page = (int)strtol(path + 2, &endp, 10);
        if (*endp != '/') return UNIDICT_ERR_NOT_FOUND;
        int start_offset = (int)strtol(endp + 1, &endp, 10);
        if (*endp != '/') return UNIDICT_ERR_NOT_FOUND;
        int end_page = (int)strtol(endp + 1, &endp, 10);
        if (*endp != '/') return UNIDICT_ERR_NOT_FOUND;
        int end_offset = (int)strtol(endp + 1, &endp, 10);

        EB_Position start = { .page = start_page, .offset = start_offset };
        EB_Position end = { .page = end_page, .offset = end_offset };

        ec = eb_set_binary_wave(&epwing->book, &start, &end);
        if (ec != EB_SUCCESS) return UNIDICT_ERR_IO;

    } else if (path[0] == 'v' && path[1] == '/') {
        // Video resource: v/{argv1}/{argv2}/{argv3}/{argv4}/{argv5}
        char *endp;
        unsigned int mpeg_argv[5];
        mpeg_argv[0] = (unsigned int)strtoul(path + 2, &endp, 10);
        for (int i = 1; i < 5; i++) {
            if (*endp != '/') return UNIDICT_ERR_NOT_FOUND;
            mpeg_argv[i] = (unsigned int)strtoul(endp + 1, &endp, 10);
        }
        ec = eb_set_binary_mpeg(&epwing->book, mpeg_argv);
        if (ec != EB_SUCCESS) return UNIDICT_ERR_IO;

    } else {
        return UNIDICT_ERR_NOT_FOUND;
    }

    // Read binary data with growing buffer
    size_t capacity = 65536;
    uint8_t *data = malloc(capacity);
    if (!data) {
        eb_unset_binary(&epwing->book);
        return UNIDICT_ERR_NOMEM;
    }

    size_t total = 0;
    for (;;) {
        ssize_t chunk_len = 0;
        ec = eb_read_binary(&epwing->book, capacity - total, (char *)(data + total), &chunk_len);
        if (ec != EB_SUCCESS || chunk_len <= 0) break;
        total += chunk_len;
        if (total >= capacity) {
            capacity *= 2;
            uint8_t *new_data = realloc(data, capacity);
            if (!new_data) {
                free(data);
                eb_unset_binary(&epwing->book);
                return UNIDICT_ERR_NOMEM;
            }
            data = new_data;
        }
    }

    eb_unset_binary(&epwing->book);

    if (total == 0) {
        free(data);
        return UNIDICT_ERR_NOT_FOUND;
    }

    // Shrink to actual size
    if (total < capacity) {
        uint8_t *shrunk = realloc(data, total);
        if (shrunk) data = shrunk;
    }

    unidict_resource *res = calloc(1, sizeof(unidict_resource));
    if (!res) {
        free(data);
        return UNIDICT_ERR_NOMEM;
    }

    res->key = strdup(key);
    res->data = data;
    res->size = total;

    // Determine MIME type from URL type indicator
    if (path[0] == 'g') {
        const char *type_start = path + 2;
        if (strncmp(type_start, "j/", 2) == 0 || strncmp(type_start, "ij/", 3) == 0) {
            res->mime_type = strdup("image/jpeg");
        } else {
            res->mime_type = strdup("image/bmp");
        }
    } else if (path[0] == 'w') {
        res->mime_type = strdup("audio/wav");
    } else {
        res->mime_type = strdup("video/mpeg");
    }

    *out_res = res;
    return UNIDICT_OK;
}
