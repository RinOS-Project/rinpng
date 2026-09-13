#ifndef RIN_PNG_RPNG_H
#define RIN_PNG_RPNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPNG_OK 0
#define RPNG_ERR_PARAM -1
#define RPNG_ERR_FORMAT -2
#define RPNG_ERR_UNSUPPORTED -3
#define RPNG_ERR_NOMEM -4
#define RPNG_ERR_CRC -5
#define RPNG_ERR_DECOMPRESS -6

int rpng_get_info(const uint8_t* data, size_t size, int* width, int* height);

int rpng_decode_rgba(const uint8_t* data, size_t size,
                     uint32_t* out_pixels, int out_width, int out_height);

#ifdef __cplusplus
}
#endif

#endif /* RIN_PNG_RPNG_H */
