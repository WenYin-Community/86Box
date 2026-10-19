/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          3DFX Voodoo texture decode: AVX-512 row decoders.
 *
 *          These helpers expand one row of texels into the RGBA32 layout
 *          used by the software rasterizer.  They are bit-identical to the
 *          scalar code in vid_voodoo_texture.c: every operation is integer
 *          arithmetic, and the palette/NCC variants reproduce the packed
 *          rgba_u layout directly.
 *
 *          The whole header is compiled out unless VOODOO_TEX_AVX512 is
 *          enabled and the compiler supports function target attributes,
 *          so the baseline -msse2 build remains valid.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2026 86Box contributors.
 */
#ifndef VOODOO_TEXTURE_AVX512_H
#define VOODOO_TEXTURE_AVX512_H

#include <stdint.h>

#ifndef VOODOO_TEX_AVX512
#    define VOODOO_TEX_AVX512 1
#endif

#if VOODOO_TEX_AVX512 && (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
#    include <immintrin.h>

#    define VOODOO_TEX_AVX512_ENABLED 1
#    define VOODOO_TEX_AVX512_TARGET  __attribute__((target("avx512f,avx512bw,avx512dq,avx512vl")))

/* Determine once whether the host can execute the target-attributed helpers. */
static int
voodoo_tex_avx512_available(void)
{
    static int available = -1;

    if (available < 0) {
        __builtin_cpu_init();
        available = __builtin_cpu_supports("avx512f")
                 && __builtin_cpu_supports("avx512bw")
                 && __builtin_cpu_supports("avx512dq")
                 && __builtin_cpu_supports("avx512vl");
    }

    return available;
}

#    define VOODOO_TEX_ROW8(name, body)                                                    \
        VOODOO_TEX_AVX512_TARGET static void name(const uint8_t *src, uint32_t *dst, int count) \
        {                                                                                  \
            for (int x = 0; x < count; x += 16) {                                          \
                __m512i v = _mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i *) (src + x))); \
                body                                                                       \
                _mm512_storeu_si512((void *) (dst + x), v);                                \
            }                                                                              \
        }

#    define VOODOO_TEX_ROW16(name, body)                                                    \
        VOODOO_TEX_AVX512_TARGET static void name(const uint8_t *src, uint32_t *dst, int count) \
        {                                                                                   \
            for (int x = 0; x < count; x += 16) {                                           \
                __m512i v = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *) (src + x * 2))); \
                body                                                                        \
                _mm512_storeu_si512((void *) (dst + x), v);                                 \
            }                                                                               \
        }

#    define VOODOO_TEX_GATHER(name, table_arg, body)                                        \
        VOODOO_TEX_AVX512_TARGET static void name(const uint8_t *src, uint32_t *dst, int count, const uint32_t *table_arg) \
        {                                                                                   \
            for (int x = 0; x < count; x += 16) {                                           \
                __m512i v   = _mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i *) (src + x))); \
                __m512i idx = _mm512_and_si512(v, _mm512_set1_epi32(0xff));                 \
                __m512i t   = _mm512_i32gather_epi32(idx, (const void *) table_arg, 4);     \
                body                                                                        \
                _mm512_storeu_si512((void *) (dst + x), v);                                 \
            }                                                                               \
        }

#    define VOODOO_TEX_GATHER16(name, table_arg, body)                                      \
        VOODOO_TEX_AVX512_TARGET static void name(const uint8_t *src, uint32_t *dst, int count, const uint32_t *table_arg) \
        {                                                                                   \
            for (int x = 0; x < count; x += 16) {                                           \
                __m512i v   = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *) (src + x * 2))); \
                __m512i idx = _mm512_and_si512(v, _mm512_set1_epi32(0xff));                 \
                __m512i t   = _mm512_i32gather_epi32(idx, (const void *) table_arg, 4);     \
                body                                                                        \
                _mm512_storeu_si512((void *) (dst + x), v);                                 \
            }                                                                               \
        }

