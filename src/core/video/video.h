// emugcxbox360 - GameCube GPU ("Flipper" GX) software implementation
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Register semantics and the arithmetic of transform, TEV and blending follow
// Dolphin's software renderer (GPL-2.0-or-later) and YAGCD.
#pragma once

#include "core/common.h"

namespace Video {

constexpr int EFB_WIDTH = 640;
constexpr int EFB_HEIGHT = 528;

// GPU register files
extern u32 g_bp[0x100];    // BP: raster, TEV, textures, pixel engine
extern u32 g_cp[0x100];    // CP: vertex descriptors, attribute formats, arrays
extern u32 g_xf[0x1058];   // XF: matrices, lights, transform state

void Reset();

// Entry points used by the command processor
void LoadCPReg(u8 reg, u32 value);
void LoadXF(u32 addr, u32 count, const u8* data);  // data: big-endian u32 words
void LoadIndexedXF(u32 array, u32 value);          // array 0..3 = A..D
void LoadBP(u32 value);                            // reg in bits 24..31
// Size in bytes of one vertex for the given VAT entry.
u32 VertexSize(u32 vat);
// cmd: primitive opcode byte (0x80..0xBF), data: count vertices.
void Draw(u8 cmd, u32 count, const u8* data);

// Hooks provided by the pixel-engine side (hw/gx.cpp)
void SignalDrawDone();
void SignalToken(u16 token, bool interrupt);

// EFB access for the CPU (0x08000000 region) and statistics
u32 PeekEFBColor(u32 x, u32 y);
u32 PeekEFBDepth(u32 x, u32 y);
void PokeEFBColor(u32 x, u32 y, u32 argb);
void PokeEFBDepth(u32 x, u32 y, u32 z);

struct Stats {
  u32 primitives, vertices, triangles, pixels, efb_copies, xfb_copies;
};
extern Stats g_stats;

}  // namespace Video
