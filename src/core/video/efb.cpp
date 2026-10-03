// SPDX-License-Identifier: GPL-2.0-or-later
// Embedded framebuffer: depth test, blending/logic ops, clears and copies to
// the external framebuffer (YUYV) or to textures in main memory.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/memory.h"
#include "core/state.h"
#include "core/video/gpu_backend.h"
#include "core/video/video_internal.h"

namespace Video {

GpuBackend* g_gpu = nullptr;

namespace {

u32* s_color = nullptr;  // RGBA packed as 0xRRGGBBAA
u32* s_depth = nullptr;  // 24-bit depth
bool s_mirror_stale = false;  // with a GPU backend: s_color/s_depth older than the GPU EFB

// Refreshes the CPU copy of the EFB from the GPU backend when needed.
void SyncMirror() {
  if (g_gpu && s_mirror_stale) {
    g_gpu->ReadEFB(s_color, s_depth);
    s_mirror_stale = false;
  }
}

inline u32 PixelFormat() { return Bits(g_bp[BP_ZCOMPARE], 0, 3); }

inline u32 Pack(const u8* c) { return ((u32)c[0] << 24) | ((u32)c[1] << 16) | ((u32)c[2] << 8) | c[3]; }
inline void Unpack(u32 v, u8* c) {
  c[0] = (u8)(v >> 24);
  c[1] = (u8)(v >> 16);
  c[2] = (u8)(v >> 8);
  c[3] = (u8)v;
}

// Applies the precision of the current EFB pixel format.
void Quantize(u8* c) {
  switch (PixelFormat()) {
    case 1:  // RGBA6
      for (int i = 0; i < 4; i++) c[i] = (u8)((c[i] & 0xFC) | (c[i] >> 6));
      break;
    case 2:  // RGB565
      c[0] = (u8)((c[0] & 0xF8) | (c[0] >> 5));
      c[1] = (u8)((c[1] & 0xFC) | (c[1] >> 6));
      c[2] = (u8)((c[2] & 0xF8) | (c[2] >> 5));
      c[3] = 255;
      break;
    default:  // RGB8: no alpha channel
      c[3] = 255;
      break;
  }
}

inline bool Compare(u32 func, u32 a, u32 b) {
  switch (func) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return true;
  }
}

u8 LogicOp(u32 op, u8 s, u8 d) {
  switch (op) {
    case 0: return 0;
    case 1: return s & d;
    case 2: return s & ~d;
    case 3: return s;
    case 4: return ~s & d;
    case 5: return d;
    case 6: return s ^ d;
    case 7: return s | d;
    case 8: return ~(s | d);
    case 9: return ~(s ^ d);
    case 10: return ~d;
    case 11: return s | ~d;
    case 12: return ~s;
    case 13: return ~s | d;
    case 14: return ~(s & d);
    default: return 255;
  }
}

// ---- Copies ----
void ReadEFB(int x, int y, u8* c) {
  x = std::clamp(x, 0, EFB_WIDTH - 1);
  y = std::clamp(y, 0, EFB_HEIGHT - 1);
  Unpack(s_color[y * EFB_WIDTH + x], c);
}

inline u8 Luma(const u8* c) { return (u8)std::min(255, ((66 * c[0] + 129 * c[1] + 25 * c[2] + 128) >> 8) + 16); }

// Source pixel for a texture copy, optionally box-filtered 2x2 (half scale).
void CopySource(int x, int y, bool half, bool depth, u8* c) {
  if (depth) {
    auto zat = [](int px, int py) {
      px = std::clamp(px, 0, EFB_WIDTH - 1);
      py = std::clamp(py, 0, EFB_HEIGHT - 1);
      return s_depth[py * EFB_WIDTH + px];
    };
    u32 z = zat(x, y);
    if (half) z = (zat(x, y) + zat(x + 1, y) + zat(x, y + 1) + zat(x + 1, y + 1)) / 4;
    // Depth copies expose the 24-bit value through the color channels.
    c[0] = (u8)(z >> 16);
    c[1] = (u8)(z >> 8);
    c[2] = (u8)z;
    c[3] = (u8)(z >> 16);
    return;
  }
  if (!half) {
    ReadEFB(x, y, c);
    return;
  }
  u8 a[4], b[4], d[4], e[4];
  ReadEFB(x, y, a);
  ReadEFB(x + 1, y, b);
  ReadEFB(x, y + 1, d);
  ReadEFB(x + 1, y + 1, e);
  for (int i = 0; i < 4; i++) c[i] = (u8)((a[i] + b[i] + d[i] + e[i] + 2) / 4);
}

// Encodes one texel into the destination texture block layout.
void CopyToTexture(u32 cmd, int sx, int sy, int w, int h) {
  u32 tpf = Bits(cmd, 3, 4);
  u32 fmt = (tpf >> 1) | ((tpf & 1) << 3);
  bool intensity = Bits(cmd, 15, 1);
  bool half = Bits(cmd, 9, 1);
  bool depth = PixelFormat() == 3;
  u32 dest = (Bits(g_bp[BP_EFB_ADDR], 0, 24) << 5) & 0x01FFFFFF;
  u32 stride = Bits(g_bp[BP_EFB_STRIDE], 0, 10) << 5;  // bytes per row of blocks
  if (half) {
    w /= 2;
    h /= 2;
  }
  // Block geometry per copy format
  int bw, bh, bpp;  // block width/height in texels, bits per texel
  switch (fmt) {
    case 0: bw = 8; bh = 8; bpp = 4; break;                 // R4 / I4
    case 1: case 7: case 8: case 9: case 10: bw = 8; bh = 4; bpp = 8; break;  // 8-bit formats
    case 2: bw = 8; bh = 4; bpp = 8; break;                 // RA4 / IA4
    case 3: case 4: case 5: case 11: case 12: bw = 4; bh = 4; bpp = 16; break;
    case 6: bw = 4; bh = 4; bpp = 32; break;                // RGBA8
    default: bw = 4; bh = 4; bpp = 16; break;
  }
  int blocks_x = (w + bw - 1) / bw;
  int blocks_y = (h + bh - 1) / bh;
  u32 block_bytes = (u32)(bw * bh * bpp / 8);
  (void)blocks_x;
  for (int by = 0; by < blocks_y; by++) {
    for (int bx = 0; bx * bw < w; bx++) {
      u32 block_addr = dest + by * stride + bx * block_bytes;
      u8* out = Mem::PhysPtr(block_addr, block_bytes);
      if (!out) continue;
      memset(out, 0, block_bytes);
      Mem::MarkWritten(block_addr, block_bytes);
      for (int ty = 0; ty < bh; ty++) {
        for (int tx = 0; tx < bw; tx++) {
          int px = bx * bw + tx, py = by * bh + ty;
          u8 c[4];
          int ex = half ? sx + px * 2 : sx + px, ey = half ? sy + py * 2 : sy + py;
          CopySource(ex, ey, half, depth, c);
          int idx = ty * bw + tx;
          u8 lum = intensity && !depth ? Luma(c) : c[0];
          switch (fmt) {
            case 0: {  // R4/I4
              u8 v = lum >> 4;
              out[idx >> 1] |= (idx & 1) ? v : (u8)(v << 4);
              break;
            }
            case 1: case 8: out[idx] = lum; break;      // R8 / I8
            case 7: out[idx] = c[3]; break;             // A8
            case 9: out[idx] = c[1]; break;             // G8
            case 10: out[idx] = c[2]; break;            // B8
            case 2: out[idx] = (u8)((c[3] & 0xF0) | (lum >> 4)); break;  // RA4 / IA4
            case 3: out[idx * 2] = c[3]; out[idx * 2 + 1] = lum; break;   // RA8 / IA8
            case 11: out[idx * 2] = c[0]; out[idx * 2 + 1] = c[1]; break;  // RG8
            case 12: out[idx * 2] = c[1]; out[idx * 2 + 1] = c[2]; break;  // GB8
            case 4: {  // RGB565
              u16 v = (u16)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
              StoreBE16(out + idx * 2, v);
              break;
            }
            case 5: {  // RGB5A3
              u16 v;
              if (c[3] >= 0xE0)
                v = (u16)(0x8000 | ((c[0] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[2] >> 3));
              else
                v = (u16)(((c[3] >> 5) << 12) | ((c[0] >> 4) << 8) | ((c[1] >> 4) << 4) | (c[2] >> 4));
              StoreBE16(out + idx * 2, v);
              break;
            }
            case 6:  // RGBA8: AR plane then GB plane
              out[idx * 2] = c[3];
              out[idx * 2 + 1] = c[0];
              out[32 + idx * 2] = c[1];
              out[32 + idx * 2 + 1] = c[2];
              break;
            default: break;
          }
        }
      }
    }
  }
}

u8 s_gamma[3][256];
bool s_gamma_ready = false;

void CopyToXFB(u32 cmd, int sx, int sy, int w, int h) {
  if (!s_gamma_ready) {
    const float g[3] = {1.0f, 1.7f, 2.2f};
    for (int k = 0; k < 3; k++)
      for (int i = 0; i < 256; i++) s_gamma[k][i] = (u8)std::lround(255.0 * std::pow(i / 255.0, 1.0 / g[k]));
    s_gamma_ready = true;
  }
  u32 dest = (Bits(g_bp[BP_EFB_ADDR], 0, 24) << 5) & 0x01FFFFFF;
  u32 stride = Bits(g_bp[BP_EFB_STRIDE], 0, 10) << 5;
  u32 yscale_reg = g_bp[BP_COPY_YSCALE] & 0x1FF;
  float yscale = 1.0f;
  if (yscale_reg) yscale = Bits(cmd, 10, 1) ? 256.0f / yscale_reg : yscale_reg / 256.0f;
  int out_h = std::min(1024, (int)(1.0f + (h - 1) * yscale));
  const u8* gamma = s_gamma[std::min<u32>(Bits(cmd, 7, 2), 2)];

  // Vertical filter: 7 coefficients over the previous, current and next line.
  u32 f0 = g_bp[BP_COPY_FILTER0], f1 = g_bp[BP_COPY_FILTER0 + 1];
  int coef[7] = {(int)Bits(f0, 0, 6), (int)Bits(f0, 6, 6), (int)Bits(f0, 12, 6), (int)Bits(f0, 18, 6),
                 (int)Bits(f1, 0, 6), (int)Bits(f1, 6, 6), (int)Bits(f1, 12, 6)};
  int sum = 0;
  for (int c : coef) sum += c;
  bool filter = sum > 0;

  for (int oy = 0; oy < out_h; oy++) {
    int src_y = sy + std::min(h - 1, (int)(oy / yscale));
    u8* line = Mem::PhysPtr(dest + oy * stride, (u32)w * 2);
    if (!line) break;
    Mem::MarkWritten(dest + oy * stride, (u32)w * 2);
    for (int x = 0; x < w; x += 2) {
      int rgb[2][3];
      for (int k = 0; k < 2; k++) {
        u8 prev[4], cur[4], next[4];
        ReadEFB(sx + x + k, src_y - 1, prev);
        ReadEFB(sx + x + k, src_y, cur);
        ReadEFB(sx + x + k, src_y + 1, next);
        for (int i = 0; i < 3; i++) {
          int v = cur[i];
          if (filter)
            v = std::min(255, (prev[i] * (coef[0] + coef[1]) + cur[i] * (coef[2] + coef[3] + coef[4]) +
                               next[i] * (coef[5] + coef[6])) >> 6);
          rgb[k][i] = gamma[v];
        }
      }
      auto Y = [](const int* c) { return std::clamp(((66 * c[0] + 129 * c[1] + 25 * c[2] + 128) >> 8) + 16, 16, 235); };
      int r = (rgb[0][0] + rgb[1][0]) / 2, g = (rgb[0][1] + rgb[1][1]) / 2, b = (rgb[0][2] + rgb[1][2]) / 2;
      int u = std::clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 16, 240);
      int v = std::clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 16, 240);
      line[x * 2] = (u8)Y(rgb[0]);
      line[x * 2 + 1] = (u8)u;
      line[x * 2 + 2] = (u8)Y(rgb[1]);
      line[x * 2 + 3] = (u8)v;
    }
  }
}

