#include "rpng.h"

/* Freestanding: declare libc functions manually to avoid C++ header conflicts */
extern void* malloc(size_t);
extern void* realloc(void*, size_t);
extern void  free(void*);
extern void* memset(void*, int, size_t);
extern void* memcpy(void*, const void*, size_t);
extern int   memcmp(const void*, const void*, size_t);

#include "../rinzlib/rinz.h"
#include "../rinzlib/rinz_checksum.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    int bit_depth;
    int color_type;
    int compression;
    int filter;
    int interlace;
    int has_ihdr;
    int has_iend;

    uint8_t* idat;
    size_t idat_size;
    size_t idat_cap;

    uint8_t* palette;
    size_t palette_size; /* bytes, RGB triplets */

    uint8_t trns_palette[256];
    size_t trns_palette_size;

    int has_trns_gray;
    uint16_t trns_gray;
    int has_trns_rgb;
    uint16_t trns_r;
    uint16_t trns_g;
    uint16_t trns_b;
} RPNGState;

static uint32_t rpng_read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint16_t rpng_read_u16_be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int rpng_is_valid_signature(const uint8_t* data, size_t size) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (!data || size < 8) return 0;
    return memcmp(data, sig, 8) == 0;
}

static int rpng_valid_bit_depth(int color_type, int bit_depth) {
    switch (color_type) {
        case 0: return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
                       bit_depth == 8 || bit_depth == 16;
        case 2: return bit_depth == 8 || bit_depth == 16;
        case 3: return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
                       bit_depth == 8;
        case 4: return bit_depth == 8 || bit_depth == 16;
        case 6: return bit_depth == 8 || bit_depth == 16;
        default: return 0;
    }
}

static int rpng_channels_for_color_type(int color_type) {
    switch (color_type) {
        case 0: return 1;
        case 2: return 3;
        case 3: return 1;
        case 4: return 2;
        case 6: return 4;
        default: return 0;
    }
}

static int rpng_append_idat(RPNGState* st, const uint8_t* data, size_t size) {
    if (!st || !data) return RPNG_ERR_PARAM;
    if (size == 0) return RPNG_OK;

    if (st->idat_size + size < st->idat_size) return RPNG_ERR_FORMAT;
    if (st->idat_size + size > st->idat_cap) {
        size_t new_cap = st->idat_cap ? st->idat_cap : 4096;
        while (new_cap < st->idat_size + size) {
            if (new_cap > (size_t)(1u << 30)) return RPNG_ERR_NOMEM;
            new_cap *= 2;
        }
        uint8_t* new_buf = (uint8_t*)realloc(st->idat, new_cap);
        if (!new_buf) return RPNG_ERR_NOMEM;
        st->idat = new_buf;
        st->idat_cap = new_cap;
    }

    memcpy(st->idat + st->idat_size, data, size);
    st->idat_size += size;
    return RPNG_OK;
}

static void rpng_state_init(RPNGState* st) {
    memset(st, 0, sizeof(*st));
}

static void rpng_state_free(RPNGState* st) {
    if (!st) return;
    if (st->idat) free(st->idat);
    if (st->palette) free(st->palette);
    memset(st, 0, sizeof(*st));
}

