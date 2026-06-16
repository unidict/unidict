//
//  ud_mime.c
//  unidict
//
//  Image MIME type detection by magic bytes
//
#include "ud_mime.h"

const char *ud_detect_image_mime(const uint8_t *data, size_t size) {
    if (size >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return "image/png";
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return "image/jpeg";
    if (size >= 2 && data[0] == 'B' && data[1] == 'M')
        return "image/bmp";
    if (size >= 4 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8')
        return "image/gif";
    if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0x00)
        return "image/x-icon";
    return "image/bmp";
}