void Clear(int sx, int sy, int w, int h) {
  u32 blend = g_bp[BP_BLENDMODE];
  bool color = Bits(blend, 3, 1), alpha = Bits(blend, 4, 1);
  bool z = Bits(g_bp[BP_ZMODE], 4, 1);
  u32 ar = g_bp[BP_CLEAR_AR], gb = g_bp[BP_CLEAR_GB];
  u8 c[4] = {(u8)ar, (u8)(gb >> 8), (u8)gb, (u8)(ar >> 8)};
  Quantize(c);
  u32 cz = g_bp[BP_CLEAR_Z] & 0xFFFFFF;
  if (g_gpu) {
    g_gpu->ClearEFB(std::max(0, sx), std::max(0, sy), std::min(EFB_WIDTH, sx + w), std::min(EFB_HEIGHT, sy + h), color,
                    alpha, z, Pack(c), cz);
    s_mirror_stale = true;
    return;
  }
  for (int y = std::max(0, sy); y < std::min(EFB_HEIGHT, sy + h); y++) {
    for (int x = std::max(0, sx); x < std::min(EFB_WIDTH, sx + w); x++) {
      u32& px = s_color[y * EFB_WIDTH + x];
      u8 d[4];
      Unpack(px, d);
      if (color) d[0] = c[0], d[1] = c[1], d[2] = c[2];
      if (alpha) d[3] = c[3];
      px = Pack(d);
      if (z) s_depth[y * EFB_WIDTH + x] = cz;
    }
  }
}

}  // namespace