static int rpng_parse_chunks(const uint8_t* data, size_t size, RPNGState* st) {
    size_t pos = 8; /* signature */
    if (!rpng_is_valid_signature(data, size)) return RPNG_ERR_FORMAT;

    while (pos + 12 <= size) {
        uint32_t chunk_len = rpng_read_u32_be(data + pos);
        const uint8_t* chunk_type = data + pos + 4;
        const uint8_t* chunk_data = data + pos + 8;
        size_t chunk_total = 12u + (size_t)chunk_len;
        uint32_t chunk_crc;
        uint32_t calc_crc;

        if (pos + chunk_total > size) return RPNG_ERR_FORMAT;

        chunk_crc = rpng_read_u32_be(chunk_data + chunk_len);
        calc_crc = rinz_crc32(chunk_type, 4);
        calc_crc = rinz_crc32_update(calc_crc, chunk_data, chunk_len);
        if (calc_crc != chunk_crc) return RPNG_ERR_CRC;

        if (memcmp(chunk_type, "IHDR", 4) == 0) {
            if (chunk_len != 13 || st->has_ihdr) return RPNG_ERR_FORMAT;
            st->width = rpng_read_u32_be(chunk_data);
            st->height = rpng_read_u32_be(chunk_data + 4);
            st->bit_depth = (int)chunk_data[8];
            st->color_type = (int)chunk_data[9];
            st->compression = (int)chunk_data[10];
            st->filter = (int)chunk_data[11];
            st->interlace = (int)chunk_data[12];

            if (st->width == 0 || st->height == 0) return RPNG_ERR_FORMAT;
            if (st->compression != 0 || st->filter != 0) return RPNG_ERR_UNSUPPORTED;
            if (!(st->interlace == 0 || st->interlace == 1)) return RPNG_ERR_UNSUPPORTED;
            if (!rpng_valid_bit_depth(st->color_type, st->bit_depth)) return RPNG_ERR_UNSUPPORTED;
            st->has_ihdr = 1;
        } else if (memcmp(chunk_type, "PLTE", 4) == 0) {
            uint8_t* new_palette;
            if (!st->has_ihdr) return RPNG_ERR_FORMAT;
            if (chunk_len == 0 || (chunk_len % 3) != 0 || chunk_len > 768) return RPNG_ERR_FORMAT;
            new_palette = (uint8_t*)realloc(st->palette, chunk_len);
            if (!new_palette) return RPNG_ERR_NOMEM;
            st->palette = new_palette;
            memcpy(st->palette, chunk_data, chunk_len);
            st->palette_size = chunk_len;
        } else if (memcmp(chunk_type, "tRNS", 4) == 0) {
            if (!st->has_ihdr) return RPNG_ERR_FORMAT;
            if (st->color_type == 3) {
                if (chunk_len > 256) return RPNG_ERR_FORMAT;
                memset(st->trns_palette, 255, sizeof(st->trns_palette));
                memcpy(st->trns_palette, chunk_data, chunk_len);
                st->trns_palette_size = chunk_len;
            } else if (st->color_type == 0) {
                if (chunk_len != 2) return RPNG_ERR_FORMAT;
                st->has_trns_gray = 1;
                st->trns_gray = rpng_read_u16_be(chunk_data);
            } else if (st->color_type == 2) {
                if (chunk_len != 6) return RPNG_ERR_FORMAT;
                st->has_trns_rgb = 1;
                st->trns_r = rpng_read_u16_be(chunk_data);
                st->trns_g = rpng_read_u16_be(chunk_data + 2);
                st->trns_b = rpng_read_u16_be(chunk_data + 4);
            }
        } else if (memcmp(chunk_type, "IDAT", 4) == 0) {
            int rc;
            if (!st->has_ihdr) return RPNG_ERR_FORMAT;
            rc = rpng_append_idat(st, chunk_data, chunk_len);
            if (rc != RPNG_OK) return rc;
        } else if (memcmp(chunk_type, "IEND", 4) == 0) {
            if (chunk_len != 0) return RPNG_ERR_FORMAT;
            st->has_iend = 1;
            return RPNG_OK;
        }

        pos += chunk_total;
    }

    return st->has_iend ? RPNG_OK : RPNG_ERR_FORMAT;
}

static uint64_t rpng_row_bytes(uint32_t width, int bit_depth, int channels) {
    uint64_t bits = (uint64_t)width * (uint64_t)bit_depth * (uint64_t)channels;
    return (bits + 7u) / 8u;
}

static uint64_t rpng_expected_raw_size(const RPNGState* st) {
    static const int pass_x_start[7] = {0, 4, 0, 2, 0, 1, 0};
    static const int pass_y_start[7] = {0, 0, 4, 0, 2, 0, 1};
    static const int pass_x_step[7] = {8, 8, 4, 4, 2, 2, 1};
    static const int pass_y_step[7] = {8, 8, 8, 4, 4, 2, 2};
    int channels = rpng_channels_for_color_type(st->color_type);
    uint64_t total = 0;
    int i;

    if (channels <= 0) return 0;

    if (st->interlace == 0) {
        uint64_t rb = rpng_row_bytes(st->width, st->bit_depth, channels);
        return (rb + 1u) * (uint64_t)st->height;
    }

    for (i = 0; i < 7; ++i) {
        uint32_t pw = 0;
        uint32_t ph = 0;
        uint64_t rb;
        if (st->width > (uint32_t)pass_x_start[i]) {
            pw = (st->width - (uint32_t)pass_x_start[i] + (uint32_t)pass_x_step[i] - 1u) /
                 (uint32_t)pass_x_step[i];
        }
        if (st->height > (uint32_t)pass_y_start[i]) {
            ph = (st->height - (uint32_t)pass_y_start[i] + (uint32_t)pass_y_step[i] - 1u) /
                 (uint32_t)pass_y_step[i];
        }
        if (pw == 0 || ph == 0) continue;
        rb = rpng_row_bytes(pw, st->bit_depth, channels);
        total += (rb + 1u) * (uint64_t)ph;
    }
    return total;
}

