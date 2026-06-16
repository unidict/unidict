//
//  ud_mime.h
//  unidict
//
//  Image MIME type detection utility
//
#ifndef ud_mime_h
#define ud_mime_h

#include <stdint.h>
#include <stddef.h>

const char *ud_detect_image_mime(const uint8_t *data, size_t size);

#endif /* ud_mime_h */