void EFBDoState(StateBuffer& s) {
  SyncMirror();
  s.DoBytes(s_color, EFB_WIDTH * EFB_HEIGHT * 4);
  s.DoBytes(s_depth, EFB_WIDTH * EFB_HEIGHT * 4);
}

void EFBReset() {
  if (!s_color) s_color = (u32*)calloc(EFB_WIDTH * EFB_HEIGHT, 4);
  if (!s_depth) s_depth = (u32*)calloc(EFB_WIDTH * EFB_HEIGHT, 4);
  for (int i = 0; i < EFB_WIDTH * EFB_HEIGHT; i++) {
    s_color[i] = 0x000000FF;
    s_depth[i] = 0xFFFFFF;
  }
}

bool EFBDepthTest(int x, int y, u32 z, u32 func) { return Compare(func, z, s_depth[y * EFB_WIDTH + x]); }

void EFBWriteDepth(int x, int y, u32 z) { s_depth[y * EFB_WIDTH + x] = z & 0xFFFFFF; }

namespace {
// Blend state decoded once per draw call
enum BlendMode { BLEND_NONE, BLEND_FACTORS, BLEND_LOGIC, BLEND_SUBTRACT };
struct BlendState {
  BlendMode mode;
  u32 sf, df, logic_op;
  bool const_alpha;
  u8 const_alpha_value;
  bool color_update, alpha_update;
  u32 format;
  bool dst_has_alpha;
};
BlendState s_blend;

inline void Factors(u32 f, bool is_src, const u8* src, const u8* dst, u32* out) {
  switch (f) {
    case 0: out[0] = out[1] = out[2] = out[3] = 0; break;
    case 1: out[0] = out[1] = out[2] = out[3] = 255; break;
    case 2: {
      const u8* c = is_src ? dst : src;
      out[0] = c[0], out[1] = c[1], out[2] = c[2], out[3] = c[3];
      break;
    }
    case 3: {
      const u8* c = is_src ? dst : src;
      out[0] = 255 - c[0], out[1] = 255 - c[1], out[2] = 255 - c[2], out[3] = 255 - c[3];
      break;
    }
    case 4: out[0] = out[1] = out[2] = out[3] = src[3]; break;
    case 5: out[0] = out[1] = out[2] = out[3] = 255 - src[3]; break;
    case 6: out[0] = out[1] = out[2] = out[3] = dst[3]; break;
    default: out[0] = out[1] = out[2] = out[3] = 255 - dst[3]; break;
  }
}

inline void QuantizeFmt(u32 fmt, u8* c) {
  switch (fmt) {
    case 1:  // RGBA6
      for (int i = 0; i < 4; i++) c[i] = (u8)((c[i] & 0xFC) | (c[i] >> 6));
      break;
    case 2:  // RGB565
      c[0] = (u8)((c[0] & 0xF8) | (c[0] >> 5));
      c[1] = (u8)((c[1] & 0xFC) | (c[1] >> 6));
      c[2] = (u8)((c[2] & 0xF8) | (c[2] >> 5));
      c[3] = 255;
      break;
    default:  // RGB8: no alpha channel
      c[3] = 255;
      break;
  }
}
}  // namespace

