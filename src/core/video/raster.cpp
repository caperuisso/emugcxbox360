// SPDX-License-Identifier: GPL-2.0-or-later
// Triangle rasterizer and the TEV (texture environment) pixel pipeline.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/state.h"
#include "core/video/video_internal.h"
#include "platform/platform.h"

namespace Video {

namespace {

// ---- Scissor ----
int s_sc_left, s_sc_top, s_sc_right, s_sc_bottom;  // EFB coordinates, right/bottom exclusive
int s_x_off, s_y_off;                              // screen -> EFB offset

// ---- Interpolation ----
struct Slope {
  float f0, dfdx, dfdy;
  float x0, y0;  // reference point (pixel centre of v0)
  float At(float x, float y) const { return f0 + dfdx * (x - x0) + dfdy * (y - y0); }
};

struct SlopeContext {
  float x0, y0, dx10, dx20, dy10, dy20, det;
};

Slope MakeSlope(float f0, float f1, float f2, const SlopeContext& c) {
  Slope s;
  float d10 = f1 - f0, d20 = f2 - f0;
  s.dfdx = (d20 * c.dy10 - d10 * c.dy20) / c.det;
  s.dfdy = (c.dx20 * d10 - c.dx10 * d20) / c.det;
  s.f0 = f0;
  s.x0 = c.x0;
  s.y0 = c.y0;
  return s;
}


// ---- TEV state cached per triangle ----
struct S16x4 {
  s16 r, g, b, a;
};

u32 s_num_stages, s_num_texgens, s_num_ind, s_num_chans;
u32 s_ksel_swap[4][4];  // swap tables: source channel for R, G, B, A

inline s16 SignExtend11(u32 v) { return (s16)((s32)(v << 21) >> 21); }

}  // namespace

// TEV color registers are latched when written (the konst and normal banks
// share BP addresses), so LoadBP calls this.
S16x4 g_tev_color_regs[4];
S16x4 g_tev_konst_regs[4];

void RasterDoState(StateBuffer& s) {
  s.Do(g_tev_color_regs);
  s.Do(g_tev_konst_regs);
}

void SetTevRegister(u32 bp_reg, u32 value) {
  u32 index = (bp_reg - BP_TEV_REGS) >> 1;
  bool is_bg = (bp_reg - BP_TEV_REGS) & 1;
  bool konst = (value >> 23) & 1;
  S16x4& r = konst ? g_tev_konst_regs[index] : g_tev_color_regs[index];
  if (!is_bg) {
    r.r = SignExtend11(value & 0x7FF);
    r.a = SignExtend11((value >> 12) & 0x7FF);
  } else {
    r.b = SignExtend11(value & 0x7FF);
    r.g = SignExtend11((value >> 12) & 0x7FF);
  }
}

namespace {

// Konstant selector values 0..7 are fixed fractions.
const u8 kKonstFixed[8] = {255, 223, 191, 159, 128, 96, 64, 32};

inline S16x4 KonstColor(u32 sel) {
  if (sel < 8) {
    s16 v = kKonstFixed[sel];
    return {v, v, v, v};
  }
  if (sel >= 12 && sel <= 15) return g_tev_konst_regs[sel - 12];
  if (sel >= 16) {
    const S16x4& k = g_tev_konst_regs[(sel - 16) & 3];
    s16 v;
    switch ((sel - 16) >> 2) {
      case 0: v = k.r; break;
      case 1: v = k.g; break;
      case 2: v = k.b; break;
      default: v = k.a; break;
    }
    return {v, v, v, v};
  }
  return {0, 0, 0, 0};
}

bool AlphaCompare(int alpha, int ref, u32 comp) {
  switch (comp) {
    case 0: return false;
    case 1: return alpha < ref;
    case 2: return alpha == ref;
    case 3: return alpha <= ref;
    case 4: return alpha > ref;
    case 5: return alpha != ref;
    case 6: return alpha >= ref;
    default: return true;
  }
}

bool AlphaTest(int alpha) {
  u32 r = g_bp[BP_ALPHA_COMPARE];
  bool a = AlphaCompare(alpha, Bits(r, 0, 8), Bits(r, 16, 3));
  bool b = AlphaCompare(alpha, Bits(r, 8, 8), Bits(r, 19, 3));
  switch (Bits(r, 22, 2)) {
    case 0: return a && b;
    case 1: return a || b;
    case 2: return a != b;
    default: return a == b;
  }
}

// Fog parameters decoded from their 11-bit-mantissa float encoding.
float FogFloat(u32 v) {
  u32 mant = Bits(v, 0, 11), exp = Bits(v, 11, 8), sign = Bits(v, 19, 1);
  u32 bits = (sign << 31) | (exp << 23) | (mant << 12);
  return BitCast<float>(bits);
}

// TEV stage configuration decoded once per triangle
struct StageConfig {
  u32 texmap, texcoord, ras_chan, cc, ac, ind;
  bool tex_enable;
  S16x4 konst;
};
StageConfig s_stage[16];

struct PixelInput {
  int x, y;
  u32 z;
  u8 color[2][4];
  s32 uv[8][2];     // texel coordinates with 7 fractional bits
  s32 lod[16];
  bool linear[16];
  s32 ind_lod[4];
  bool ind_linear[4];
};

inline s32 WrapIndirect(s32 coord, u32 mode) {
  switch (mode) {
    case 0: return coord;
    case 1: return coord & ((256 << 7) - 1);
    case 2: return coord & ((128 << 7) - 1);
    case 3: return coord & ((64 << 7) - 1);
    case 4: return coord & ((32 << 7) - 1);
    case 5: return coord & ((16 << 7) - 1);
    default: return 0;
  }
}

bool ShadePixel(const PixelInput& in) {
  u32 zcomp = g_bp[BP_ZCOMPARE];
  u32 zmode = g_bp[BP_ZMODE];
  u32 z = in.z;
  bool early_z = Bits(zcomp, 6, 1) && !(g_bp[BP_ZTEX2] & 0xC);
  if (early_z && (zmode & 1) && !EFBDepthTest(in.x, in.y, z)) return false;

  S16x4 reg[4];
  for (int i = 0; i < 4; i++) reg[i] = g_tev_color_regs[i];

  // Indirect texture lookups
  u8 ind_tex[4][4] = {};
  u32 iref = g_bp[BP_IREF];
  for (u32 s = 0; s < s_num_ind; s++) {
    u32 map = Bits(iref, s * 6, 3), coord = Bits(iref, s * 6 + 3, 3);
    if (coord >= s_num_texgens) coord = 0;
    u32 scale = g_bp[BP_RAS1_SS0 + (s >> 1)];
    u32 ss = Bits(scale, (s & 1) * 8, 4), ts = Bits(scale, (s & 1) * 8 + 4, 4);
    SampleTexture(map, in.uv[coord][0] >> ss, in.uv[coord][1] >> ts, in.ind_lod[s], in.ind_linear[s], ind_tex[s]);
  }

  u8 raw_tex[4] = {0, 0, 0, 0};
  s32 tc_s = 0, tc_t = 0;
  u8 alpha_bump = 0;

  for (u32 stage = 0; stage <= s_num_stages; stage++) {
    const StageConfig& cfg = s_stage[stage];
    u32 texmap = cfg.texmap, texcoord = cfg.texcoord, ras_chan = cfg.ras_chan;
    bool tex_enable = cfg.tex_enable;
    u32 cc = cfg.cc, ac = cfg.ac;

    // Indirect stage: perturb texture coordinates
    if (cfg.ind == 0) {  // common case: no indirect texturing
      tc_s = in.uv[texcoord][0];
      tc_t = in.uv[texcoord][1];
      alpha_bump = 0;
    } else {
      u32 ind = cfg.ind;
      s32 s = in.uv[texcoord][0], t = in.uv[texcoord][1];
      const u8* map = ind_tex[Bits(ind, 0, 2)];
      u32 fmt = Bits(ind, 2, 2), bs = Bits(ind, 7, 2);
      switch (bs) {
        case 1: alpha_bump = map[3]; break;
        case 2: alpha_bump = map[2]; break;
        case 3: alpha_bump = map[1]; break;
        default: alpha_bump = 0; break;
      }
      s32 bias_val = fmt == 0 ? -128 : 1;
      s32 coord[3];
      static const u32 kShift[4] = {0, 3, 4, 5};
      coord[0] = (map[3] >> kShift[fmt]) + (Bits(ind, 4, 1) ? bias_val : 0);
      coord[1] = (map[2] >> kShift[fmt]) + (Bits(ind, 5, 1) ? bias_val : 0);
      coord[2] = (map[1] >> kShift[fmt]) + (Bits(ind, 6, 1) ? bias_val : 0);
      switch (fmt) {
        case 0: alpha_bump &= 0xF8; break;
        case 1: alpha_bump <<= 5; break;
        case 2: alpha_bump <<= 4; break;
        default: alpha_bump <<= 3; break;
      }
      s32 trans[2] = {0, 0};
      u32 mtx_index = Bits(ind, 9, 2), mtx_id = Bits(ind, 11, 2);
      if (mtx_index) {
        u32 base = BP_IND_MTX + (mtx_index - 1) * 3;
        u32 m0 = g_bp[base], m1 = g_bp[base + 1], m2 = g_bp[base + 2];
        auto s11 = [](u32 v) { return (s32)(v << 21) >> 21; };
        s32 ma = s11(m0 & 0x7FF), mb = s11((m0 >> 11) & 0x7FF);
        s32 mc = s11(m1 & 0x7FF), md = s11((m1 >> 11) & 0x7FF);
        s32 me = s11(m2 & 0x7FF), mf = s11((m2 >> 11) & 0x7FF);
        s32 scale = (s32)(Bits(m0, 22, 2) | (Bits(m1, 22, 2) << 2) | (Bits(m2, 22, 1) << 4)) - 17;
        s32 sh = -scale;  // 17 - exponent
        switch (mtx_id) {
          case 0:
            trans[0] = (ma * coord[0] + mc * coord[1] + me * coord[2]) >> 3;
            trans[1] = (mb * coord[0] + md * coord[1] + mf * coord[2]) >> 3;
            break;
          case 1:
            trans[0] = s * coord[0] / 256;
            trans[1] = t * coord[0] / 256;
            break;
          case 2:
            trans[0] = s * coord[1] / 256;
            trans[1] = t * coord[1] / 256;
            break;
          default: break;
        }
        trans[0] = sh >= 0 ? trans[0] >> sh : trans[0] << -sh;
        trans[1] = sh >= 0 ? trans[1] >> sh : trans[1] << -sh;
      }
      if (Bits(ind, 20, 1)) {
        tc_s += WrapIndirect(s, Bits(ind, 13, 3)) + trans[0];
        tc_t += WrapIndirect(t, Bits(ind, 16, 3)) + trans[1];
      } else {
        tc_s = WrapIndirect(s, Bits(ind, 13, 3)) + trans[0];
        tc_t = WrapIndirect(t, Bits(ind, 16, 3)) + trans[1];
      }
    }

    // Texture
    S16x4 tex = {0, 0, 0, 0};
    if (tex_enable) {
      u8 texel[4] = {0, 0, 0, 0};
      if (s_num_texgens > 0) SampleTexture(texmap, tc_s, tc_t, in.lod[stage], in.linear[stage], texel);
      memcpy(raw_tex, texel, 4);
      const u32* sw = s_ksel_swap[Bits(ac, 2, 2)];
      tex = {texel[sw[0]], texel[sw[1]], texel[sw[2]], texel[sw[3]]};
    }

    // Rasterized color
    S16x4 ras = {0, 0, 0, 0};
    {
      const u32* sw = s_ksel_swap[Bits(ac, 0, 2)];
      if (ras_chan == 0 || ras_chan == 1) {
        const u8* c = in.color[ras_chan];
        ras = {c[sw[0]], c[sw[1]], c[sw[2]], c[sw[3]]};
      } else if (ras_chan == 5) {
        ras = {alpha_bump, alpha_bump, alpha_bump, alpha_bump};
      } else if (ras_chan == 6) {
        s16 n = (s16)(alpha_bump | (alpha_bump >> 5));
        ras = {n, n, n, n};
      }
    }

    // Konstant
    const S16x4& konst = cfg.konst;

    // Gather inputs
    auto color_in = [&](u32 arg, int ch) -> s16 {
      auto pick = [ch](const S16x4& v) { return ch == 0 ? v.r : ch == 1 ? v.g : v.b; };
      switch (arg) {
        case 0: return pick(reg[0]);
        case 1: return reg[0].a;
        case 2: return pick(reg[1]);
        case 3: return reg[1].a;
        case 4: return pick(reg[2]);
        case 5: return reg[2].a;
        case 6: return pick(reg[3]);
        case 7: return reg[3].a;
        case 8: return pick(tex);
        case 9: return tex.a;
        case 10: return pick(ras);
        case 11: return ras.a;
        case 12: return 255;
        case 13: return 128;
        case 14: return pick(konst);
        default: return 0;
      }
    };
    auto alpha_in = [&](u32 arg) -> s16 {
      switch (arg) {
        case 0: return reg[0].a;
        case 1: return reg[1].a;
        case 2: return reg[2].a;
        case 3: return reg[3].a;
        case 4: return tex.a;
        case 5: return ras.a;
        case 6: return konst.a;
        default: return 0;
      }
    };

    static const int kLShift[4] = {0, 1, 2, 0};
    static const int kRShift[4] = {0, 0, 0, 1};
    static const int kBias[3] = {0, 128, -128};

    // Color combiner
    {
      u32 a_arg = Bits(cc, 12, 4), b_arg = Bits(cc, 8, 4), c_arg = Bits(cc, 4, 4), d_arg = Bits(cc, 0, 4);
      u32 bias = Bits(cc, 16, 2), op = Bits(cc, 18, 1), clamp = Bits(cc, 19, 1);
      u32 scale = Bits(cc, 20, 2), dest = Bits(cc, 22, 2);
      s16 out[3];
      if (bias != 3) {
        for (int ch = 0; ch < 3; ch++) {
          s32 a = color_in(a_arg, ch) & 0xFF, b = color_in(b_arg, ch) & 0xFF;
          s32 c = color_in(c_arg, ch) & 0xFF, d = color_in(d_arg, ch);
          d = (s32)(s16)(d << 5) >> 5;  // 11-bit signed
          s32 c2 = c + (c >> 7);
          s32 temp = a * (256 - c2) + b * c2;
          temp <<= kLShift[scale];
          temp += (scale == 3) ? 0 : (op ? 127 : 128);
          temp >>= 8;
          if (op) temp = -temp;
          s32 r = ((d + kBias[bias]) << kLShift[scale]) + temp;
          out[ch] = (s16)(r >> kRShift[scale]);
        }
      } else {
        u32 mode = scale;
        for (int ch = 0; ch < 3; ch++) {
          u32 a, b;
          auto av = [&](int c) { return (u32)(color_in(a_arg, c) & 0xFF); };
          auto bv = [&](int c) { return (u32)(color_in(b_arg, c) & 0xFF); };
          switch (mode) {
            case 0: a = av(0); b = bv(0); break;
            case 1: a = (av(1) << 8) | av(0); b = (bv(1) << 8) | bv(0); break;
            case 2: a = (av(2) << 16) | (av(1) << 8) | av(0); b = (bv(2) << 16) | (bv(1) << 8) | bv(0); break;
            default: a = av(ch); b = bv(ch); break;
          }
          bool pass = op ? (a == b) : (a > b);
          out[ch] = (s16)(color_in(d_arg, ch) + (pass ? (color_in(c_arg, ch) & 0xFF) : 0));
        }
      }
      for (int ch = 0; ch < 3; ch++) out[ch] = clamp ? std::clamp<s16>(out[ch], 0, 255) : std::clamp<s16>(out[ch], -1024, 1023);
      reg[dest].r = out[0];
      reg[dest].g = out[1];
      reg[dest].b = out[2];
    }

    // Alpha combiner
    {
      u32 a_arg = Bits(ac, 13, 3), b_arg = Bits(ac, 10, 3), c_arg = Bits(ac, 7, 3), d_arg = Bits(ac, 4, 3);
      u32 bias = Bits(ac, 16, 2), op = Bits(ac, 18, 1), clamp = Bits(ac, 19, 1);
      u32 scale = Bits(ac, 20, 2), dest = Bits(ac, 22, 2);
      s16 out;
      if (bias != 3) {
        s32 a = alpha_in(a_arg) & 0xFF, b = alpha_in(b_arg) & 0xFF, c = alpha_in(c_arg) & 0xFF;
        s32 d = (s32)(s16)(alpha_in(d_arg) << 5) >> 5;
        s32 c2 = c + (c >> 7);
        s32 temp = a * (256 - c2) + b * c2;
        temp <<= kLShift[scale];
        temp += (scale == 3) ? 0 : (op ? 127 : 128);
        temp = op ? (-temp >> 8) : (temp >> 8);
        s32 r = ((d + kBias[bias]) << kLShift[scale]) + temp;
        out = (s16)(r >> kRShift[scale]);
      } else {
        u32 a, b;
        auto colA = [&](int c) { return (u32)(color_in(Bits(cc, 12, 4), c) & 0xFF); };
        auto colB = [&](int c) { return (u32)(color_in(Bits(cc, 8, 4), c) & 0xFF); };
        switch (scale) {
          case 0: a = colA(0); b = colB(0); break;
          case 1: a = (colA(1) << 8) | colA(0); b = (colB(1) << 8) | colB(0); break;
          case 2: a = (colA(2) << 16) | (colA(1) << 8) | colA(0); b = (colB(2) << 16) | (colB(1) << 8) | colB(0); break;
          default: a = alpha_in(a_arg) & 0xFF; b = alpha_in(b_arg) & 0xFF; break;
        }
        bool pass = op ? (a == b) : (a > b);
        out = (s16)(alpha_in(d_arg) + (pass ? (alpha_in(c_arg) & 0xFF) : 0));
      }
      reg[dest].a = clamp ? std::clamp<s16>(out, 0, 255) : std::clamp<s16>(out, -1024, 1023);
    }
  }

  u32 last = s_num_stages;
  u32 cdest = Bits(g_bp[BP_TEV_COLOR_ENV + last * 2], 22, 2);
  u32 adest = Bits(g_bp[BP_TEV_COLOR_ENV + last * 2 + 1], 22, 2);
  u8 out[4] = {(u8)reg[cdest].r, (u8)reg[cdest].g, (u8)reg[cdest].b, (u8)reg[adest].a};

  if (!AlphaTest(out[3])) return false;

  // Z texture
  u32 ztex2 = g_bp[BP_ZTEX2];
  if (ztex2 & 0xC) {
    u32 zt = g_bp[BP_ZTEX1] & 0xFFFFFF;
    u32 op = Bits(ztex2, 2, 2);  // 0 disabled, 1 add, 2 replace; type in bits 0-1
    if (op) {
      switch (Bits(ztex2, 0, 2)) {
        case 0: zt += raw_tex[3]; break;
        case 1: zt += ((u32)raw_tex[3] << 8) | raw_tex[0]; break;
        default: zt += ((u32)raw_tex[0] << 16) | ((u32)raw_tex[1] << 8) | raw_tex[2]; break;
      }
      if (op == 1) zt += z;
      z = zt & 0xFFFFFF;
    }
  }

  // Fog
  u32 fog3 = g_bp[BP_FOG3];
  u32 fsel = Bits(fog3, 21, 3);
  if (fsel) {
    float a = FogFloat(g_bp[BP_FOG0]);
    float c = FogFloat(fog3);
    float ze;
    if (!Bits(fog3, 20, 1)) {  // perspective
      s32 denom = (s32)(g_bp[BP_FOG_B_MAG] & 0xFFFFFF) - (s32)(z >> (g_bp[BP_FOG_B_EXP] & 0x1F));
      ze = denom ? (a * 16777215.0f) / (float)denom : 0.0f;
    } else {
      ze = a * ((float)z / 16777215.0f);
    }
    ze -= c;
    float fog = std::clamp(ze, 0.0f, 1.0f);
    switch (fsel) {
      case 4: fog = 1.0f - std::pow(2.0f, -8.0f * fog); break;
      case 5: fog = 1.0f - std::pow(2.0f, -8.0f * fog * fog); break;
      case 6: fog = std::pow(2.0f, -8.0f * (1.0f - fog)); break;
      case 7: fog = std::pow(2.0f, -8.0f * (1.0f - fog) * (1.0f - fog)); break;
      default: break;
    }
    u32 fi = (u32)(fog * 256), inv = 256 - fi;
    u32 fc = g_bp[BP_FOG_COLOR];
    u8 fr = (u8)(fc >> 16), fg = (u8)(fc >> 8), fb = (u8)fc;
    out[0] = (u8)((out[0] * inv + fi * fr) >> 8);
    out[1] = (u8)((out[1] * inv + fi * fg) >> 8);
    out[2] = (u8)((out[2] * inv + fi * fb) >> 8);
  }

  if (!early_z && (zmode & 1) && !EFBDepthTest(in.x, in.y, z)) return false;
  if ((zmode & 1) && (zmode & 0x10)) EFBWriteDepth(in.x, in.y, z);
  EFBBlend(in.x, in.y, out);
  return true;
}

// Fixed-point log2 used for texture LOD (4 fractional bits), as in hardware.
inline s32 FixedLog2(float f) {
  u32 x = BitCast<u32>(f);
  s32 log_int = (s32)((x & 0x7F800000) >> 19) - 2032;
  s32 log_frac = (s32)((x & 0x007FFFFF) >> 19);
  return log_int + log_frac;
}

}  // namespace

void ComputeLOD(u32 texmap, float dsdx, float dsdy, float dtdx, float dtdy, s32& lod, bool& linear) {
  u32 mode0 = g_bp[TexReg(0x80, texmap)], mode1 = g_bp[TexReg(0x84, texmap)];
  float s_delta, t_delta;
  if (Bits(mode0, 8, 1)) {  // diagonal LOD
    s_delta = std::fabs(dsdx) + std::fabs(dsdy);
    t_delta = std::fabs(dtdx) + std::fabs(dtdy);
  } else {
    s_delta = std::max(std::fabs(dsdx), std::fabs(dsdy));
    t_delta = std::max(std::fabs(dtdx), std::fabs(dtdy));
  }
  s32 l = FixedLog2(std::max(s_delta, t_delta));
  s32 bias = (s32)(s8)Bits(mode0, 9, 8);
  l += bias >> 1;
  linear = (l > 0 && Bits(mode0, 7, 1)) || (l <= 0 && Bits(mode0, 4, 1));
  s32 max_lod = (s32)Bits(mode1, 8, 8), min_lod = (s32)Bits(mode1, 0, 8);
  l = std::clamp(l, min_lod, std::max(min_lod, max_lod));
  lod = l;
}

void UpdateScissor() {
  u32 tl = g_bp[BP_SCISSOR_TL], br = g_bp[BP_SCISSOR_BR], off = g_bp[BP_SCISSOR_OFFSET];
  s_x_off = (int)Bits(off, 0, 10) * 2 - 342;
  s_y_off = (int)Bits(off, 10, 10) * 2 - 342;
  // Scissor registers and the viewport both carry the 342 pixel offset.
  s_sc_left = (int)Bits(tl, 12, 12) - 342 - s_x_off;
  s_sc_top = (int)Bits(tl, 0, 12) - 342 - s_y_off;
  s_sc_right = (int)Bits(br, 12, 12) - 342 - s_x_off + 1;
  s_sc_bottom = (int)Bits(br, 0, 12) - 342 - s_y_off + 1;
  s_sc_left = std::max(s_sc_left, 0);
  s_sc_top = std::max(s_sc_top, 0);
  s_sc_right = std::min(s_sc_right, EFB_WIDTH);
  s_sc_bottom = std::min(s_sc_bottom, EFB_HEIGHT);
}

void LoadSwapTables() {
  for (int t = 0; t < 4; t++) {
    u32 k0 = g_bp[BP_TEV_KSEL + t * 2], k1 = g_bp[BP_TEV_KSEL + t * 2 + 1];
    s_ksel_swap[t][0] = Bits(k0, 0, 2);
    s_ksel_swap[t][1] = Bits(k0, 2, 2);
    s_ksel_swap[t][2] = Bits(k1, 0, 2);
    s_ksel_swap[t][3] = Bits(k1, 2, 2);
  }
}

namespace {

// Everything a worker needs to rasterize one triangle (read-only once queued).
struct TriSetup {
  int minx, maxx, miny, maxy;
  s64 C1, C2, C3;
  s32 DX12, DX23, DX31, DY12, DY23, DY31;
  bool flip;
  Slope z, w, color[2][4], tex[8][3];
};

std::vector<TriSetup> s_batch;  // triangles of the current draw call
u64 s_batch_area = 0;
u32 s_iref;

void RasterizeTriangle(const TriSetup& t, int task, int tasks, u32& pixels) {
  auto inside = [&](int x, int y) {
    s64 px = (s64)x << 4, py = (s64)y << 4;
    s64 e1 = t.C1 + t.DX12 * py - t.DY12 * px;
    s64 e2 = t.C2 + t.DX23 * py - t.DY23 * px;
    s64 e3 = t.C3 + t.DX31 * py - t.DY31 * px;
    if (t.flip) return e1 < 0 && e2 < 0 && e3 < 0;
    return e1 > 0 && e2 > 0 && e3 > 0;
  };

  PixelInput px;
  memset(&px, 0, sizeof(px));  // unused color channels read as zero
  // Process 2x2 blocks so texture LOD can use screen-space derivatives.
  // Block rows are interleaved between workers, which therefore never touch
  // the same EFB pixels.
  for (int by = t.miny & ~1; by < t.maxy; by += 2) {
    if (tasks > 1 && ((by >> 1) % tasks) != task) continue;
    for (int bx = t.minx & ~1; bx < t.maxx; bx += 2) {
      bool cover[2][2];
      bool any = false;
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          int x = bx + i, y = by + j;
          cover[i][j] = x >= t.minx && x < t.maxx && y >= t.miny && y < t.maxy && inside(x, y);
          any |= cover[i][j];
        }
      if (!any) continue;

      // Perspective-correct texture coordinates for the block
      float uv[2][2][8][2];
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          float cx = bx + i + 0.5f, cy = by + j + 0.5f;
          float inv_w = 1.0f / t.w.At(cx, cy);
          for (u32 c = 0; c < s_num_texgens; c++) {
            float proj = inv_w;
            float q = t.tex[c][2].At(cx, cy) * inv_w;
            if (q != 0.0f) proj = inv_w / q;
            uv[i][j][c][0] = t.tex[c][0].At(cx, cy) * proj;
            uv[i][j][c][1] = t.tex[c][1].At(cx, cy) * proj;
          }
        }
      // LOD per TEV stage / indirect stage
      for (u32 st = 0; st <= s_num_stages; st++) {
        const StageConfig& cfg = s_stage[st];
        if (!cfg.tex_enable) continue;
        u32 tc = cfg.texcoord;
        ComputeLOD(cfg.texmap, uv[0][0][tc][0] - uv[1][0][tc][0], uv[0][0][tc][0] - uv[0][1][tc][0],
                   uv[0][0][tc][1] - uv[1][0][tc][1], uv[0][0][tc][1] - uv[0][1][tc][1], px.lod[st], px.linear[st]);
      }
      for (u32 st = 0; st < s_num_ind; st++) {
        u32 map = Bits(s_iref, st * 6, 3), tc = Bits(s_iref, st * 6 + 3, 3);
        if (tc >= s_num_texgens) tc = 0;
        ComputeLOD(map, uv[0][0][tc][0] - uv[1][0][tc][0], uv[0][0][tc][0] - uv[0][1][tc][0],
                   uv[0][0][tc][1] - uv[1][0][tc][1], uv[0][0][tc][1] - uv[0][1][tc][1], px.ind_lod[st],
                   px.ind_linear[st]);
      }

      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          if (!cover[i][j]) continue;
          int x = bx + i, y = by + j;
          float cx = x + 0.5f, cy = y + 0.5f;
          px.x = x;
          px.y = y;
          px.z = (u32)std::clamp(t.z.At(cx, cy), 0.0f, 16777215.0f);
          for (u32 c = 0; c < std::min<u32>(s_num_chans, 2); c++)
            for (int k = 0; k < 4; k++)
              px.color[c][k] = (u8)std::clamp(t.color[c][k].At(cx, cy), 0.0f, 255.0f);
          for (u32 c = 0; c < s_num_texgens; c++) {
            px.uv[c][0] = (s32)(uv[i][j][c][0] * 128.0f);
            px.uv[c][1] = (s32)(uv[i][j][c][1] * 128.0f);
          }
          if (ShadePixel(px)) pixels++;
        }
    }
  }
}

