// SPDX-License-Identifier: GPL-2.0-or-later
// Texture sampling: TMEM, TLUTs, mipmaps, wrapping, filtering and texel decoding
// for every GameCube texture format.
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

#include "core/memory.h"
#include "core/state.h"
#include "core/video/gpu_backend.h"
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

// ---- Decoded texture cache ----
// Textures are decoded once to RGBA8 (all needed mip levels) and reused until
// their source changes: guest RAM pages written since decoding (Mem write
// stamps), TMEM reloads, or a different palette.

struct LevelSource {
  Source src;
  const u8* odd;
  int w1, h1;  // level size minus one
};

struct TexParams {
  u32 fmt, tlut_fmt;
  int w1, h1;
  bool from_tmem;
  const u8* tlut;
  u32 ram_addr;
};

TexParams GetParams(u32 texmap) {
  u32 img0 = g_bp[TexReg(0x88, texmap)];
  u32 img1 = g_bp[TexReg(0x8C, texmap)];
  u32 img3 = g_bp[TexReg(0x94, texmap)];
  u32 tlutr = g_bp[TexReg(0x98, texmap)];
  TexParams p;
  p.fmt = Bits(img0, 20, 4);
  p.w1 = (int)Bits(img0, 0, 10);
  p.h1 = (int)Bits(img0, 10, 10);
  p.tlut_fmt = Bits(tlutr, 10, 2);
  p.tlut = s_tmem + ((Bits(tlutr, 0, 10) << 9) & (TMEM_SIZE - 1));
  p.from_tmem = Bits(img1, 21, 1);
  p.ram_addr = (Bits(img3, 0, 24) << 5) & 0x01FFFFFF;
  return p;
}

// Source data of mip level `mip` (same layout rules as the hardware).
bool GetLevelSource(u32 texmap, const TexParams& p, int mip, LevelSource& out) {
  u32 img1 = g_bp[TexReg(0x8C, texmap)];
  u32 img2 = g_bp[TexReg(0x90, texmap)];
  out.odd = nullptr;
  if (p.from_tmem) {
    u32 even_off = Bits(img1, 0, 15) * TMEM_LINE;
    out.src = {s_tmem + (even_off & (TMEM_SIZE - 1)), TMEM_SIZE - (even_off & (TMEM_SIZE - 1))};
    out.odd = s_tmem + ((Bits(img2, 0, 15) * TMEM_LINE) & (TMEM_SIZE - 1));
  } else {
    u8* ptr = Mem::PhysPtr(p.ram_addr, 1);
    if (!ptr) return false;
    out.src = {ptr, Mem::MEM1_SIZE - p.ram_addr};
  }
  out.w1 = p.w1;
  out.h1 = p.h1;
  if (mip) {
    int mw = p.w1 + 1, mh = p.h1 + 1;
    int bw = (int)BlockWidth(p.fmt), bh = (int)BlockHeight(p.fmt);
    u32 bpt = BitsPerTexel(p.fmt);
    out.w1 >>= mip;
    out.h1 >>= mip;
    for (int m = 0; m < mip; m++) {
      mw = std::max(mw, bw);
      mh = std::max(mh, bh);
      u32 bytes = (u32)(((mw + bw - 1) / bw * bw) * ((mh + bh - 1) / bh * bh)) * bpt / 8;
      if (bytes >= out.src.size) {
        out.src.size = 0;
        break;
      }
      out.src.p += bytes;
      out.src.size -= bytes;
      mw >>= 1;
      mh >>= 1;
    }
  }
  return true;
}

constexpr int MAX_LEVELS = 11;

struct TexEntry {
  u32 img0, img1, img2, img3, tlutr;
  u32 tlut_hash;
  u32 stamp;      // Mem write stamp at decode time
  u32 tmem_gen;   // TMEM generation at decode time
  u32 src_bytes;  // RAM bytes covered by the decoded levels
  int levels;     // decoded levels
  bool valid;
  u32 last_use;
  u32 version;    // unique per decode (GPU upload key)
  std::vector<u8> data;  // RGBA bytes, levels back to back
  u32 level_offset[MAX_LEVELS];
  int level_w[MAX_LEVELS], level_h[MAX_LEVELS];
};

struct BoundTexture {
  const TexEntry* e;  // nullptr: unmapped texture (samples read 0)
  u32 wrap_s, wrap_t;
  u32 mip_filter;
};

std::vector<std::unique_ptr<TexEntry>>* s_cache = nullptr;
size_t s_cache_bytes = 0;
u32 s_tmem_gen = 1;
u32 s_use_counter = 0;
u32 s_version_counter = 0;
BoundTexture s_bound[8];
constexpr size_t CACHE_BUDGET = 24u << 20;

