// SPDX-License-Identifier: GPL-2.0-or-later
// Texture sampling: TMEM, TLUTs, mipmaps, wrapping, filtering and texel decoding
// for every GameCube texture format.
#include <algorithm>
#include <cstdlib>

#include "core/memory.h"
#include "core/video/video_internal.h"

namespace Video {

namespace {

constexpr u32 TMEM_SIZE = 1024 * 1024;
constexpr u32 TMEM_LINE = 32;
u8* s_tmem = nullptr;

enum Format : u32 {
  I4 = 0, I8 = 1, IA4 = 2, IA8 = 3, RGB565 = 4, RGB5A3 = 5, RGBA8 = 6, C4 = 8, C8 = 9, C14X2 = 10, CMPR = 14,
};

inline u8 C3to8(u32 v) { return (u8)((v << 5) | (v << 2) | (v >> 1)); }
inline u8 C4to8(u32 v) { return (u8)(v * 17); }
inline u8 C5to8(u32 v) { return (u8)((v << 3) | (v >> 2)); }
inline u8 C6to8(u32 v) { return (u8)((v << 2) | (v >> 4)); }

inline void DecodeRGB565(u16 v, u8* o) {
  o[0] = C5to8((v >> 11) & 31);
  o[1] = C6to8((v >> 5) & 63);
  o[2] = C5to8(v & 31);
  o[3] = 255;
}
inline void DecodeRGB5A3(u16 v, u8* o) {
  if (v & 0x8000) {
    o[0] = C5to8((v >> 10) & 31);
    o[1] = C5to8((v >> 5) & 31);
    o[2] = C5to8(v & 31);
    o[3] = 255;
  } else {
    o[0] = C4to8((v >> 8) & 15);
    o[1] = C4to8((v >> 4) & 15);
    o[2] = C4to8(v & 15);
    o[3] = C3to8((v >> 12) & 7);
  }
}
inline void DecodeIA8(u16 v, u8* o) {  // high byte alpha, low byte intensity
  o[0] = o[1] = o[2] = (u8)v;
  o[3] = (u8)(v >> 8);
}
inline void DecodePalette(const u8* tlut, u32 index, u32 tlut_fmt, u8* o) {
  u16 v = LoadBE16(tlut + (index & 0x3FFF) * 2);
  switch (tlut_fmt) {
    case 0: DecodeIA8(v, o); break;
    case 1: DecodeRGB565(v, o); break;
    default: DecodeRGB5A3(v, o); break;
  }
}

// Safe byte read from a texture source (returns 0 past the end)
struct Source {
  const u8* p;
  u32 size;
  u8 At(u32 off) const { return off < size ? p[off] : 0; }
  u16 At16(u32 off) const { return off + 1 < size ? LoadBE16(p + off) : 0; }
};

inline u8 DXTBlend(u32 a, u32 b) { return (u8)((a * 3 + b * 5) >> 3); }

void DecodeTexel(const Source& src, int s, int t, int width_minus_1, u32 fmt, const u8* tlut, u32 tlut_fmt,
                 u8* o) {
  switch (fmt) {
    case I4:
    case C4: {
      u32 base = (((u32)(t >> 3) * ((width_minus_1 >> 3) + 1)) + (s >> 3)) << 5;
      u32 off = ((t & 7) << 3) + (s & 7);
      u8 val = (src.At(base + (off >> 1)) >> ((off & 1) ? 0 : 4)) & 0xF;
      if (fmt == C4) {
        DecodePalette(tlut, val, tlut_fmt, o);
      } else {
        o[0] = o[1] = o[2] = o[3] = C4to8(val);
      }
      break;
    }
    case I8:
    case C8:
    case IA4: {
      u32 base = (((u32)(t >> 2) * ((width_minus_1 >> 3) + 1)) + (s >> 3)) << 5;
      u8 val = src.At(base + ((t & 3) << 3) + (s & 7));
      if (fmt == I8) {
        o[0] = o[1] = o[2] = o[3] = val;
      } else if (fmt == C8) {
        DecodePalette(tlut, val, tlut_fmt, o);
      } else {
        o[0] = o[1] = o[2] = C4to8(val & 0xF);
        o[3] = C4to8(val >> 4);
      }
      break;
    }
    case IA8:
    case RGB565:
    case RGB5A3:
    case C14X2: {
      u32 base = (((u32)(t >> 2) * ((width_minus_1 >> 2) + 1)) + (s >> 2)) << 4;
      u32 off = (base + ((t & 3) << 2) + (s & 3)) << 1;
      u16 v = src.At16(off);
      if (fmt == IA8) DecodeIA8(v, o);
      else if (fmt == RGB565) DecodeRGB565(v, o);
      else if (fmt == RGB5A3) DecodeRGB5A3(v, o);
      else DecodePalette(tlut, v & 0x3FFF, tlut_fmt, o);
      break;
    }
    case RGBA8: {
      u32 base = (((u32)(t >> 2) * ((width_minus_1 >> 2) + 1)) + (s >> 2)) << 5;
      u32 off = (base + ((t & 3) << 2) + (s & 3)) << 1;
      o[3] = src.At(off);
      o[0] = src.At(off + 1);
      o[1] = src.At(off + 32);
      o[2] = src.At(off + 33);
      break;
    }
    case CMPR: {
      u32 sd = s >> 2, td = t >> 2;
      u32 base = ((td >> 1) * ((width_minus_1 >> 3) + 1) + (sd >> 1)) << 2;
      u32 off = (base + ((td & 1) << 1) + (sd & 1)) << 3;
      u16 c1 = src.At16(off), c2 = src.At16(off + 2);
      u8 r1 = C5to8((c1 >> 11) & 31), g1 = C6to8((c1 >> 5) & 63), b1 = C5to8(c1 & 31);
      u8 r2 = C5to8((c2 >> 11) & 31), g2 = C6to8((c2 >> 5) & 63), b2 = C5to8(c2 & 31);
      u32 sel = (src.At(off + 4 + (t & 3)) >> (6 - ((s & 3) << 1))) & 3;
      if (c1 <= c2) sel |= 4;
      switch (sel) {
        case 0: case 4: o[0] = r1; o[1] = g1; o[2] = b1; o[3] = 255; break;
        case 1: case 5: o[0] = r2; o[1] = g2; o[2] = b2; o[3] = 255; break;
        case 2: o[0] = DXTBlend(r2, r1); o[1] = DXTBlend(g2, g1); o[2] = DXTBlend(b2, b1); o[3] = 255; break;
        case 3: o[0] = DXTBlend(r1, r2); o[1] = DXTBlend(g1, g2); o[2] = DXTBlend(b1, b2); o[3] = 255; break;
        case 6: o[0] = (r1 + r2) / 2; o[1] = (g1 + g2) / 2; o[2] = (b1 + b2) / 2; o[3] = 255; break;
        default: o[0] = (r1 + r2) / 2; o[1] = (g1 + g2) / 2; o[2] = (b1 + b2) / 2; o[3] = 0; break;
      }
      break;
    }
    default: o[0] = o[1] = o[2] = o[3] = 0; break;
  }
}

// RGBA8 textures preloaded in TMEM keep the AR and GB halves in separate banks.
void DecodeRGBA8FromTMEM(const u8* even, const u8* odd, int s, int t, int width_minus_1, u8* o) {
  u32 base = (((u32)(t >> 2) * ((width_minus_1 >> 2) + 1)) + (s >> 2)) << 4;
  u32 off = (base + ((t & 3) << 2) + (s & 3)) << 1;
  o[3] = even[off & (TMEM_SIZE - 1)];
  o[0] = even[(off + 1) & (TMEM_SIZE - 1)];
  o[1] = odd[off & (TMEM_SIZE - 1)];
  o[2] = odd[(off + 1) & (TMEM_SIZE - 1)];
}

u32 BlockWidth(u32 fmt) {
  switch (fmt) {
    case I4: case C4: case CMPR: case I8: case C8: case IA4: return 8;
    default: return 4;
  }
}
u32 BlockHeight(u32 fmt) {
  switch (fmt) {
    case I4: case C4: case CMPR: return 8;
    default: return 4;
  }
}
u32 BitsPerTexel(u32 fmt) {
  switch (fmt) {
    case I4: case C4: case CMPR: return 4;
    case I8: case C8: case IA4: return 8;
    case RGBA8: return 32;
    default: return 16;
  }
}

inline int Wrap(int coord, u32 mode, int size) {
  switch (mode) {
    case 0: return std::clamp(coord, 0, size - 1);  // clamp
    case 1: return coord & (size - 1);              // repeat (power of two sizes)
    case 2:                                         // mirror
      if (coord & size) coord = ~coord;
      return coord & (size - 1);
    default: return std::clamp(coord, 0, size - 1);
  }
}

void SampleMip(u32 texmap, s32 s, s32 t, s32 mip, bool linear, u8 out[4]) {
  u32 mode0 = g_bp[TexReg(0x80, texmap)];
  u32 img0 = g_bp[TexReg(0x88, texmap)];
  u32 img1 = g_bp[TexReg(0x8C, texmap)];
  u32 img2 = g_bp[TexReg(0x90, texmap)];
  u32 img3 = g_bp[TexReg(0x94, texmap)];
  u32 tlutr = g_bp[TexReg(0x98, texmap)];
  u32 fmt = Bits(img0, 20, 4);
  int w1 = (int)Bits(img0, 0, 10), h1 = (int)Bits(img0, 10, 10);
  u32 tlut_fmt = Bits(tlutr, 10, 2);
  const u8* tlut = s_tmem + ((Bits(tlutr, 0, 10) << 9) & (TMEM_SIZE - 1));
  bool from_tmem = Bits(img1, 21, 1);

  Source src;
  const u8* odd = nullptr;
  if (from_tmem) {
    u32 even_off = Bits(img1, 0, 15) * TMEM_LINE;
    src = {s_tmem + (even_off & (TMEM_SIZE - 1)), TMEM_SIZE - (even_off & (TMEM_SIZE - 1))};
    odd = s_tmem + ((Bits(img2, 0, 15) * TMEM_LINE) & (TMEM_SIZE - 1));
  } else {
    u32 addr = (Bits(img3, 0, 24) << 5) & 0x01FFFFFF;
    u8* p = Mem::PhysPtr(addr, 1);
    src = {p, p ? Mem::MEM1_SIZE - addr : 0};
    if (!p) {
      out[0] = out[1] = out[2] = out[3] = 0;
      return;
    }
  }

  if (mip) {
    int mw = w1 + 1, mh = h1 + 1;
    int bw = (int)BlockWidth(fmt), bh = (int)BlockHeight(fmt);
    u32 bpt = BitsPerTexel(fmt);
    w1 >>= mip;
    h1 >>= mip;
    s >>= mip;
    t >>= mip;
    for (int m = 0; m < mip; m++) {
      mw = std::max(mw, bw);
      mh = std::max(mh, bh);
      u32 bytes = (u32)(((mw + bw - 1) / bw * bw) * ((mh + bh - 1) / bh * bh)) * bpt / 8;
      if (bytes >= src.size) {
        src.size = 0;
        break;
      }
      src.p += bytes;
      src.size -= bytes;
      mw >>= 1;
      mh >>= 1;
    }
  }
  u32 wrap_s = Bits(mode0, 0, 2), wrap_t = Bits(mode0, 2, 2);
  bool tmem_rgba8 = from_tmem && fmt == RGBA8;

  auto fetch = [&](int x, int y, u8* o) {
    x = Wrap(x, wrap_s, w1 + 1);
    y = Wrap(y, wrap_t, h1 + 1);
    if (tmem_rgba8)
      DecodeRGBA8FromTMEM(src.p, odd, x, y, w1, o);
    else
      DecodeTexel(src, x, y, w1, fmt, tlut, tlut_fmt, o);
  };

  if (linear) {
    s -= 64;
    t -= 64;
    int x0 = s >> 7, y0 = t >> 7;
    u32 fs = s & 0x7F, ft = t & 0x7F;
    u8 a[4], b[4], c[4], d[4];
    fetch(x0, y0, a);
    fetch(x0 + 1, y0, b);
    fetch(x0, y0 + 1, c);
    fetch(x0 + 1, y0 + 1, d);
    for (int i = 0; i < 4; i++) {
      u32 v = a[i] * (128 - fs) * (128 - ft) + b[i] * fs * (128 - ft) + c[i] * (128 - fs) * ft + d[i] * fs * ft;
      out[i] = (u8)(v >> 14);
    }
  } else {
    fetch(s >> 7, t >> 7, out);
  }
}

}  // namespace

void TextureReset() {
  if (!s_tmem) s_tmem = (u8*)calloc(1, TMEM_SIZE);
  memset(s_tmem, 0, TMEM_SIZE);
}

void SampleTexture(u32 texmap, s32 s, s32 t, s32 lod, bool linear, u8 out[4]) {
  u32 mode0 = g_bp[TexReg(0x80, texmap)];
  u32 mip_filter = Bits(mode0, 5, 2);  // 0 none, 1 point, 2 linear
  int base = 0;
  bool mip_linear = false;
  s32 frac = lod & 0xF;
  if (lod > 0 && mip_filter != 0) {
    base = lod >> 4;
    mip_linear = frac && mip_filter == 2;
    if (mip_filter == 1 && frac >= 8) base++;
  }
  if (mip_linear) {
    u8 a[4], b[4];
    SampleMip(texmap, s, t, base, linear, a);
    SampleMip(texmap, s, t, base + 1, linear, b);
    for (int i = 0; i < 4; i++) out[i] = (u8)((a[i] * (16 - frac) + b[i] * frac) >> 4);
  } else {
    SampleMip(texmap, s, t, base, linear, out);
  }
}

void LoadTLUT(u32 value) {
  u32 tmem_addr = Bits(value, 0, 10) << 9;
  u32 bytes = Bits(value, 10, 11) * TMEM_LINE;
  u32 src = (g_bp[0x64] << 5) & 0x01FFFFFF;
  const u8* p = Mem::PhysPtr(src, bytes);
  if (!p || tmem_addr + bytes > TMEM_SIZE) return;
  memcpy(s_tmem + tmem_addr, p, bytes);
}

void PreloadTMEM(u32 value) {
  if (!value) return;
  u32 src = (g_bp[0x60] << 5) & 0x01FFFFFF;
  u32 even = Bits(g_bp[0x61], 0, 15) * TMEM_LINE, odd = Bits(g_bp[0x62], 0, 15) * TMEM_LINE;
  u32 count = Bits(value, 0, 15), type = Bits(value, 15, 2);
  if (type != 3) {
    u32 bytes = count * TMEM_LINE;
    if (even >= TMEM_SIZE) return;
    bytes = std::min(bytes, TMEM_SIZE - even);
    if (const u8* p = Mem::PhysPtr(src, bytes)) memcpy(s_tmem + even, p, bytes);
  } else {
    for (u32 i = 0; i < count; i++) {
      if (even + TMEM_LINE > TMEM_SIZE || odd + TMEM_LINE > TMEM_SIZE) break;
      const u8* p = Mem::PhysPtr(src + i * 64, 64);
      if (!p) break;
      memcpy(s_tmem + even, p, TMEM_LINE);
      memcpy(s_tmem + odd, p + TMEM_LINE, TMEM_LINE);
      even += TMEM_LINE;
      odd += TMEM_LINE;
    }
  }
}

}  // namespace Video
