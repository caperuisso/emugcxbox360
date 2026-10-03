// emugcxbox360 - translation of GX draws into Xenos shaders and render state
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared by the real Xenos backend and the PC simulator (which executes the
// generated microcode), so the translation can be verified without a console.
#pragma once

#include <vector>

#include "core/video/gpu_backend.h"
#include "xenos/shaders.h"

namespace XenosGx {

// Xenos blend factors / ops / compare functions (libxenon XE_* values)
enum : u32 {
  BLEND_ZERO = 0, BLEND_ONE = 1, BLEND_SRCCOLOR = 4, BLEND_INVSRCCOLOR = 5, BLEND_SRCALPHA = 6,
  BLEND_INVSRCALPHA = 7, BLEND_DESTCOLOR = 8, BLEND_INVDESTCOLOR = 9, BLEND_DESTALPHA = 10, BLEND_INVDESTALPHA = 11,
};
enum : u32 { BLENDOP_ADD = 0, BLENDOP_SUBTRACT = 1, BLENDOP_REVSUBTRACT = 4 };

// EFB size: the render target is EFB-sized, normalized device coordinates
// map EFB pixels as x = (ndc + 1) * 320, y = (1 - ndc) * 264.
constexpr int EFB_W = 640, EFB_H = 528;

struct RenderState {
  bool z_enable, z_write;
  u32 z_func;  // GX and Xenos use the same numbering (0 never .. 7 always)
  u32 color_src, color_dst, color_op;
  u32 alpha_src, alpha_dst, alpha_op;
  int sc_left, sc_top, sc_right, sc_bottom;  // EFB pixels, right/bottom exclusive
};

struct ShaderEntry {
  Xenos::BuiltShader shader;
  void* backend_object = nullptr;  // owned by the backend (GPU copy of the code)
};

constexpr int NUM_PS_CONSTS = 40;

struct PreparedDraw {
  ShaderEntry* vs;
  ShaderEntry* ps;
  float ps_consts[NUM_PS_CONSTS][4];
  std::vector<float> vertices;  // stride floats per vertex
  u32 stride;                   // in floats (= dwords)
  u32 vertex_count;
  RenderState rs;
  u32 tex_mask;  // texture units sampled by the pixel shader (fetch constant = unit)
};

// Builds shaders (cached), constants, vertices and render state for a draw.
void Prepare(const Video::GpuDrawState& st, const Video::GpuVertex* v, u32 count, PreparedDraw& out);

// A quad filling an EFB rectangle with a color and/or depth (EFB clears).
void PrepareClear(int x0, int y0, int x1, int y1, bool color, bool alpha, bool depth, u32 rgba, u32 z24,
                  PreparedDraw& out);

// Number of distinct shaders built so far (statistics).
u32 ShaderCount();

}  // namespace XenosGx