u32 HashBytes(const u8* p, u32 n) {
  u32 h = 2166136261u;
  for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

u32 PaletteEntries(u32 fmt) {
  switch (fmt) {
    case C4: return 16;
    case C8: return 256;
    case C14X2: return 16384;
    default: return 0;
  }
}

void DecodeLevel(u32 texmap, const TexParams& p, int mip, u8* out, int w, int h) {
  LevelSource ls;
  if (!GetLevelSource(texmap, p, mip, ls)) {
    memset(out, 0, (size_t)w * h * 4);
    return;
  }
  bool tmem_rgba8 = p.from_tmem && p.fmt == RGBA8;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      u8* o = out + ((size_t)y * w + x) * 4;
      if (tmem_rgba8)
        DecodeRGBA8FromTMEM(ls.src.p, ls.odd, x, y, ls.w1, o);
      else
        DecodeTexel(ls.src, x, y, ls.w1, p.fmt, p.tlut, p.tlut_fmt, o);
    }
}

// RAM bytes used by levels [0, levels)
u32 SourceBytes(const TexParams& p, int levels) {
  int mw = p.w1 + 1, mh = p.h1 + 1;
  int bw = (int)BlockWidth(p.fmt), bh = (int)BlockHeight(p.fmt);
  u32 bpt = BitsPerTexel(p.fmt), total = 0;
  for (int m = 0; m < levels; m++) {
    mw = std::max(mw, bw);
    mh = std::max(mh, bh);
    total += (u32)(((mw + bw - 1) / bw * bw) * ((mh + bh - 1) / bh * bh)) * bpt / 8;
    mw >>= 1;
    mh >>= 1;
  }
  return total;
}

void BindTexture(u32 texmap) {
  BoundTexture& b = s_bound[texmap];
  u32 mode0 = g_bp[TexReg(0x80, texmap)], mode1 = g_bp[TexReg(0x84, texmap)];
  b.wrap_s = Bits(mode0, 0, 2);
  b.wrap_t = Bits(mode0, 2, 2);
  b.mip_filter = Bits(mode0, 5, 2);
  b.e = nullptr;
  TexParams p = GetParams(texmap);
  if (!p.from_tmem && !Mem::PhysPtr(p.ram_addr, 1)) return;

  int levels = 1;
  if (b.mip_filter != 0) levels = std::min(MAX_LEVELS, (int)(Bits(mode1, 8, 8) >> 4) + 2);
  u32 img0 = g_bp[TexReg(0x88, texmap)], img1 = g_bp[TexReg(0x8C, texmap)];
  u32 img2 = g_bp[TexReg(0x90, texmap)], img3 = g_bp[TexReg(0x94, texmap)];
  u32 tlutr = g_bp[TexReg(0x98, texmap)];
  u32 pal = PaletteEntries(p.fmt);
  u32 tlut_hash = 0;
  if (pal) {
    u32 off = (u32)(p.tlut - s_tmem);
    tlut_hash = HashBytes(p.tlut, std::min<u32>(pal * 2, TMEM_SIZE - off));
  } else {
    tlutr = 0;  // the palette does not matter for direct formats
  }
  if (!p.from_tmem) img1 &= 1u << 21, img2 = 0;  // TMEM placement irrelevant for RAM textures
  u32 src_bytes = p.from_tmem ? 0 : SourceBytes(p, levels);

  TexEntry* hit = nullptr;
  for (auto& ep : *s_cache) {
    TexEntry& e = *ep;
    if (e.img3 == img3 && e.img0 == img0 && e.img1 == img1 && e.img2 == img2 && e.tlutr == tlutr &&
        e.tlut_hash == tlut_hash) {
      hit = &e;
      break;
    }
  }
  if (hit) {
    bool fresh = hit->levels >= levels &&
                 (p.from_tmem ? hit->tmem_gen == s_tmem_gen
                              : Mem::UnchangedSince(p.ram_addr, hit->src_bytes, hit->stamp));
    if (fresh) {
      hit->last_use = ++s_use_counter;
      b.e = hit;
      return;
    }
  } else {
    s_cache->push_back(std::make_unique<TexEntry>());
    hit = s_cache->back().get();
  }
  TexEntry& e = *hit;
  e.img0 = img0, e.img1 = img1, e.img2 = img2, e.img3 = img3, e.tlutr = tlutr, e.tlut_hash = tlut_hash;
  e.stamp = Mem::NewStamp();
  e.tmem_gen = s_tmem_gen;
  e.src_bytes = src_bytes;
  e.levels = levels;
  e.valid = true;
  e.last_use = ++s_use_counter;
  e.version = ++s_version_counter;
  size_t total = 0;
  for (int l = 0; l < levels; l++) {
    e.level_w[l] = (p.w1 >> l) + 1;
    e.level_h[l] = (p.h1 >> l) + 1;
    e.level_offset[l] = (u32)total;
    total += (size_t)e.level_w[l] * e.level_h[l] * 4;
  }
  s_cache_bytes -= e.data.size();
  e.data.resize(total);
  s_cache_bytes += total;
  for (int l = 0; l < levels; l++) DecodeLevel(texmap, p, l, e.data.data() + e.level_offset[l], e.level_w[l], e.level_h[l]);
  b.e = hit;
}