/* TEX_A8: makergba(dat, dat, dat, dat) */
VOODOO_TEX_ROW8(voodoo_tex_avx512_a8, {
    v = _mm512_mullo_epi32(v, _mm512_set1_epi32(0x01010101));
})

/* TEX_I8: makergba(dat, dat, dat, 0xff) */
VOODOO_TEX_ROW8(voodoo_tex_avx512_i8, {
    v = _mm512_mullo_epi32(v, _mm512_set1_epi32(0x01010101));
    v = _mm512_or_si512(v, _mm512_set1_epi32((int) 0xff000000));
})

/* TEX_AI8: replicated low nibble for RGB, replicated high nibble for alpha. */
VOODOO_TEX_ROW8(voodoo_tex_avx512_ai8, {
    __m512i lownib = _mm512_and_si512(v, _mm512_set1_epi32(0x0f));
    __m512i rgb    = _mm512_or_si512(lownib, _mm512_and_si512(_mm512_slli_epi32(v, 4), _mm512_set1_epi32(0xf0)));
    __m512i alpha  = _mm512_or_si512(_mm512_and_si512(v, _mm512_set1_epi32(0xf0)),
                                     _mm512_and_si512(_mm512_srli_epi32(v, 4), _mm512_set1_epi32(0x0f)));
    v = _mm512_or_si512(_mm512_mullo_epi32(rgb, _mm512_set1_epi32(0x010101)),
                        _mm512_slli_epi32(alpha, 24));
})

/* TEX_RGB332: expand 3-3-2 with bit replication, opaque alpha. */
VOODOO_TEX_ROW8(voodoo_tex_avx512_rgb332, {
    __m512i r = _mm512_and_si512(v, _mm512_set1_epi32(0xe0));
    __m512i g = _mm512_and_si512(_mm512_slli_epi32(v, 3), _mm512_set1_epi32(0xe0));
    __m512i b = _mm512_and_si512(_mm512_slli_epi32(v, 6), _mm512_set1_epi32(0xc0));
    r = _mm512_or_si512(r, _mm512_or_si512(_mm512_srli_epi32(r, 3), _mm512_srli_epi32(r, 6)));
    g = _mm512_or_si512(g, _mm512_or_si512(_mm512_srli_epi32(g, 3), _mm512_srli_epi32(g, 6)));
    /* The scalar table builds blue in two sequential steps. */
    b = _mm512_or_si512(b, _mm512_srli_epi32(b, 2));
    b = _mm512_or_si512(b, _mm512_srli_epi32(b, 4));
    v = _mm512_or_si512(_mm512_or_si512(b, _mm512_slli_epi32(g, 8)),
                        _mm512_or_si512(_mm512_slli_epi32(r, 16), _mm512_set1_epi32((int) 0xff000000)));
})

/* TEX_Y4I2Q2: NCC lookup, opaque alpha. */
VOODOO_TEX_GATHER(voodoo_tex_avx512_y4i2q2, ncc, {
    v = _mm512_or_si512(_mm512_and_si512(t, _mm512_set1_epi32(0x00ffffff)),
                        _mm512_set1_epi32((int) 0xff000000));
})

/* TEX_PAL8: palette lookup, opaque alpha. */
VOODOO_TEX_GATHER(voodoo_tex_avx512_pal8, pal, {
    v = _mm512_or_si512(_mm512_and_si512(t, _mm512_set1_epi32(0x00ffffff)),
                        _mm512_set1_epi32((int) 0xff000000));
})

