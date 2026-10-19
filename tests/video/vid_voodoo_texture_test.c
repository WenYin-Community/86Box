/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Byte-exactness tests for the AVX-512 Voodoo texture decoders.
 *
 *          The expected values are produced by a scalar reference that
 *          copies the expressions from the scalar switch in
 *          vid_voodoo_texture.c, including the lookup tables built by
 *          voodoo_init.  The vector row decoders are required to match
 *          them byte for byte for every texture format and width.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2026 86Box contributors.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/include/86box/vid_voodoo_texture_avx512.h"

#if !defined(VOODOO_TEX_AVX512_ENABLED)
#    include <stdio.h>

int
main(void)
{
    printf("AVX-512 decode not compiled in, skipping.\n");
    return 77;
}
#else

#    define MAKERGBA(r, g, b, a) ((b) | ((g) << 8) | ((r) << 16) | ((a) << 24))

typedef union {
    struct {
        uint8_t b;
        uint8_t g;
        uint8_t r;
        uint8_t a;
    } rgba;
    uint32_t u;
} ref_rgba_u;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} ref_rgba8_t;

static ref_rgba8_t ref_rgb332_table[256];
static ref_rgba8_t ref_rgb565_table[65536];
static ref_rgba8_t ref_argb1555_table[65536];
static ref_rgba8_t ref_argb4444_table[65536];
static ref_rgba_u  ref_palette[256];
static ref_rgba_u  ref_ncc[256];

static int failures;

#    define CHECK(cond)                                                    \
        do {                                                               \
            if (!(cond)) {                                                 \
                fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
                failures++;                                                \
            }                                                              \
        } while (0)

static void
init_tables(void)
{
    int c;

    for (c = 0; c < 0x100; c++) {
        ref_rgb332_table[c].r = c & 0xe0;
        ref_rgb332_table[c].g = (c << 3) & 0xe0;
        ref_rgb332_table[c].b = (c << 6) & 0xc0;
        ref_rgb332_table[c].r = ref_rgb332_table[c].r | (ref_rgb332_table[c].r >> 3) | (ref_rgb332_table[c].r >> 6);
        ref_rgb332_table[c].g = ref_rgb332_table[c].g | (ref_rgb332_table[c].g >> 3) | (ref_rgb332_table[c].g >> 6);
        ref_rgb332_table[c].b = ref_rgb332_table[c].b | (ref_rgb332_table[c].b >> 2);
        ref_rgb332_table[c].b = ref_rgb332_table[c].b | (ref_rgb332_table[c].b >> 4);
        ref_rgb332_table[c].a = 0xff;

        ref_palette[c].rgba.b = (uint8_t) (c * 37 + 11);
        ref_palette[c].rgba.g = (uint8_t) (c * 91 + 23);
        ref_palette[c].rgba.r = (uint8_t) (c * 53 + 47);
        ref_palette[c].rgba.a = (uint8_t) (c * 17 + 5);

        ref_ncc[c].rgba.b = (uint8_t) (c * 29 + 3);
        ref_ncc[c].rgba.g = (uint8_t) (c * 71 + 19);
        ref_ncc[c].rgba.r = (uint8_t) (c * 113 + 31);
        ref_ncc[c].rgba.a = 0xff;
    }

    for (c = 0; c < 0x10000; c++) {
        ref_rgb565_table[c].r = (c >> 8) & 0xf8;
        ref_rgb565_table[c].g = (c >> 3) & 0xfc;
        ref_rgb565_table[c].b = (c << 3) & 0xf8;
        ref_rgb565_table[c].r |= (ref_rgb565_table[c].r >> 5);
        ref_rgb565_table[c].g |= (ref_rgb565_table[c].g >> 6);
        ref_rgb565_table[c].b |= (ref_rgb565_table[c].b >> 5);
        ref_rgb565_table[c].a = 0xff;

        ref_argb1555_table[c].r = (c >> 7) & 0xf8;
        ref_argb1555_table[c].g = (c >> 2) & 0xf8;
        ref_argb1555_table[c].b = (c << 3) & 0xf8;
        ref_argb1555_table[c].r |= (ref_argb1555_table[c].r >> 5);
        ref_argb1555_table[c].g |= (ref_argb1555_table[c].g >> 5);
        ref_argb1555_table[c].b |= (ref_argb1555_table[c].b >> 5);
        ref_argb1555_table[c].a = (c & 0x8000) ? 0xff : 0;

        ref_argb4444_table[c].a = (c >> 8) & 0xf0;
        ref_argb4444_table[c].r = (c >> 4) & 0xf0;
        ref_argb4444_table[c].g = c & 0xf0;
        ref_argb4444_table[c].b = (c << 4) & 0xf0;
        ref_argb4444_table[c].a |= (ref_argb4444_table[c].a >> 4);
        ref_argb4444_table[c].r |= (ref_argb4444_table[c].r >> 4);
        ref_argb4444_table[c].g |= (ref_argb4444_table[c].g >> 4);
        ref_argb4444_table[c].b |= (ref_argb4444_table[c].b >> 4);
    }
}