static int rpng_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p - a;
    int pb = p - b;
    int pc = p - c;
    if (pa < 0) pa = -pa;
    if (pb < 0) pb = -pb;
    if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int rpng_unfilter_row(uint8_t* dst, const uint8_t* src, const uint8_t* prev,
                             size_t row_bytes, size_t bpp) {
    uint8_t filter = src[0];
    const uint8_t* in = src + 1;
    size_t i;
    if (bpp == 0) bpp = 1;

    switch (filter) {
        case 0:
            memcpy(dst, in, row_bytes);
            break;
        case 1:
            for (i = 0; i < row_bytes; ++i) {
                uint8_t left = (i >= bpp) ? dst[i - bpp] : 0;
                dst[i] = (uint8_t)(in[i] + left);
            }
            break;
        case 2:
            for (i = 0; i < row_bytes; ++i) {
                uint8_t up = prev ? prev[i] : 0;
                dst[i] = (uint8_t)(in[i] + up);
            }
            break;
        case 3:
            for (i = 0; i < row_bytes; ++i) {
                uint8_t left = (i >= bpp) ? dst[i - bpp] : 0;
                uint8_t up = prev ? prev[i] : 0;
                dst[i] = (uint8_t)(in[i] + ((left + up) >> 1));
            }
            break;
        case 4:
            for (i = 0; i < row_bytes; ++i) {
                int left = (i >= bpp) ? dst[i - bpp] : 0;
                int up = prev ? prev[i] : 0;
                int ul = (prev && i >= bpp) ? prev[i - bpp] : 0;
                dst[i] = (uint8_t)(in[i] + rpng_paeth(left, up, ul));
            }
            break;
        default:
            return RPNG_ERR_FORMAT;
    }
    return RPNG_OK;
}

static uint32_t rpng_read_packed_sample(const uint8_t* row, int bit_depth, uint32_t x) {
    uint32_t bit_index = x * (uint32_t)bit_depth;
    uint32_t byte_index = bit_index >> 3;
    uint32_t shift = 8u - (uint32_t)bit_depth - (bit_index & 7u);
    uint32_t mask = ((uint32_t)1u << (uint32_t)bit_depth) - 1u;
    return ((uint32_t)row[byte_index] >> shift) & mask;
}

static void rpng_store_argb(uint32_t* out, uint32_t idx,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    out[idx] = ((uint32_t)a << 24) |
               ((uint32_t)r << 16) |
               ((uint32_t)g << 8) |
               (uint32_t)b;
}