/* TEX_APAL8: premultiplied palette entry rearrangement. */
VOODOO_TEX_GATHER(voodoo_tex_avx512_apal8, pal, {
    __m512i pr = _mm512_and_si512(_mm512_srli_epi32(t, 16), _mm512_set1_epi32(0xff));
    __m512i pg = _mm512_and_si512(_mm512_srli_epi32(t, 8), _mm512_set1_epi32(0xff));
    __m512i pb = _mm512_and_si512(t, _mm512_set1_epi32(0xff));
    __m512i ro = _mm512_or_si512(_mm512_slli_epi32(_mm512_and_si512(pr, _mm512_set1_epi32(0x03)), 6),
                                 _mm512_or_si512(_mm512_srli_epi32(_mm512_and_si512(pg, _mm512_set1_epi32(0xf0)), 2),
                                                 _mm512_and_si512(pr, _mm512_set1_epi32(0x03))));
    __m512i go = _mm512_or_si512(_mm512_slli_epi32(_mm512_and_si512(pg, _mm512_set1_epi32(0x0f)), 4),
                                 _mm512_or_si512(_mm512_srli_epi32(_mm512_and_si512(pb, _mm512_set1_epi32(0xc0)), 4),
                                                 _mm512_srli_epi32(_mm512_and_si512(pg, _mm512_set1_epi32(0x0f)), 2)));
    __m512i bo = _mm512_or_si512(_mm512_slli_epi32(_mm512_and_si512(pb, _mm512_set1_epi32(0x3f)), 2),
                                 _mm512_srli_epi32(_mm512_and_si512(pb, _mm512_set1_epi32(0x30)), 4));
    __m512i ao = _mm512_or_si512(_mm512_and_si512(pr, _mm512_set1_epi32(0xfc)),
                                 _mm512_srli_epi32(_mm512_and_si512(pr, _mm512_set1_epi32(0xc0)), 6));
    v = _mm512_or_si512(_mm512_or_si512(bo, _mm512_slli_epi32(go, 8)),
                        _mm512_or_si512(_mm512_slli_epi32(ro, 16), _mm512_slli_epi32(ao, 24)));
})

/* TEX_ARGB8332: RGB332 low byte, alpha from high byte. */
VOODOO_TEX_ROW16(voodoo_tex_avx512_argb8332, {
    __m512i low = _mm512_and_si512(v, _mm512_set1_epi32(0xff));
    __m512i r   = _mm512_and_si512(low, _mm512_set1_epi32(0xe0));
    __m512i g   = _mm512_and_si512(_mm512_slli_epi32(low, 3), _mm512_set1_epi32(0xe0));
    __m512i b   = _mm512_and_si512(_mm512_slli_epi32(low, 6), _mm512_set1_epi32(0xc0));
    __m512i a   = _mm512_and_si512(_mm512_srli_epi32(v, 8), _mm512_set1_epi32(0xff));
    r = _mm512_or_si512(r, _mm512_or_si512(_mm512_srli_epi32(r, 3), _mm512_srli_epi32(r, 6)));
    g = _mm512_or_si512(g, _mm512_or_si512(_mm512_srli_epi32(g, 3), _mm512_srli_epi32(g, 6)));
    /* The scalar table builds blue in two sequential steps. */
    b = _mm512_or_si512(b, _mm512_srli_epi32(b, 2));
    b = _mm512_or_si512(b, _mm512_srli_epi32(b, 4));
    v = _mm512_or_si512(_mm512_or_si512(b, _mm512_slli_epi32(g, 8)),
                        _mm512_or_si512(_mm512_slli_epi32(r, 16), _mm512_slli_epi32(a, 24)));
})

/* TEX_A8Y4I2Q2: NCC low byte, alpha from high byte. */
VOODOO_TEX_GATHER16(voodoo_tex_avx512_a8y4i2q2, ncc, {
    __m512i a = _mm512_and_si512(_mm512_srli_epi32(v, 8), _mm512_set1_epi32(0xff));
    v = _mm512_or_si512(_mm512_and_si512(t, _mm512_set1_epi32(0x00ffffff)),
                        _mm512_slli_epi32(a, 24));
})

/* TEX_R5G6B5: expand 5-6-5 with bit replication, opaque alpha. */
VOODOO_TEX_ROW16(voodoo_tex_avx512_r5g6b5, {
    __m512i r = _mm512_and_si512(_mm512_srli_epi32(v, 8), _mm512_set1_epi32(0xf8));
    __m512i g = _mm512_and_si512(_mm512_srli_epi32(v, 3), _mm512_set1_epi32(0xfc));
    __m512i b = _mm512_and_si512(_mm512_slli_epi32(v, 3), _mm512_set1_epi32(0xf8));
    r = _mm512_or_si512(r, _mm512_srli_epi32(r, 5));
    g = _mm512_or_si512(g, _mm512_srli_epi32(g, 6));
    b = _mm512_or_si512(b, _mm512_srli_epi32(b, 5));
    v = _mm512_or_si512(_mm512_or_si512(b, _mm512_slli_epi32(g, 8)),
                        _mm512_or_si512(_mm512_slli_epi32(r, 16), _mm512_set1_epi32((int) 0xff000000)));
})

