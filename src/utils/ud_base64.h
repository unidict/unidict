//
//  ud_base64.h
//  unidict
//
//  Base64 encode/decode utility
//
#ifndef ud_base64_h
#define ud_base64_h

#include <stdint.h>
#include <stddef.h>

// Returns malloc'd Base64 string, caller must free. Returns NULL on error.
char *ud_base64_encode(const uint8_t *data, size_t len);

// Returns malloc'd decoded buffer, writes decoded length to *out_len. Returns NULL on error.
uint8_t *ud_base64_decode(const char *str, size_t *out_len);

#endif /* ud_base64_h */