static int rpng_decode_row_to_argb(const RPNGState* st, const uint8_t* row,
                                   uint32_t row_pixels, uint32_t* out_row) {
    uint32_t x;
    if (!st || !row || !out_row) return RPNG_ERR_PARAM;

    switch (st->color_type) {
        case 0: {
            if (st->bit_depth < 8) {
                uint32_t maxv = ((uint32_t)1u << (uint32_t)st->bit_depth) - 1u;
                for (x = 0; x < row_pixels; ++x) {
                    uint32_t g = rpng_read_packed_sample(row, st->bit_depth, x);
                    uint8_t gray = (uint8_t)((g * 255u + (maxv / 2u)) / maxv);
                    uint8_t a = 255;
                    if (st->has_trns_gray) {
                        uint16_t key = (uint16_t)(st->trns_gray & maxv);
                        if ((uint16_t)g == key) a = 0;
                    }
                    rpng_store_argb(out_row, x, gray, gray, gray, a);
                }
            } else if (st->bit_depth == 8) {
                for (x = 0; x < row_pixels; ++x) {
                    uint8_t gray = row[x];
                    uint8_t a = 255;
                    if (st->has_trns_gray && st->trns_gray == (uint16_t)gray) a = 0;
                    rpng_store_argb(out_row, x, gray, gray, gray, a);
                }
            } else { /* 16 */
                for (x = 0; x < row_pixels; ++x) {
                    const uint8_t* p = row + (size_t)x * 2u;
                    uint16_t gv = rpng_read_u16_be(p);
                    uint8_t gray = (uint8_t)(gv >> 8);
                    uint8_t a = (st->has_trns_gray && gv == st->trns_gray) ? 0 : 255;
                    rpng_store_argb(out_row, x, gray, gray, gray, a);
                }
            }
            break;
        }
        case 2: {
            if (st->bit_depth == 8) {
                for (x = 0; x < row_pixels; ++x) {
                    const uint8_t* p = row + (size_t)x * 3u;
                    uint8_t r = p[0], g = p[1], b = p[2], a = 255;
                    if (st->has_trns_rgb &&
                        st->trns_r == (uint16_t)r &&
                        st->trns_g == (uint16_t)g &&
                        st->trns_b == (uint16_t)b) {
                        a = 0;
                    }
                    rpng_store_argb(out_row, x, r, g, b, a);
                }
            } else {
                for (x = 0; x < row_pixels; ++x) {
                    const uint8_t* p = row + (size_t)x * 6u;
                    uint16_t rv = rpng_read_u16_be(p);
                    uint16_t gv = rpng_read_u16_be(p + 2);
                    uint16_t bv = rpng_read_u16_be(p + 4);
                    uint8_t a = (st->has_trns_rgb &&
                                 rv == st->trns_r && gv == st->trns_g && bv == st->trns_b) ? 0 : 255;
                    rpng_store_argb(out_row, x, (uint8_t)(rv >> 8), (uint8_t)(gv >> 8),
                                    (uint8_t)(bv >> 8), a);
                }
            }
            break;
        }
        case 3: {
            uint32_t maxv = ((uint32_t)1u << (uint32_t)st->bit_depth) - 1u;
            if (!st->palette || st->palette_size == 0) return RPNG_ERR_FORMAT;
            for (x = 0; x < row_pixels; ++x) {
                uint32_t idx;
                uint8_t r, g, b, a = 255;
                if (st->bit_depth < 8) idx = rpng_read_packed_sample(row, st->bit_depth, x);
                else idx = row[x];
                if (idx > maxv) return RPNG_ERR_FORMAT;
                if ((size_t)idx * 3u + 2u >= st->palette_size) return RPNG_ERR_FORMAT;
                r = st->palette[(size_t)idx * 3u + 0u];
                g = st->palette[(size_t)idx * 3u + 1u];
                b = st->palette[(size_t)idx * 3u + 2u];
                if ((size_t)idx < st->trns_palette_size) a = st->trns_palette[idx];
                rpng_store_argb(out_row, x, r, g, b, a);
            }
            break;
        }
        case 4: {
            if (st->bit_depth == 8) {
                for (x = 0; x < row_pixels; ++x) {
                    const uint8_t* p = row + (size_t)x * 2u;
                    rpng_store_argb(out_row, x, p[0], p[0], p[0], p[1]);
                }
            } else {
                for (x = 0; x < row_pixels; ++x) {
                    const uint8_t* p = row + (size_t)x * 4u;
                    uint16_t gv = rpng_read_u16_be(p);
                    uint16_t av = rpng_read_u16_be(p + 2);
                    rpng_store_argb(out_row, x, (uint8_t)(gv >> 8), (uint8_t)(gv >> 8),
                                    (uint8_t)(gv >> 8), (uint8_t)(av >> 8));
                }
            }
            break;
        }
        case 6: {
            if (st->bit_depth == 8) {
                for (x = 0; x < row_pixels; ++x) {
                    const uint8_t* p = row + (size_t)x * 4u;
                    rpng_store_argb(out_row, x, p[0], p[1], p[2], p[3]);
                }
            } else {
                for (x = 0; x < row_pixels; ++x) {
                    const uint8_t* p = row + (size_t)x * 8u;
                    uint16_t rv = rpng_read_u16_be(p);
                    uint16_t gv = rpng_read_u16_be(p + 2);
                    uint16_t bv = rpng_read_u16_be(p + 4);
                    uint16_t av = rpng_read_u16_be(p + 6);
                    rpng_store_argb(out_row, x, (uint8_t)(rv >> 8), (uint8_t)(gv >> 8),
                                    (uint8_t)(bv >> 8), (uint8_t)(av >> 8));
                }
            }
            break;
        }
        default:
            return RPNG_ERR_UNSUPPORTED;
    }

    return RPNG_OK;
}

