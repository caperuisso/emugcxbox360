// SPDX-License-Identifier: GPL-2.0-or-later
// GPU register state, XF/CP/BP loads and the vertex loader.
#include <cmath>
#include <cstring>

#include "core/memory.h"
#include "core/state.h"
#include "core/video/video_internal.h"

namespace Video {

u32 g_bp[0x100];
u32 g_cp[0x100];
u32 g_xf[0x1058];
Stats g_stats;

namespace {

const u32 kFmtSize[8] = {1, 1, 2, 2, 4, 4, 4, 4};  // u8 s8 u16 s16 f32 (5-7 behave as f32)
const u32 kColorSize[8] = {2, 3, 4, 2, 3, 4, 4, 4};

Vec3 s_cached_normal[3] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};

u32 IndexSize(u32 mode, u32 direct_size) {
  switch (mode) {
    case 1: return direct_size;
    case 2: return 1;
    case 3: return 2;
    default: return 0;
  }
}

// Decoded vertex attribute format for one VAT entry
struct VertexFormat {
  u32 pnmtx, texmtx_mask;
  u32 pos_mode, pos_comps, pos_fmt;
  float pos_scale;
  u32 nrm_mode, nrm_fmt;
  bool nbt, nrm_index3;
  float nrm_scale;
  u32 col_mode[2], col_fmt[2];
  u32 tc_mode[8], tc_comps[8], tc_fmt[8];
  float tc_scale[8];
};

VertexFormat DecodeFormat(u32 vat, u32 lo, u32 hi, u32 a, u32 b, u32 c) {
  VertexFormat f;
  f.pnmtx = lo & 1;
  f.texmtx_mask = (lo >> 1) & 0xFF;
  f.pos_mode = Bits(lo, 9, 2);
  f.pos_comps = (a & 1) ? 3 : 2;
  f.pos_fmt = Bits(a, 1, 3);
  f.pos_scale = 1.0f / (float)(1u << Bits(a, 4, 5));
  f.nrm_mode = Bits(lo, 11, 2);
  f.nbt = Bits(a, 9, 1);
  f.nrm_fmt = Bits(a, 10, 3);
  f.nrm_index3 = Bits(a, 31, 1);
  f.nrm_scale = (kFmtSize[f.nrm_fmt] == 1) ? 1.0f / 64.0f : (kFmtSize[f.nrm_fmt] == 2 ? 1.0f / 16384.0f : 1.0f);
  f.col_mode[0] = Bits(lo, 13, 2);
  f.col_mode[1] = Bits(lo, 15, 2);
  f.col_fmt[0] = Bits(a, 14, 3);
  f.col_fmt[1] = Bits(a, 18, 3);
  const u32 cnt[8] = {Bits(a, 21, 1), Bits(b, 0, 1), Bits(b, 9, 1), Bits(b, 18, 1),
                      Bits(b, 27, 1), Bits(c, 5, 1), Bits(c, 14, 1), Bits(c, 23, 1)};
  const u32 fmt[8] = {Bits(a, 22, 3), Bits(b, 1, 3), Bits(b, 10, 3), Bits(b, 19, 3),
                      Bits(b, 28, 3), Bits(c, 6, 3), Bits(c, 15, 3), Bits(c, 24, 3)};
  const u32 frac[8] = {Bits(a, 25, 5), Bits(b, 4, 5), Bits(b, 13, 5), Bits(b, 22, 5),
                       Bits(c, 0, 5), Bits(c, 9, 5), Bits(c, 18, 5), Bits(c, 27, 5)};
  for (int t = 0; t < 8; t++) {
    f.tc_mode[t] = Bits(hi, t * 2, 2);
    f.tc_comps[t] = cnt[t] + 1;
    f.tc_fmt[t] = fmt[t];
    f.tc_scale[t] = 1.0f / (float)(1u << frac[t]);
  }
  (void)vat;
  return f;
}

// Decoded per VAT and kept while the CP registers it depends on are unchanged
// (each primitive needs it twice: for its size, then to read it).
const VertexFormat& GetFormat(u32 vat) {
  struct Cached {
    u32 key[5];
    bool valid;
    VertexFormat f;
  };
  static Cached cache[8];
  Cached& e = cache[vat & 7];
  u32 key[5] = {g_cp[0x50], g_cp[0x60], g_cp[0x70 + vat], g_cp[0x80 + vat], g_cp[0x90 + vat]};
  if (!e.valid || memcmp(e.key, key, sizeof(key)) != 0) {
    e.f = DecodeFormat(vat, key[0], key[1], key[2], key[3], key[4]);
    memcpy(e.key, key, sizeof(key));
    e.valid = true;
  }
  return e.f;
}

// Reads one component of an integer/float format and applies the fixed point scale.
inline float ReadComponent(const u8*& p, u32 fmt, float scale) {
  float v;
  switch (fmt) {
    case 0: v = (float)p[0] * scale; p += 1; break;
    case 1: v = (float)(s8)p[0] * scale; p += 1; break;
    case 2: v = (float)LoadBE16(p) * scale; p += 2; break;
    case 3: v = (float)(s16)LoadBE16(p) * scale; p += 2; break;
    default: v = BitCast<float>(LoadBE32(p)); p += 4; break;
  }
  return v;
}

void ReadColor(const u8* p, u32 fmt, u8 out[4]) {
  switch (fmt) {
    case 0: {  // RGB565
      u16 v = LoadBE16(p);
      u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
      out[0] = (u8)((r << 3) | (r >> 2));
      out[1] = (u8)((g << 2) | (g >> 4));
      out[2] = (u8)((b << 3) | (b >> 2));
      out[3] = 255;
      break;
    }
    case 1:  // RGB888
    case 2:  // RGB888x
      out[0] = p[0];
      out[1] = p[1];
      out[2] = p[2];
      out[3] = 255;
      break;
    case 3: {  // RGBA4444
      u16 v = LoadBE16(p);
      out[0] = (u8)(((v >> 12) & 15) * 17);
      out[1] = (u8)(((v >> 8) & 15) * 17);
      out[2] = (u8)(((v >> 4) & 15) * 17);
      out[3] = (u8)((v & 15) * 17);
      break;
    }
    case 4: {  // RGBA6666
      u32 v = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
      for (int i = 0; i < 4; i++) {
        u32 c = (v >> (18 - 6 * i)) & 63;
        out[i] = (u8)((c << 2) | (c >> 4));
      }
      break;
    }
    default:  // RGBA8888
      out[0] = p[0];
      out[1] = p[1];
      out[2] = p[2];
      out[3] = p[3];
      break;
  }
}

// Returns a pointer to attribute data: inline for direct mode, from the array for indexed.
const u8* Attribute(const u8*& p, u32 mode, u32 array, u32 direct_size) {
  if (mode == 1) {
    const u8* d = p;
    p += direct_size;
    return d;
  }
  u32 index = (mode == 2) ? p[0] : LoadBE16(p);
  p += (mode == 2) ? 1 : 2;
  u32 addr = g_cp[0xA0 + array] + index * g_cp[0xB0 + array];
  static const u8 zeros[64] = {};
  const u8* d = Mem::PhysPtr(addr & 0x01FFFFFF, direct_size);
  return d ? d : zeros;
}

}  // namespace

