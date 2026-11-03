// The BC7 texture cache (bc7_cache.c) and its encoder (bc7_encode.cpp). Ported from UltraTears (kuraeow/ultratears,
// GPL-3.0, commit 57bf619: src/gfx/vkb_bc7.c, src/gfx/tex/bc7key.c, src/gfx/tex/bc7.cpp, tools/bc7build/bc7build.c);
// the encoder is bc7enc and rgbcx (Rich Geldreich, MIT, third_party/bc7enc).
//
// GPUs without ASTC get TotK's ASTC textures decoded to RGBA8: 4x the memory and bandwidth of BC7. The cache holds
// BC7 (or BC1) blocks keyed by the ASTC blocks of one surface (one level of one layer) in rows, as upload deswizzles
// them, with the size and block size. A texture whose every surface is in the cache becomes a BC7 / BC1 image.
// The key hashes our own deswizzled rows, so a builder that lays a texture out differently only causes misses.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- bc7_encode.cpp: RGBA8 rows (w * 4 bytes) of one slice into blocks; past the edge the last row / column repeats
size_t bc7_size(uint32_t w, uint32_t h);   // 16 bytes per 4x4 block
size_t bc1_size(uint32_t w, uint32_t h);   // 8 bytes per 4x4 block
// perceptual: colour weights (sRGB textures). quality: bc7enc's uber level 0 (fastest) .. 4.
void bc7_encode(const uint8_t *rgba, uint32_t w, uint32_t h, int perceptual, int quality, uint8_t *out);
void bc1_encode(const uint8_t *rgba, uint32_t w, uint32_t h, int level, uint8_t *out);   // opaque; rgbcx level 0..18

// ---- bc7_cache.c
enum { BC7_MISS = 0, BC7_FMT_BC7 = 1, BC7_FMT_BC1 = 2 };
typedef struct { uint64_t a, b; } Bc7Key;
int bc7_on(void);   // TOTK_BC7 (default 1)
Bc7Key bc7_key_of(const uint8_t *blocks, size_t n, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh);
int bc7_lookup(Bc7Key k);   // BC7_FMT_* or BC7_MISS
// The cached blocks of k when they are n bytes of format fmt; 0 otherwise.
int bc7_read(Bc7Key k, int fmt, uint8_t *out, size_t n);
// A miss on an RGBA8 upload: rows (ASTC blocks) copied, decoded and encoded to BC7 by a low-priority thread, so the
// texture is BC7 the next time it is created.
void bc7_queue(Bc7Key k, const uint8_t *rows, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, int srgb);
// A miss on a BC7 / BC1 image (its storage changed): decoded and encoded now into out; a BC7 result is cached.
void bc7_encode_now(Bc7Key k, const uint8_t *rows, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, int srgb, int fmt,
                    uint8_t *out);
int bc7_build(void);   // totk --bc7-build: fills the cache from the RomFS (fs_init first)

#ifdef __cplusplus
}
#endif
