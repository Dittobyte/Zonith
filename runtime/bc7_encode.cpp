// BC7 and BC1 encoding of RGBA8 texels: bc7enc and rgbcx (third_party/bc7enc, Rich Geldreich, MIT) block by block.
// After UltraTears src/gfx/tex/bc7.cpp (kuraeow/ultratears, GPL-3.0, commit 57bf619), one slice per call.
#include <string.h>

#include "bc7.h"
#include "third_party/bc7enc/bc7enc.h"
#include "third_party/bc7enc/rgbcx.h"

static void init_once() {
    static const bool done = (bc7enc_compress_block_init(), rgbcx::init(), true);   // their tables (C++ static: once)
    (void)done;
}

// The 4x4 texels of block (x, y); past the edge the last row / column repeats.
static void block_px(const uint8_t *s, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint8_t px[64]) {
    for (uint32_t j = 0; j < 4; j++) {
        uint32_t sy = y * 4 + j < h ? y * 4 + j : h - 1;
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t sx = x * 4 + i < w ? x * 4 + i : w - 1;
            memcpy(px + (j * 4 + i) * 4, s + ((size_t)sy * w + sx) * 4, 4);
        }
    }
}

extern "C" size_t bc7_size(uint32_t w, uint32_t h) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16; }
extern "C" size_t bc1_size(uint32_t w, uint32_t h) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8; }

extern "C" void bc1_encode(const uint8_t *rgba, uint32_t w, uint32_t h, int level, uint8_t *out) {
    init_once();
    for (uint32_t y = 0; y < (h + 3) / 4; y++)
        for (uint32_t x = 0; x < (w + 3) / 4; x++, out += 8) {
            uint8_t px[64];
            block_px(rgba, w, h, x, y, px);
            rgbcx::encode_bc1((uint32_t)level, out, px, false, false);   // 4-colour blocks: opaque
        }
}

extern "C" void bc7_encode(const uint8_t *rgba, uint32_t w, uint32_t h, int perceptual, int quality, uint8_t *out) {
    init_once();
    bc7enc_compress_block_params p;
    bc7enc_compress_block_params_init(&p);
    if (!perceptual) bc7enc_compress_block_params_init_linear_weights(&p);
    p.m_uber_level = (uint32_t)(quality < 0 ? 0 : quality > BC7ENC_MAX_UBER_LEVEL ? BC7ENC_MAX_UBER_LEVEL : quality);
    for (uint32_t y = 0; y < (h + 3) / 4; y++)
        for (uint32_t x = 0; x < (w + 3) / 4; x++, out += 16) {
            uint8_t px[64];
            block_px(rgba, w, h, x, y, px);
            bc7enc_compress_block(out, px, &p);
        }
}