/* Scalar references, one per format, copied from vid_voodoo_texture.c. */
static uint32_t
ref_a8(uint32_t dat, const uint32_t *table)
{
    (void) table;
    return MAKERGBA(dat, dat, dat, dat);
}

static uint32_t
ref_i8(uint32_t dat, const uint32_t *table)
{
    (void) table;
    return MAKERGBA(dat, dat, dat, 0xff);
}

static uint32_t
ref_ai8(uint32_t dat, const uint32_t *table)
{
    (void) table;
    return MAKERGBA((dat & 0x0f) | ((dat << 4) & 0xf0), (dat & 0x0f) | ((dat << 4) & 0xf0),
                    (dat & 0x0f) | ((dat << 4) & 0xf0), (dat & 0xf0) | ((dat >> 4) & 0x0f));
}

static uint32_t
ref_rgb332(uint32_t dat, const uint32_t *table)
{
    (void) table;
    return MAKERGBA(ref_rgb332_table[dat].r, ref_rgb332_table[dat].g, ref_rgb332_table[dat].b, 0xff);
}

static uint32_t
ref_y4i2q2(uint32_t dat, const uint32_t *table)
{
    const ref_rgba_u *pal = (const ref_rgba_u *) table;
    return MAKERGBA(pal[dat].rgba.r, pal[dat].rgba.g, pal[dat].rgba.b, 0xff);
}

static uint32_t
ref_pal8(uint32_t dat, const uint32_t *table)
{
    const ref_rgba_u *pal = (const ref_rgba_u *) table;
    return MAKERGBA(pal[dat].rgba.r, pal[dat].rgba.g, pal[dat].rgba.b, 0xff);
}

static uint32_t
ref_apal8(uint32_t dat, const uint32_t *table)
{
    const ref_rgba_u *pal = (const ref_rgba_u *) table;
    int r = ((pal[dat].rgba.r & 3) << 6) | ((pal[dat].rgba.g & 0xf0) >> 2) | (pal[dat].rgba.r & 3);
    int g = ((pal[dat].rgba.g & 0xf) << 4) | ((pal[dat].rgba.b & 0xc0) >> 4) | ((pal[dat].rgba.g & 0xf) >> 2);
    int b = ((pal[dat].rgba.b & 0x3f) << 2) | ((pal[dat].rgba.b & 0x30) >> 4);
    int a = (pal[dat].rgba.r & 0xfc) | ((pal[dat].rgba.r & 0xc0) >> 6);
    return MAKERGBA(r, g, b, a);
}

static uint32_t
ref_argb8332(uint32_t dat, const uint32_t *table)
{
    (void) table;
    return MAKERGBA(ref_rgb332_table[dat & 0xff].r, ref_rgb332_table[dat & 0xff].g, ref_rgb332_table[dat & 0xff].b, dat >> 8);
}