struct RasterJob {
  u32 pixels[16];
};

void RasterTask(int task, int tasks, void* ctx) {
  RasterJob* job = (RasterJob*)ctx;
  u32 pixels = 0;
  for (const TriSetup& t : s_batch) RasterizeTriangle(t, task, tasks, pixels);
  job->pixels[task] = pixels;
}

}  // namespace

void BeginDraw() {
  u32 genmode = g_bp[BP_GENMODE];
  s_num_texgens = Bits(genmode, 0, 4);
  s_num_chans = Bits(genmode, 4, 3);
  s_num_stages = Bits(genmode, 10, 4);
  s_num_ind = Bits(genmode, 16, 3);
  s_iref = g_bp[BP_IREF];
  LoadSwapTables();
  for (u32 stage = 0; stage <= s_num_stages; stage++) {
    StageConfig& c = s_stage[stage];
    u32 order = g_bp[BP_TREF + (stage >> 1)];
    u32 shift = (stage & 1) ? 12 : 0;
    c.texmap = Bits(order, shift, 3);
    c.texcoord = Bits(order, shift + 3, 3);
    c.tex_enable = Bits(order, shift + 6, 1);
    c.ras_chan = Bits(order, shift + 7, 3);
    if (c.texcoord >= s_num_texgens) c.texcoord = 0;
    c.cc = g_bp[BP_TEV_COLOR_ENV + stage * 2];
    c.ac = g_bp[BP_TEV_COLOR_ENV + stage * 2 + 1];
    c.ind = g_bp[BP_IND_CMD + stage] & 0x1FFFFF;
    u32 ksel = g_bp[BP_TEV_KSEL + (stage >> 1)];
    u32 kshift = (stage & 1) ? 14 : 4;
    S16x4 kc = KonstColor(Bits(ksel, kshift, 5));
    S16x4 ka = KonstColor(Bits(ksel, kshift + 5, 5));
    c.konst = {kc.r, kc.g, kc.b, ka.a};
  }
  s_batch.clear();
  s_batch_area = 0;
}

