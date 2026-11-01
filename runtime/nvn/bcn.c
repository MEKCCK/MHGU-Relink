// BC1-BC7 decoded on the CPU, for GPUs without BC texture support (phones: nvn/vulkan/textures.c, as astc.c does for
// ASTC). third_party/bcdec.h (MIT / Unlicense) decodes the blocks.
#include "bcn.h"

#include <stdlib.h>
#include <string.h>

#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../../third_party/bcdec.h"

uint32_t bcn_block_bytes(int kind) {
    return kind == BCN_BC1 || kind == BCN_BC4U || kind == BCN_BC4S ? 8 : 16;
}

uint32_t bcn_texel_bytes(int kind) { return kind == BCN_BC6HS || kind == BCN_BC6HU ? 8 : 4; }

// One block into 4x4 texels of the output format (RGBA8, or RGBA16F for BC6H), rows `pitch` bytes apart.
static void block(int kind, const uint8_t *b, uint8_t *o, int pitch, uint32_t tb) {
    switch (kind) {
    case BCN_BC1: bcdec_bc1(b, o, pitch); break;
    case BCN_BC2: bcdec_bc2(b, o, pitch); break;
    case BCN_BC3: bcdec_bc3(b, o, pitch); break;
    case BCN_BC7: bcdec_bc7(b, o, pitch); break;
    case BCN_BC6HS: case BCN_BC6HU: {  // RGB halfs (the pitch in halfs) -> RGBA16F with A = 1.0
        uint16_t rgb[48];
        bcdec_bc6h_half(b, rgb, 12, kind == BCN_BC6HS);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                uint16_t *p = (uint16_t *)(void *)(o + y * pitch + x * 8);
                p[0] = rgb[y * 12 + x * 3], p[1] = rgb[y * 12 + x * 3 + 1], p[2] = rgb[y * 12 + x * 3 + 2], p[3] = 0x3C00;
            }
        break;
    }
    case BCN_BC4U: case BCN_BC4S: {  // one channel: R, then G = B = 0, A = 1 (what sampling R8 returns)
        uint8_t r[16];
        bcdec_bc4(b, r, 4, kind == BCN_BC4S);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                uint8_t *p = o + y * pitch + x * tb;
                p[0] = r[y * 4 + x];
                if (tb == 4) p[1] = 0, p[2] = 0, p[3] = kind == BCN_BC4S ? 127 : 255;
            }
        break;
    }
    case BCN_BC5U: case BCN_BC5S: {
        uint8_t rg[32];
        bcdec_bc5(b, rg, 8, kind == BCN_BC5S);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                uint8_t *p = o + y * pitch + x * tb;
                p[0] = rg[y * 8 + x * 2], p[1] = rg[y * 8 + x * 2 + 1];
                if (tb == 4) p[2] = 0, p[3] = kind == BCN_BC5S ? 127 : 255;
            }
        break;
    }
    default: break;
    }
}

void bcn_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out, uint32_t tb) {
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4, bb = bcn_block_bytes(kind);
    // whole blocks into a padded image, then the visible part into out (width x height, tightly packed)
    uint32_t pw = bw * 4, pitch = pw * tb;
    uint8_t *pad = malloc((size_t)pitch * bh * 4);
    if (!pad) return;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t *o = pad + (size_t)by * 4 * pitch + (size_t)bx * 4 * tb;
            block(kind, blocks + ((size_t)by * bw + bx) * bb, o, (int)pitch, tb);
        }
    for (uint32_t y = 0; y < height; y++) memcpy(out + (size_t)y * width * tb, pad + (size_t)y * pitch, (size_t)width * tb);
    free(pad);
}