static uint32_t
ref_a8y4i2q2(uint32_t dat, const uint32_t *table)
{
    const ref_rgba_u *pal = (const ref_rgba_u *) table;
    return MAKERGBA(pal[dat & 0xff].rgba.r, pal[dat & 0xff].rgba.g, pal[dat & 0xff].rgba.b, dat >> 8);
}

static uint32_t
ref_r5g6b5(uint32_t dat, const uint32_t *table)
{
    (void) table;
    return MAKERGBA(ref_rgb565_table[dat].r, ref_rgb565_table[dat].g, ref_rgb565_table[dat].b, 0xff);
}

static uint32_t
ref_argb1555(uint32_t dat, const uint32_t *table)
{
    (void) table;
    return MAKERGBA(ref_argb1555_table[dat].r, ref_argb1555_table[dat].g, ref_argb1555_table[dat].b, ref_argb1555_table[dat].a);
}

static uint32_t
ref_argb4444(uint32_t dat, const uint32_t *table)
{
    (void) table;
    return MAKERGBA(ref_argb4444_table[dat].r, ref_argb4444_table[dat].g, ref_argb4444_table[dat].b, ref_argb4444_table[dat].a);
}

static uint32_t
ref_a8i8(uint32_t dat, const uint32_t *table)
{
    (void) table;
    return MAKERGBA(dat & 0xff, dat & 0xff, dat & 0xff, dat >> 8);
}

static uint32_t
ref_apal88(uint32_t dat, const uint32_t *table)
{
    const ref_rgba_u *pal = (const ref_rgba_u *) table;
    return MAKERGBA(pal[dat & 0xff].rgba.r, pal[dat & 0xff].rgba.g, pal[dat & 0xff].rgba.b, dat >> 8);
}

typedef void (*vec8_fn)(const uint8_t *, uint32_t *, int);
typedef void (*vec8_table_fn)(const uint8_t *, uint32_t *, int, const uint32_t *);
typedef void (*vec16_fn)(const uint8_t *, uint32_t *, int);
typedef void (*vec16_table_fn)(const uint8_t *, uint32_t *, int, const uint32_t *);
typedef uint32_t (*ref_fn)(uint32_t, const uint32_t *);

static void
test8(const char *name, vec8_fn vec, ref_fn ref, const uint8_t *src, int width, int height, const uint32_t *table)
{
    uint32_t *out = malloc((size_t) width * sizeof(uint32_t));

    for (int y = 0; y < height; y++) {
        vec(src + (size_t) y * width, out, width);
        for (int x = 0; x < width; x++) {
            uint32_t expected = ref(src[(size_t) y * width + x], table);
            if (out[x] != expected) {
                fprintf(stderr, "%s: width %d row %d col %d: got %08x expected %08x\n",
                        name, width, y, x, out[x], expected);
                failures++;
                free(out);
                return;
            }
        }
    }

    free(out);
}

static void
test8_table(const char *name, vec8_table_fn vec, ref_fn ref, const uint8_t *src, int width, int height, const uint32_t *table)
{
    uint32_t *out = malloc((size_t) width * sizeof(uint32_t));

    for (int y = 0; y < height; y++) {
        vec(src + (size_t) y * width, out, width, table);
        for (int x = 0; x < width; x++) {
            uint32_t expected = ref(src[(size_t) y * width + x], table);
            if (out[x] != expected) {
                fprintf(stderr, "%s: width %d row %d col %d: got %08x expected %08x\n",
                        name, width, y, x, out[x], expected);
                failures++;
                free(out);
                return;
            }
        }
    }

    free(out);
}

static void
test16(const char *name, vec16_fn vec, ref_fn ref, const uint8_t *src, int width, int height, const uint32_t *table)
{
    uint32_t *out = malloc((size_t) width * sizeof(uint32_t));

    for (int y = 0; y < height; y++) {
        vec(src + (size_t) y * width * 2, out, width);
        for (int x = 0; x < width; x++) {
            uint16_t dat;
            memcpy(&dat, src + ((size_t) y * width + x) * 2, sizeof(dat));
            uint32_t expected = ref(dat, table);
            if (out[x] != expected) {
                fprintf(stderr, "%s: width %d row %d col %d: got %08x expected %08x\n",
                        name, width, y, x, out[x], expected);
                failures++;
                free(out);
                return;
            }
        }
    }

    free(out);
}