static int rpng_decode_non_interlaced(const RPNGState* st,
                                      const uint8_t* raw, size_t raw_size,
                                      uint32_t* out_pixels) {
    int channels = rpng_channels_for_color_type(st->color_type);
    size_t row_bytes;
    size_t bpp;
    size_t row;
    size_t pos = 0;
    uint8_t* prev = NULL;
    uint8_t* cur = NULL;
    int rc = RPNG_OK;

    if (channels <= 0) return RPNG_ERR_UNSUPPORTED;
    row_bytes = (size_t)rpng_row_bytes(st->width, st->bit_depth, channels);
    bpp = ((size_t)st->bit_depth * (size_t)channels + 7u) / 8u;
    if (bpp == 0) bpp = 1;

    prev = (uint8_t*)malloc(row_bytes);
    cur = (uint8_t*)malloc(row_bytes);
    if (!prev || !cur) {
        rc = RPNG_ERR_NOMEM;
        goto done;
    }
    memset(prev, 0, row_bytes);

    for (row = 0; row < st->height; ++row) {
        uint32_t* out_row = out_pixels + row * st->width;
        if (pos + 1u + row_bytes > raw_size) {
            rc = RPNG_ERR_FORMAT;
            goto done;
        }
        rc = rpng_unfilter_row(cur, raw + pos, prev, row_bytes, bpp);
        if (rc != RPNG_OK) goto done;

        rc = rpng_decode_row_to_argb(st, cur, st->width, out_row);
        if (rc != RPNG_OK) goto done;

        memcpy(prev, cur, row_bytes);
        pos += 1u + row_bytes;
    }

done:
    if (prev) free(prev);
    if (cur) free(cur);
    return rc;
}

static int rpng_decode_adam7(const RPNGState* st,
                             const uint8_t* raw, size_t raw_size,
                             uint32_t* out_pixels) {
    static const int pass_x_start[7] = {0, 4, 0, 2, 0, 1, 0};
    static const int pass_y_start[7] = {0, 0, 4, 0, 2, 0, 1};
    static const int pass_x_step[7] = {8, 8, 4, 4, 2, 2, 1};
    static const int pass_y_step[7] = {8, 8, 8, 4, 4, 2, 2};
    int channels = rpng_channels_for_color_type(st->color_type);
    size_t bpp;
    size_t pos = 0;
    int pass;
    int rc = RPNG_OK;

    if (channels <= 0) return RPNG_ERR_UNSUPPORTED;
    bpp = ((size_t)st->bit_depth * (size_t)channels + 7u) / 8u;
    if (bpp == 0) bpp = 1;

    for (pass = 0; pass < 7; ++pass) {
        uint32_t pw = 0;
        uint32_t ph = 0;
        size_t row_bytes;
        uint8_t* prev = NULL;
        uint8_t* cur = NULL;
        uint32_t py;

        if (st->width > (uint32_t)pass_x_start[pass]) {
            pw = (st->width - (uint32_t)pass_x_start[pass] + (uint32_t)pass_x_step[pass] - 1u) /
                 (uint32_t)pass_x_step[pass];
        }
        if (st->height > (uint32_t)pass_y_start[pass]) {
            ph = (st->height - (uint32_t)pass_y_start[pass] + (uint32_t)pass_y_step[pass] - 1u) /
                 (uint32_t)pass_y_step[pass];
        }
        if (pw == 0 || ph == 0) continue;

        row_bytes = (size_t)rpng_row_bytes(pw, st->bit_depth, channels);
        prev = (uint8_t*)malloc(row_bytes);
        cur = (uint8_t*)malloc(row_bytes);
        if (!prev || !cur) {
            if (prev) free(prev);
            if (cur) free(cur);
            return RPNG_ERR_NOMEM;
        }
        memset(prev, 0, row_bytes);

        for (py = 0; py < ph; ++py) {
            uint32_t out_tmp_max = pw;
            uint32_t* out_tmp = NULL;
            uint32_t px;
            if (pos + 1u + row_bytes > raw_size) {
                rc = RPNG_ERR_FORMAT;
                goto pass_done;
            }

            rc = rpng_unfilter_row(cur, raw + pos, prev, row_bytes, bpp);
            if (rc != RPNG_OK) goto pass_done;

            out_tmp = (uint32_t*)malloc((size_t)out_tmp_max * sizeof(uint32_t));
            if (!out_tmp) {
                rc = RPNG_ERR_NOMEM;
                goto pass_done;
            }

            rc = rpng_decode_row_to_argb(st, cur, pw, out_tmp);
            if (rc != RPNG_OK) {
                free(out_tmp);
                goto pass_done;
            }

            for (px = 0; px < pw; ++px) {
                uint32_t ox = (uint32_t)pass_x_start[pass] + px * (uint32_t)pass_x_step[pass];
                uint32_t oy = (uint32_t)pass_y_start[pass] + py * (uint32_t)pass_y_step[pass];
                if (ox < st->width && oy < st->height) {
                    out_pixels[(size_t)oy * st->width + ox] = out_tmp[px];
                }
            }
            free(out_tmp);

            memcpy(prev, cur, row_bytes);
            pos += 1u + row_bytes;
        }

pass_done:
        free(prev);
        free(cur);
        if (rc != RPNG_OK) return rc;
    }

    if (pos > raw_size) return RPNG_ERR_FORMAT;
    return RPNG_OK;
}

