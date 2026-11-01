// BCn texture blocks decoded on the CPU (runtime/nvn/bcn.c): for GPUs without BC support, such as phones'.
#pragma once
#include <stdint.h>

enum { BCN_NONE = 0, BCN_BC1, BCN_BC2, BCN_BC3, BCN_BC4U, BCN_BC4S, BCN_BC5U, BCN_BC5S, BCN_BC7, BCN_BC6HS, BCN_BC6HU };

uint32_t bcn_block_bytes(int kind);  // 8 or 16
uint32_t bcn_texel_bytes(int kind);  // of the output: 4 (RGBA8 / RGBA8 SNORM), 8 for BC6H (RGBA16F), BC4 1 and BC5 2 narrow
// Blocks in rows (ceil(width/4) per row) -> width x height texels of tb bytes each, tightly packed: bcn_texel_bytes,
// or for BC4 and BC5 as well 1 (R8) and 2 (RG8), a quarter and a half of the memory, sampled the same
void bcn_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out, uint32_t tb);