static void
test16_table(const char *name, vec16_table_fn vec, ref_fn ref, const uint8_t *src, int width, int height, const uint32_t *table)
{
    uint32_t *out = malloc((size_t) width * sizeof(uint32_t));

    for (int y = 0; y < height; y++) {
        vec(src + (size_t) y * width * 2, out, width, table);
        for (int x = 0; x < width; x++) {
            uint16_t dat;
            memcpy(&dat, src + ((size_t) y * width + x) * 2, sizeof(dat));
            uint32_t expected = ref(dat, table);
            if (out[x] != expected) {
                fprintf(stderr, "%s: width %d row %d col %d: got %08x expected %08x\n",
                        name, width, y, x, out[x], expected);
                failures++;
                free(out);
                return;
            }
        }
    }

    free(out);
}

int
main(void)
{
    static const int widths[] = { 16, 32, 64, 128, 256 };
    uint8_t  src8[256];
    uint8_t  src16[65536 * 2];
    const uint32_t *pal = (const uint32_t *) ref_palette;
    const uint32_t *ncc = (const uint32_t *) ref_ncc;

    if (!voodoo_tex_avx512_available()) {
        printf("Host lacks AVX-512F/BW/DQ/VL, skipping.\n");
        return 77;
    }

    init_tables();

    /* Every byte value, so the 256-wide pass is exhaustive for 8-bit input. */
    for (int i = 0; i < 256; i++)
        src8[i] = (uint8_t) i;

    /* Every 16-bit value, laid out as 256 rows of 256 texels. */
    for (int i = 0; i < 65536; i++) {
        uint16_t v = (uint16_t) i;
        memcpy(&src16[i * 2], &v, sizeof(v));
    }

    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        int width = widths[w];
        int rows  = 256 / width;

        test8("TEX_A8", voodoo_tex_avx512_a8, ref_a8, src8, width, rows, NULL);
        test8("TEX_I8", voodoo_tex_avx512_i8, ref_i8, src8, width, rows, NULL);
        test8("TEX_AI8", voodoo_tex_avx512_ai8, ref_ai8, src8, width, rows, NULL);
        test8("TEX_RGB332", voodoo_tex_avx512_rgb332, ref_rgb332, src8, width, rows, NULL);
        test8_table("TEX_Y4I2Q2", voodoo_tex_avx512_y4i2q2, ref_y4i2q2, src8, width, rows, ncc);
        test8_table("TEX_PAL8", voodoo_tex_avx512_pal8, ref_pal8, src8, width, rows, pal);
        test8_table("TEX_APAL8", voodoo_tex_avx512_apal8, ref_apal8, src8, width, rows, pal);

        test16("TEX_ARGB8332", voodoo_tex_avx512_argb8332, ref_argb8332, src16, width, 65536 / width, NULL);
        test16_table("TEX_A8Y4I2Q2", voodoo_tex_avx512_a8y4i2q2, ref_a8y4i2q2, src16, width, 65536 / width, ncc);
        test16("TEX_R5G6B5", voodoo_tex_avx512_r5g6b5, ref_r5g6b5, src16, width, 65536 / width, NULL);
        test16("TEX_ARGB1555", voodoo_tex_avx512_argb1555, ref_argb1555, src16, width, 65536 / width, NULL);
        test16("TEX_ARGB4444", voodoo_tex_avx512_argb4444, ref_argb4444, src16, width, 65536 / width, NULL);
        test16("TEX_A8I8", voodoo_tex_avx512_a8i8, ref_a8i8, src16, width, 65536 / width, NULL);
        test16_table("TEX_APAL88", voodoo_tex_avx512_apal88, ref_apal88, src16, width, 65536 / width, pal);
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    printf("Voodoo texture decode: all formats match the scalar reference.\n");
    return 0;
}
#endif