void Reset() {
  memset(g_bp, 0, sizeof(g_bp));
  memset(g_cp, 0, sizeof(g_cp));
  memset(g_xf, 0, sizeof(g_xf));
  g_bp[0xFE] = 0x00FFFFFF;  // BP write mask
  memset(&g_stats, 0, sizeof(g_stats));
  TextureReset();
  EFBReset();
  UpdateScissor();
}

void DoState(StateBuffer& s) {
  s.Marker("Video");
  s.Do(g_bp);
  s.Do(g_cp);
  s.Do(g_xf);
  s.Do(s_cached_normal);
  RasterDoState(s);
  TextureDoState(s);
  EFBDoState(s);
  if (s.IsReading()) UpdateScissor();
}

void LoadCPReg(u8 reg, u32 value) { g_cp[reg] = value; }

void LoadXF(u32 addr, u32 count, const u8* data) {
  for (u32 i = 0; i < count; i++, addr++) {
    if (addr < 0x1058) g_xf[addr] = LoadBE32(data + i * 4);
  }
}

void LoadIndexedXF(u32 array, u32 value) {
  u32 index = value >> 16;
  u32 count = ((value >> 12) & 0xF) + 1;
  u32 xf_addr = value & 0xFFF;
  u32 src = g_cp[0xA0 + 12 + array] + index * g_cp[0xB0 + 12 + array];
  if (const u8* p = Mem::PhysPtr(src & 0x01FFFFFF, count * 4)) LoadXF(xf_addr, count, p);
}

u32 VertexSize(u32 vat) {
  const VertexFormat& f = GetFormat(vat);
  u32 size = f.pnmtx + __builtin_popcount(f.texmtx_mask);
  size += IndexSize(f.pos_mode, f.pos_comps * kFmtSize[f.pos_fmt]);
  if (f.nrm_mode) {
    u32 comps = f.nbt ? 9 : 3;
    if (f.nrm_mode == 1)
      size += comps * kFmtSize[f.nrm_fmt];
    else
      size += (f.nrm_mode == 2 ? 1 : 2) * ((f.nbt && f.nrm_index3) ? 3 : 1);
  }
  for (int c = 0; c < 2; c++) size += IndexSize(f.col_mode[c], kColorSize[f.col_fmt[c]]);
  for (int t = 0; t < 8; t++) size += IndexSize(f.tc_mode[t], f.tc_comps[t] * kFmtSize[f.tc_fmt[t]]);
  return size;
}

