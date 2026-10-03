// emugcxbox360 - interface to a hardware rasterizer backend
// SPDX-License-Identifier: GPL-2.0-or-later
//
// When a backend is installed, the core keeps doing command processing,
// vertex transform, lighting, clipping, culling and texture decoding, and the
// backend rasterizes: TEV, fog, alpha test, depth and blending. The core keeps
// a CPU copy of the EFB which it refreshes from the backend (ReadEFB) only
// when something needs it (EFB copies, CPU peeks).
#pragma once

#include "core/common.h"

namespace Video {

struct GpuVertex {
  float x, y;     // EFB pixel coordinates (pixel centres at +0.5)
  float z;        // depth, 0..1 (GX 24-bit depth / 16777215)
  float w;        // clip-space w (1 for orthographic projections)
  float color[2][4];  // RGBA 0..1
  float tex[8][3];    // texture coordinates in texels (s, t, q)
};

// A decoded texture bound to one of the 8 texture units.
struct GpuTexture {
  const u8* rgba;  // level 0, RGBA8, rows of `width` texels; nullptr: unbound
  int width, height;
  u32 id;          // changes whenever the decoded content changes
  u32 wrap_s, wrap_t;  // GX: 0 clamp, 1 repeat, 2 mirror
  bool linear;     // bilinear magnification/minification
};

struct GpuDrawState {
  u32 bp[0x100];        // BP register file at the draw
  s16 tev_regs[4][4];   // TEV color registers (initial values), RGBA
  s16 tev_konst[4][4];  // TEV konst registers
  GpuTexture tex[8];
  u32 tex_mask;         // units used by the draw
  int sc_left, sc_top, sc_right, sc_bottom;  // scissor rectangle in EFB pixels (right/bottom exclusive)
};

class GpuBackend {
 public:
  virtual ~GpuBackend() = default;
  // Triangle list (3 vertices per triangle), already clipped and culled.
  virtual void Draw(const GpuDrawState& state, const GpuVertex* v, u32 count) = 0;
  // Fills a rectangle of the EFB (EFB pixels, right/bottom exclusive).
  virtual void ClearEFB(int x0, int y0, int x1, int y1, bool color, bool alpha, bool depth, u32 rgba, u32 z24) = 0;
  // Reads the whole EFB: color as 0xRRGGBBAA, depth as 24-bit values.
  virtual void ReadEFB(u32* color, u32* depth) = 0;
  // Copy of the EFB rectangle (sx, sy, w, h) to the external framebuffer at
  // guest address `dest`, `out_h` lines after vertical scaling. A backend may
  // keep it on the GPU and present it directly (PresentXFB); returning false
  // selects the CPU path (YUV conversion into guest RAM).
  virtual bool CopyToXFB(u32 dest, int sx, int sy, int w, int h, int out_h) { return false; }
  // Shows the frame copied to `addr` (the VI's top field address) as a
  // `width` x `height` picture (the VI's scan-out size, cropped/padded);
  // false when the backend has nothing for it.
  virtual bool PresentXFB(u32 addr, int width, int height) { return false; }
};

// nullptr: the built-in software rasterizer is used.
extern GpuBackend* g_gpu;

}  // namespace Video