void PrepareBlend() {
  u32 blend = g_bp[BP_BLENDMODE];
  BlendState& b = s_blend;
  if (Bits(blend, 11, 1)) b.mode = BLEND_SUBTRACT;
  else if (Bits(blend, 0, 1)) b.mode = BLEND_FACTORS;
  else if (Bits(blend, 1, 1)) b.mode = BLEND_LOGIC;
  else b.mode = BLEND_NONE;
  b.sf = Bits(blend, 8, 3);
  b.df = Bits(blend, 5, 3);
  b.logic_op = Bits(blend, 12, 4);
  u32 ca = g_bp[BP_CONSTANTALPHA];
  b.const_alpha = Bits(ca, 8, 1);
  b.const_alpha_value = (u8)Bits(ca, 0, 8);
  b.color_update = Bits(blend, 3, 1);
  b.alpha_update = Bits(blend, 4, 1);
  b.format = PixelFormat();
  b.dst_has_alpha = b.format == 1;
}

void EFBBlend(int x, int y, const u8 rgba[4]) {
  const BlendState& b = s_blend;
  u32& px = s_color[y * EFB_WIDTH + x];
  u8 dst[4], out[4];
  Unpack(px, dst);
  if (!b.dst_has_alpha) dst[3] = 255;  // formats without alpha read as opaque

  switch (b.mode) {
    case BLEND_SUBTRACT:
      for (int i = 0; i < 4; i++) out[i] = (u8)std::max(0, (int)dst[i] - (int)rgba[i]);
      break;
    case BLEND_FACTORS: {
      u32 sfac[4], dfac[4];
      Factors(b.sf, true, rgba, dst, sfac);
      Factors(b.df, false, rgba, dst, dfac);
      for (int i = 0; i < 4; i++) {
        u32 s = sfac[i] + (sfac[i] >> 7), d = dfac[i] + (dfac[i] >> 7);
        u32 v = (rgba[i] * s + dst[i] * d) >> 8;
        out[i] = (u8)(v > 255 ? 255 : v);
      }
      break;
    }
    case BLEND_LOGIC:
      for (int i = 0; i < 4; i++) out[i] = LogicOp(b.logic_op, rgba[i], dst[i]);
      break;
    default:
      memcpy(out, rgba, 4);
      break;
  }

  if (b.const_alpha) out[3] = b.const_alpha_value;
  QuantizeFmt(b.format, out);
  Unpack(px, dst);
  if (b.color_update) dst[0] = out[0], dst[1] = out[1], dst[2] = out[2];
  if (b.alpha_update) dst[3] = out[3];
  px = Pack(dst);
}