/* TEX_ARGB1555: expand 5-5-5 with bit replication, alpha is the top bit. */
VOODOO_TEX_ROW16(voodoo_tex_avx512_argb1555, {
    __m512i r = _mm512_and_si512(_mm512_srli_epi32(v, 7), _mm512_set1_epi32(0xf8));
    __m512i g = _mm512_and_si512(_mm512_srli_epi32(v, 2), _mm512_set1_epi32(0xf8));
    __m512i b = _mm512_and_si512(_mm512_slli_epi32(v, 3), _mm512_set1_epi32(0xf8));
    __m512i a = _mm512_and_si512(_mm512_srai_epi32(_mm512_slli_epi32(v, 16), 31),
                                 _mm512_set1_epi32((int) 0xff000000));
    r = _mm512_or_si512(r, _mm512_srli_epi32(r, 5));
    g = _mm512_or_si512(g, _mm512_srli_epi32(g, 5));
    b = _mm512_or_si512(b, _mm512_srli_epi32(b, 5));
    v = _mm512_or_si512(_mm512_or_si512(b, _mm512_slli_epi32(g, 8)),
                        _mm512_or_si512(_mm512_slli_epi32(r, 16), a));
})

/* TEX_ARGB4444: expand 4-4-4-4 with bit replication. */
VOODOO_TEX_ROW16(voodoo_tex_avx512_argb4444, {
    __m512i a = _mm512_and_si512(_mm512_srli_epi32(v, 8), _mm512_set1_epi32(0xf0));
    __m512i r = _mm512_and_si512(_mm512_srli_epi32(v, 4), _mm512_set1_epi32(0xf0));
    __m512i g = _mm512_and_si512(v, _mm512_set1_epi32(0xf0));
    __m512i b = _mm512_and_si512(_mm512_slli_epi32(v, 4), _mm512_set1_epi32(0xf0));
    a = _mm512_or_si512(a, _mm512_srli_epi32(a, 4));
    r = _mm512_or_si512(r, _mm512_srli_epi32(r, 4));
    g = _mm512_or_si512(g, _mm512_srli_epi32(g, 4));
    b = _mm512_or_si512(b, _mm512_srli_epi32(b, 4));
    v = _mm512_or_si512(_mm512_or_si512(b, _mm512_slli_epi32(g, 8)),
                        _mm512_or_si512(_mm512_slli_epi32(r, 16), _mm512_slli_epi32(a, 24)));
})

/* TEX_A8I8: intensity low byte, alpha from high byte. */
VOODOO_TEX_ROW16(voodoo_tex_avx512_a8i8, {
    __m512i i = _mm512_and_si512(v, _mm512_set1_epi32(0xff));
    __m512i a = _mm512_and_si512(_mm512_srli_epi32(v, 8), _mm512_set1_epi32(0xff));
    v = _mm512_or_si512(_mm512_mullo_epi32(i, _mm512_set1_epi32(0x010101)),
                        _mm512_slli_epi32(a, 24));
})

/* TEX_APAL88: palette low byte, alpha from high byte. */
VOODOO_TEX_GATHER16(voodoo_tex_avx512_apal88, pal, {
    __m512i a = _mm512_and_si512(_mm512_srli_epi32(v, 8), _mm512_set1_epi32(0xff));
    v = _mm512_or_si512(_mm512_and_si512(t, _mm512_set1_epi32(0x00ffffff)),
                        _mm512_slli_epi32(a, 24));
})

#endif /* VOODOO_TEX_AVX512 && x86-64 && GCC/Clang */

#endif /* VOODOO_TEXTURE_AVX512_H */