void SampleMip(u32 texmap, s32 s, s32 t, s32 mip, bool linear, u8 out[4]) {
  const BoundTexture& b = s_bound[texmap];
  const TexEntry* e = b.e;
  if (!e || mip >= e->levels) {
    out[0] = out[1] = out[2] = out[3] = 0;
    return;
  }
  int w = e->level_w[mip], h = e->level_h[mip];
  const u8* data = e->data.data() + e->level_offset[mip];
  if (mip) {
    s >>= mip;
    t >>= mip;
  }
  if (linear) {
    s -= 64;
    t -= 64;
    int x0 = s >> 7, y0 = t >> 7;
    u32 fs = s & 0x7F, ft = t & 0x7F;
    int xa = Wrap(x0, b.wrap_s, w), xb = Wrap(x0 + 1, b.wrap_s, w);
    int ya = Wrap(y0, b.wrap_t, h), yb = Wrap(y0 + 1, b.wrap_t, h);
    const u8* a = data + ((size_t)ya * w + xa) * 4;
    const u8* bb = data + ((size_t)ya * w + xb) * 4;
    const u8* c = data + ((size_t)yb * w + xa) * 4;
    const u8* d = data + ((size_t)yb * w + xb) * 4;
    u32 w00 = (128 - fs) * (128 - ft), w10 = fs * (128 - ft), w01 = (128 - fs) * ft, w11 = fs * ft;
    for (int i = 0; i < 4; i++) out[i] = (u8)((a[i] * w00 + bb[i] * w10 + c[i] * w01 + d[i] * w11) >> 14);
  } else {
    const u8* a = data + ((size_t)Wrap(t >> 7, b.wrap_t, h) * w + Wrap(s >> 7, b.wrap_s, w)) * 4;
    memcpy(out, a, 4);
  }
}

}  // namespace

void TextureDoState(StateBuffer& s) {
  s.DoBytes(s_tmem, TMEM_SIZE);
  if (s.IsReading()) InvalidateTextureCache();
}

void InvalidateTextureCache() {
  if (s_cache) s_cache->clear();
  s_cache_bytes = 0;
  s_tmem_gen++;
  for (BoundTexture& b : s_bound) b.e = nullptr;
}

void GetGpuTexture(u32 texmap, GpuTexture& out) {
  const BoundTexture& b = s_bound[texmap];
  const TexEntry* e = b.e;
  memset(&out, 0, sizeof(out));
  if (!e) return;
  out.rgba = e->data.data() + e->level_offset[0];
  out.width = e->level_w[0];
  out.height = e->level_h[0];
  out.id = e->version;
  out.wrap_s = b.wrap_s;
  out.wrap_t = b.wrap_t;
  u32 mode0 = g_bp[TexReg(0x80, texmap)];
  out.linear = Bits(mode0, 4, 1);  // magnification filter
}

void BindTextures(u32 mask) {
  if (!s_cache) s_cache = new std::vector<std::unique_ptr<TexEntry>>();
  if (s_cache_bytes > CACHE_BUDGET) {  // simple policy: start over (nothing is bound yet)
    s_cache->clear();
    s_cache_bytes = 0;
  }
  for (u32 t = 0; t < 8; t++) s_bound[t].e = nullptr;
  for (u32 t = 0; t < 8; t++)
    if (mask & (1u << t)) BindTexture(t);
}

void TextureReset() {
  if (!s_tmem) s_tmem = (u8*)calloc(1, TMEM_SIZE);
  memset(s_tmem, 0, TMEM_SIZE);
  InvalidateTextureCache();
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
  s_tmem_gen++;
}

void PreloadTMEM(u32 value) {
  if (!value) return;
  s_tmem_gen++;
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
