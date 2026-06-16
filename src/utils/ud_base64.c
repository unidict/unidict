//
//  ud_base64.c
//  unidict
//
//  Base64 encode/decode utility
//

#include "ud_base64.h"
#include <stdlib.h>
#include <string.h>

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *ud_base64_encode(const uint8_t *data, size_t len) {
    if (!data || len == 0) return NULL;

    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = data[i++];
        uint32_t b = (i < len) ? data[i++] : 0;
        uint32_t c = (i < len) ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }

    // Padding
    size_t pad = (3 - (len % 3)) % 3;
    for (size_t k = 0; k < pad; k++) {
        out[out_len - 1 - k] = '=';
    }
    out[out_len] = '\0';
    return out;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

uint8_t *ud_base64_decode(const char *str, size_t *out_len) {
    if (!str || !out_len) return NULL;

    size_t slen = strlen(str);
    if (slen == 0 || slen % 4 != 0) return NULL;

    size_t alloc_len = (slen / 4) * 3;
    // Adjust for padding
    if (str[slen - 1] == '=') alloc_len--;
    if (str[slen - 2] == '=') alloc_len--;

    uint8_t *out = malloc(alloc_len);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < slen; i += 4) {
        int a = b64_decode_char(str[i]);
        int b = b64_decode_char(str[i + 1]);
        int c = b64_decode_char(str[i + 2]);
        int d = b64_decode_char(str[i + 3]);
        if (a < 0 || b < 0) { free(out); return NULL; }

        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12);
        if (c >= 0) triple |= (uint32_t)c << 6;
        if (d >= 0) triple |= (uint32_t)d;

        out[j++] = (triple >> 16) & 0xFF;
        if (c >= 0) out[j++] = (triple >> 8) & 0xFF;
        if (d >= 0) out[j++] = triple & 0xFF;
    }

    *out_len = j;
    return out;
}