void MarkEFBGpuDirty() { s_mirror_stale = true; }

void EFBCopy(u32 cmd) {
  SyncMirror();
  int sx = (int)Bits(g_bp[BP_EFB_TL], 0, 10), sy = (int)Bits(g_bp[BP_EFB_TL], 10, 10);
  int w = (int)Bits(g_bp[BP_EFB_WH], 0, 10) + 1, h = (int)Bits(g_bp[BP_EFB_WH], 10, 10) + 1;
  if (Bits(cmd, 14, 1)) {
    g_stats.xfb_copies++;
    CopyToXFB(cmd, sx, sy, w, h);
  } else {
    g_stats.efb_copies++;
    CopyToTexture(cmd, sx, sy, w, h);
  }
  if (Bits(cmd, 11, 1)) Clear(sx, sy, w, h);
}

u32 PeekEFBColor(u32 x, u32 y) {
  if (x >= (u32)EFB_WIDTH || y >= (u32)EFB_HEIGHT) return 0;
  SyncMirror();
  u8 c[4];
  Unpack(s_color[y * EFB_WIDTH + x], c);
  return ((u32)c[3] << 24) | ((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2];
}
u32 PeekEFBDepth(u32 x, u32 y) {
  if (x >= (u32)EFB_WIDTH || y >= (u32)EFB_HEIGHT) return 0;
  SyncMirror();
  return s_depth[y * EFB_WIDTH + x];
}
void PokeEFBColor(u32 x, u32 y, u32 argb) {
  if (x >= (u32)EFB_WIDTH || y >= (u32)EFB_HEIGHT) return;
  u8 c[4] = {(u8)(argb >> 16), (u8)(argb >> 8), (u8)argb, (u8)(argb >> 24)};
  s_color[y * EFB_WIDTH + x] = Pack(c);
}
void PokeEFBDepth(u32 x, u32 y, u32 z) {
  if (x >= (u32)EFB_WIDTH || y >= (u32)EFB_HEIGHT) return;
  s_depth[y * EFB_WIDTH + x] = z & 0xFFFFFF;
}

}  // namespace Video