int rpng_get_info(const uint8_t* data, size_t size, int* width, int* height) {
    RPNGState st;
    int rc;
    if (!data || !width || !height) return RPNG_ERR_PARAM;

    rpng_state_init(&st);
    rc = rpng_parse_chunks(data, size, &st);
    if (rc == RPNG_OK) {
        if (!st.has_ihdr || st.width > 0x7fffffffU || st.height > 0x7fffffffU) {
            rc = RPNG_ERR_FORMAT;
        } else {
            *width = (int)st.width;
            *height = (int)st.height;
        }
    }
    rpng_state_free(&st);
    return rc;
}

int rpng_decode_rgba(const uint8_t* data, size_t size,
                     uint32_t* out_pixels, int out_width, int out_height) {
    RPNGState st;
    int rc;
    uint64_t expected_raw;
    size_t raw_cap;
    uint8_t* raw = NULL;
    size_t out_size = 0;

    if (!data || !out_pixels || out_width <= 0 || out_height <= 0) return RPNG_ERR_PARAM;

    rpng_state_init(&st);
    rc = rpng_parse_chunks(data, size, &st);
    if (rc != RPNG_OK) goto done;

    if (!st.has_ihdr || !st.has_iend || st.idat_size == 0) {
        rc = RPNG_ERR_FORMAT;
        goto done;
    }
    if ((uint32_t)out_width != st.width || (uint32_t)out_height != st.height) {
        rc = RPNG_ERR_PARAM;
        goto done;
    }
    if (st.color_type == 3 && (!st.palette || st.palette_size == 0)) {
        rc = RPNG_ERR_FORMAT;
        goto done;
    }

    expected_raw = rpng_expected_raw_size(&st);
    if (expected_raw == 0 || expected_raw > (uint64_t)(1u << 31)) {
        rc = RPNG_ERR_UNSUPPORTED;
        goto done;
    }
    raw_cap = (size_t)expected_raw;
    raw = (uint8_t*)malloc(raw_cap);
    if (!raw) {
        rc = RPNG_ERR_NOMEM;
        goto done;
    }

    rc = rinz_inflate(st.idat, st.idat_size, raw, raw_cap, &out_size);
    if (rc == RINZ_BUF_ERROR) {
        /* Retry once with a larger buffer for non-conforming streams. */
        size_t larger_cap = raw_cap * 2u;
        if (larger_cap > (size_t)(1u << 28)) {
            rc = RPNG_ERR_DECOMPRESS;
            goto done;
        }
        free(raw);
        raw = (uint8_t*)malloc(larger_cap);
        if (!raw) {
            rc = RPNG_ERR_NOMEM;
            goto done;
        }
        raw_cap = larger_cap;
        rc = rinz_inflate(st.idat, st.idat_size, raw, raw_cap, &out_size);
    }

    if (rc != RINZ_OK) {
        rc = RPNG_ERR_DECOMPRESS;
        goto done;
    }
    if (out_size < (size_t)expected_raw) {
        rc = RPNG_ERR_FORMAT;
        goto done;
    }

    if (st.interlace == 0) {
        rc = rpng_decode_non_interlaced(&st, raw, out_size, out_pixels);
    } else {
        rc = rpng_decode_adam7(&st, raw, out_size, out_pixels);
    }

done:
    if (raw) free(raw);
    rpng_state_free(&st);
    return rc;
}