void EndDraw() {
  if (s_batch.empty()) return;
  // Small batches are not worth waking the workers.
  int workers = g_host ? g_host->ParallelWorkers() : 1;
  int tasks = (workers > 1 && s_batch_area >= 4096) ? std::min(workers, 16) : 1;
  RasterJob job;
  memset(&job, 0, sizeof(job));
  if (tasks > 1)
    g_host->RunParallel(tasks, RasterTask, &job);
  else
    RasterTask(0, 1, &job);
  for (int i = 0; i < tasks; i++) g_stats.pixels += job.pixels[i];
  s_batch.clear();
  s_batch_area = 0;
}

void DrawTriangle(const OutputVertex* v0, const OutputVertex* v1, const OutputVertex* v2) {
  TriSetup t;
  // Screen -> EFB coordinates
  float xs[3] = {v0->screen.x - 342.0f - s_x_off, v1->screen.x - 342.0f - s_x_off, v2->screen.x - 342.0f - s_x_off};
  float ys[3] = {v0->screen.y - 342.0f - s_y_off, v1->screen.y - 342.0f - s_y_off, v2->screen.y - 342.0f - s_y_off};

  // 28.4 fixed point edge setup (hardware samples pixel centres)
  auto fx = [](float v) { return (s32)std::floor(v * 16.0f + 0.5f); };
  s32 X1 = fx(xs[0]), X2 = fx(xs[1]), X3 = fx(xs[2]);
  s32 Y1 = fx(ys[0]), Y2 = fx(ys[1]), Y3 = fx(ys[2]);

  t.minx = std::max((std::min({X1, X2, X3}) + 0xF) >> 4, s_sc_left);
  t.maxx = std::min((std::max({X1, X2, X3}) + 0xF) >> 4, s_sc_right);
  t.miny = std::max((std::min({Y1, Y2, Y3}) + 0xF) >> 4, s_sc_top);
  t.maxy = std::min((std::max({Y1, Y2, Y3}) + 0xF) >> 4, s_sc_bottom);
  if (t.minx >= t.maxx || t.miny >= t.maxy) return;

  SlopeContext ctx;
  ctx.x0 = xs[0];
  ctx.y0 = ys[0];
  ctx.dx10 = xs[1] - xs[0];
  ctx.dx20 = xs[2] - xs[0];
  ctx.dy10 = ys[1] - ys[0];
  ctx.dy20 = ys[2] - ys[0];
  ctx.det = ctx.dx20 * ctx.dy10 - ctx.dx10 * ctx.dy20;
  if (ctx.det == 0.0f) return;

  t.z = MakeSlope(v0->screen.z, v1->screen.z, v2->screen.z, ctx);
  float w[3] = {1.0f / v0->proj[3], 1.0f / v1->proj[3], 1.0f / v2->proj[3]};
  t.w = MakeSlope(w[0], w[1], w[2], ctx);
  for (u32 c = 0; c < 2; c++)
    for (int i = 0; i < 4; i++) t.color[c][i] = MakeSlope(v0->color[c][i], v1->color[c][i], v2->color[c][i], ctx);
  for (u32 c = 0; c < s_num_texgens; c++) {
    t.tex[c][0] = MakeSlope(v0->texcoords[c].x * w[0], v1->texcoords[c].x * w[1], v2->texcoords[c].x * w[2], ctx);
    t.tex[c][1] = MakeSlope(v0->texcoords[c].y * w[0], v1->texcoords[c].y * w[1], v2->texcoords[c].y * w[2], ctx);
    t.tex[c][2] = MakeSlope(v0->texcoords[c].z * w[0], v1->texcoords[c].z * w[1], v2->texcoords[c].z * w[2], ctx);
  }

  // Edge equations (top-left fill convention)
  t.DX12 = X1 - X2;
  t.DX23 = X2 - X3;
  t.DX31 = X3 - X1;
  t.DY12 = Y1 - Y2;
  t.DY23 = Y2 - Y3;
  t.DY31 = Y3 - Y1;
  t.C1 = (s64)t.DY12 * X1 - (s64)t.DX12 * Y1;
  t.C2 = (s64)t.DY23 * X2 - (s64)t.DX23 * Y2;
  t.C3 = (s64)t.DY31 * X3 - (s64)t.DX31 * Y3;
  if (t.DY12 < 0 || (t.DY12 == 0 && t.DX12 > 0)) t.C1++;
  if (t.DY23 < 0 || (t.DY23 == 0 && t.DX23 > 0)) t.C2++;
  if (t.DY31 < 0 || (t.DY31 == 0 && t.DX31 > 0)) t.C3++;
  // Orientation: make the inside test "> 0" regardless of winding.
  t.flip = ((s64)t.DX12 * t.DY31 - (s64)t.DY12 * t.DX31) < 0;

  s_batch_area += (u64)(t.maxx - t.minx) * (u64)(t.maxy - t.miny);
  s_batch.push_back(t);
}

}  // namespace Video