void Draw(u8 cmd, u32 count, const u8* data) {
  u32 vat = cmd & 7;
  u32 primitive = (cmd >> 3) & 7;
  const VertexFormat& f = GetFormat(vat);
  u32 mia = g_xf[XF_MATRIX_INDEX_A], mib = g_xf[XF_MATRIX_INDEX_B];
  g_stats.primitives++;
  g_stats.vertices += count;

  BeginDraw();
  BeginPrimitive(primitive);
  const u8* p = data;
  for (u32 v = 0; v < count; v++) {
    InputVertex in;
    in.posmtx = (u8)Bits(mia, 0, 6);
    for (int t = 0; t < 4; t++) in.texmtx[t] = (u8)Bits(mia, 6 + 6 * t, 6);
    for (int t = 0; t < 4; t++) in.texmtx[4 + t] = (u8)Bits(mib, 6 * t, 6);

    if (f.pnmtx) in.posmtx = *p++ & 0x3F;
    for (int t = 0; t < 8; t++)
      if (f.texmtx_mask & (1u << t)) in.texmtx[t] = *p++ & 0x3F;

    // Position
    {
      const u8* a = Attribute(p, f.pos_mode, 0, f.pos_comps * kFmtSize[f.pos_fmt]);
      in.position.x = ReadComponent(a, f.pos_fmt, f.pos_scale);
      in.position.y = ReadComponent(a, f.pos_fmt, f.pos_scale);
      in.position.z = f.pos_comps == 3 ? ReadComponent(a, f.pos_fmt, f.pos_scale) : 0.0f;
    }
    // Normal / binormal / tangent
    if (f.nrm_mode) {
      u32 vec_size = 3 * kFmtSize[f.nrm_fmt];
      int vecs = f.nbt ? 3 : 1;
      if (f.nrm_mode != 1 && f.nbt && f.nrm_index3) {
        for (int n = 0; n < 3; n++) {
          const u8* a = Attribute(p, f.nrm_mode, 1, vec_size);
          const u8* q = a + n * vec_size;
          in.normal[n].x = ReadComponent(q, f.nrm_fmt, f.nrm_scale);
          in.normal[n].y = ReadComponent(q, f.nrm_fmt, f.nrm_scale);
          in.normal[n].z = ReadComponent(q, f.nrm_fmt, f.nrm_scale);
        }
      } else {
        const u8* a = Attribute(p, f.nrm_mode, 1, vec_size * vecs);
        for (int n = 0; n < vecs; n++) {
          in.normal[n].x = ReadComponent(a, f.nrm_fmt, f.nrm_scale);
          in.normal[n].y = ReadComponent(a, f.nrm_fmt, f.nrm_scale);
          in.normal[n].z = ReadComponent(a, f.nrm_fmt, f.nrm_scale);
        }
      }
      for (int n = 0; n < vecs; n++) s_cached_normal[n] = in.normal[n];
      for (int n = vecs; n < 3; n++) in.normal[n] = s_cached_normal[n];
    } else {
      for (int n = 0; n < 3; n++) in.normal[n] = s_cached_normal[n];
    }
    // Colors: if only one color attribute is present it feeds channel 0.
    int ch = 0;
    memset(in.color, 0xFF, sizeof(in.color));
    for (int c = 0; c < 2; c++) {
      if (!f.col_mode[c]) continue;
      const u8* a = Attribute(p, f.col_mode[c], 2 + c, kColorSize[f.col_fmt[c]]);
      ReadColor(a, f.col_fmt[c], in.color[ch++]);
    }
    // Texture coordinates
    for (int t = 0; t < 8; t++) {
      in.texcoords[t][0] = in.texcoords[t][1] = 0.0f;
      if (!f.tc_mode[t]) continue;
      const u8* a = Attribute(p, f.tc_mode[t], 4 + t, f.tc_comps[t] * kFmtSize[f.tc_fmt[t]]);
      in.texcoords[t][0] = ReadComponent(a, f.tc_fmt[t], f.tc_scale[t]);
      if (f.tc_comps[t] == 2) in.texcoords[t][1] = ReadComponent(a, f.tc_fmt[t], f.tc_scale[t]);
    }

    OutputVertex out;
    TransformVertex(in, out);
    AddVertex(out);
  }
  EndPrimitive();
  EndDraw();
}

}  // namespace Video
