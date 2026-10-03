// SPDX-License-Identifier: GPL-2.0-or-later
// Triangle rasterizer and the TEV (texture environment) pixel pipeline.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/state.h"
#include "core/video/gpu_backend.h"
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
u32 s_iref;
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
// TEV stage configuration, decoded once per draw call
struct StageConfig {
  u32 texmap, texcoord, ras_chan, cc, ac, ind;
  bool tex_enable;
  S16x4 konst;
  // Color combiner
  u8 c_a, c_b, c_c, c_d, c_bias, c_op, c_clamp, c_scale, c_dest;
  // Alpha combiner
  u8 a_a, a_b, a_c, a_d, a_bias, a_op, a_clamp, a_scale, a_dest;
  const u32* tex_swap;
  const u32* ras_swap;
};
StageConfig s_stage[16];

// Per-draw pixel pipeline state (everything after the TEV)
struct PixelState {
  bool early_z, z_enable, z_write;
  u32 z_func;
  bool alpha_pass[256];
  u32 last_cdest, last_adest;
  u32 ztex_op, ztex_type, ztex_bias;
  u32 fog_sel;
  bool fog_ortho;
  float fog_a, fog_c;
  s32 fog_b_mag;
  u32 fog_b_exp;
  u8 fog_r, fog_g, fog_b;
};
PixelState s_ps;

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

inline bool DepthCompare(u32 func, u32 a, u32 b) {
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

// Color operand: the selected source's RGB (or its alpha broadcast).
inline S16x4 ColorArg(u32 arg, const S16x4* reg, const S16x4& tex, const S16x4& ras, const S16x4& konst) {
  switch (arg) {
    case 0: return reg[0];
    case 1: return {reg[0].a, reg[0].a, reg[0].a, 0};
    case 2: return reg[1];
    case 3: return {reg[1].a, reg[1].a, reg[1].a, 0};
    case 4: return reg[2];
    case 5: return {reg[2].a, reg[2].a, reg[2].a, 0};
    case 6: return reg[3];
    case 7: return {reg[3].a, reg[3].a, reg[3].a, 0};
    case 8: return tex;
    case 9: return {tex.a, tex.a, tex.a, 0};
    case 10: return ras;
    case 11: return {ras.a, ras.a, ras.a, 0};
    case 12: return {255, 255, 255, 0};
    case 13: return {128, 128, 128, 0};
    case 14: return konst;
    default: return {0, 0, 0, 0};
  }
}

inline s16 AlphaArg(u32 arg, const S16x4* reg, const S16x4& tex, const S16x4& ras, const S16x4& konst) {
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
}

constexpr int kLShift[4] = {0, 1, 2, 0};
constexpr int kRShift[4] = {0, 0, 0, 1};
constexpr int kBias[3] = {0, 128, -128};

// The TEV lerp: d + (a*(1-c) + b*c), with bias, sign, scale.
inline s16 TevLerp(s32 a, s32 b, s32 c, s32 d, u32 bias, u32 op, u32 scale) {
  a &= 0xFF, b &= 0xFF, c &= 0xFF;
  d = (s32)(s16)(d << 5) >> 5;  // 11-bit signed
  s32 c2 = c + (c >> 7);
  s32 temp = a * (256 - c2) + b * c2;
  temp <<= kLShift[scale];
  temp += (scale == 3) ? 0 : (op ? 127 : 128);
  temp = op ? (-temp >> 8) : (temp >> 8);
  s32 r = ((d + kBias[bias]) << kLShift[scale]) + temp;
  return (s16)(r >> kRShift[scale]);
}

inline s16 Clamp11(s16 v, bool clamp) {
  return clamp ? (v < 0 ? 0 : (v > 255 ? 255 : v)) : (v < -1024 ? -1024 : (v > 1023 ? 1023 : v));
}

bool ShadePixel(const PixelInput& in) {
  const PixelState& ps = s_ps;
  u32 z = in.z;
  if (ps.early_z && ps.z_enable && !EFBDepthTest(in.x, in.y, z, ps.z_func)) return false;

  S16x4 reg[4];
  for (int i = 0; i < 4; i++) reg[i] = g_tev_color_regs[i];

  // Indirect texture lookups
  u8 ind_tex[4][4] = {};
  u32 iref = s_iref;
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

    // Indirect stage: perturb texture coordinates
    if (cfg.ind == 0) {  // common case: no indirect texturing
      tc_s = in.uv[cfg.texcoord][0];
      tc_t = in.uv[cfg.texcoord][1];
      alpha_bump = 0;
    } else {
      u32 ind = cfg.ind;
      s32 s = in.uv[cfg.texcoord][0], t = in.uv[cfg.texcoord][1];
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
    if (cfg.tex_enable) {
      u8 texel[4] = {0, 0, 0, 0};
      if (s_num_texgens > 0) SampleTexture(cfg.texmap, tc_s, tc_t, in.lod[stage], in.linear[stage], texel);
      memcpy(raw_tex, texel, 4);
      const u32* sw = cfg.tex_swap;
      tex = {texel[sw[0]], texel[sw[1]], texel[sw[2]], texel[sw[3]]};
    }

    // Rasterized color
    S16x4 ras = {0, 0, 0, 0};
    if (cfg.ras_chan <= 1) {
      const u8* c = in.color[cfg.ras_chan];
      const u32* sw = cfg.ras_swap;
      ras = {c[sw[0]], c[sw[1]], c[sw[2]], c[sw[3]]};
    } else if (cfg.ras_chan == 5) {
      ras = {alpha_bump, alpha_bump, alpha_bump, alpha_bump};
    } else if (cfg.ras_chan == 6) {
      s16 n = (s16)(alpha_bump | (alpha_bump >> 5));
      ras = {n, n, n, n};
    }

    const S16x4& konst = cfg.konst;

    // Color combiner
    {
      S16x4 va = ColorArg(cfg.c_a, reg, tex, ras, konst), vb = ColorArg(cfg.c_b, reg, tex, ras, konst);
      S16x4 vc = ColorArg(cfg.c_c, reg, tex, ras, konst), vd = ColorArg(cfg.c_d, reg, tex, ras, konst);
      s16 out[3];
      if (cfg.c_bias != 3) {
        out[0] = TevLerp(va.r, vb.r, vc.r, vd.r, cfg.c_bias, cfg.c_op, cfg.c_scale);
        out[1] = TevLerp(va.g, vb.g, vc.g, vd.g, cfg.c_bias, cfg.c_op, cfg.c_scale);
        out[2] = TevLerp(va.b, vb.b, vc.b, vd.b, cfg.c_bias, cfg.c_op, cfg.c_scale);
      } else {
        const s16 a3[3] = {va.r, va.g, va.b}, b3[3] = {vb.r, vb.g, vb.b};
        const s16 c3[3] = {vc.r, vc.g, vc.b}, d3[3] = {vd.r, vd.g, vd.b};
        for (int ch = 0; ch < 3; ch++) {
          u32 a, b;
          switch (cfg.c_scale) {
            case 0: a = a3[0] & 0xFF; b = b3[0] & 0xFF; break;
            case 1: a = ((a3[1] & 0xFF) << 8) | (a3[0] & 0xFF); b = ((b3[1] & 0xFF) << 8) | (b3[0] & 0xFF); break;
            case 2:
              a = ((a3[2] & 0xFF) << 16) | ((a3[1] & 0xFF) << 8) | (a3[0] & 0xFF);
              b = ((b3[2] & 0xFF) << 16) | ((b3[1] & 0xFF) << 8) | (b3[0] & 0xFF);
              break;
            default: a = a3[ch] & 0xFF; b = b3[ch] & 0xFF; break;
          }
          bool pass = cfg.c_op ? (a == b) : (a > b);
          out[ch] = (s16)(d3[ch] + (pass ? (c3[ch] & 0xFF) : 0));
        }
      }
      S16x4& dst = reg[cfg.c_dest];
      dst.r = Clamp11(out[0], cfg.c_clamp);
      dst.g = Clamp11(out[1], cfg.c_clamp);
      dst.b = Clamp11(out[2], cfg.c_clamp);

      // Alpha combiner
      s16 aa = AlphaArg(cfg.a_a, reg, tex, ras, konst), ab = AlphaArg(cfg.a_b, reg, tex, ras, konst);
      s16 ac = AlphaArg(cfg.a_c, reg, tex, ras, konst), ad = AlphaArg(cfg.a_d, reg, tex, ras, konst);
      s16 aout;
      if (cfg.a_bias != 3) {
        aout = TevLerp(aa, ab, ac, ad, cfg.a_bias, cfg.a_op, cfg.a_scale);
      } else {
        // Packed compares read the color combiner's A/B operands, after this
        // stage's color result has been written.
        if (cfg.a_scale != 3) {
          va = ColorArg(cfg.c_a, reg, tex, ras, konst);
          vb = ColorArg(cfg.c_b, reg, tex, ras, konst);
        }
        u32 a, b;
        switch (cfg.a_scale) {
          case 0: a = va.r & 0xFF; b = vb.r & 0xFF; break;
          case 1: a = ((va.g & 0xFF) << 8) | (va.r & 0xFF); b = ((vb.g & 0xFF) << 8) | (vb.r & 0xFF); break;
          case 2:
            a = ((va.b & 0xFF) << 16) | ((va.g & 0xFF) << 8) | (va.r & 0xFF);
            b = ((vb.b & 0xFF) << 16) | ((vb.g & 0xFF) << 8) | (vb.r & 0xFF);
            break;
          default: a = aa & 0xFF; b = ab & 0xFF; break;
        }
        bool pass = cfg.a_op ? (a == b) : (a > b);
        aout = (s16)(ad + (pass ? (ac & 0xFF) : 0));
      }
      reg[cfg.a_dest].a = Clamp11(aout, cfg.a_clamp);
    }
  }

  u8 out[4] = {(u8)reg[ps.last_cdest].r, (u8)reg[ps.last_cdest].g, (u8)reg[ps.last_cdest].b,
               (u8)reg[ps.last_adest].a};

  if (!ps.alpha_pass[out[3]]) return false;

  // Z texture
  if (ps.ztex_op) {
    u32 zt = ps.ztex_bias;
    switch (ps.ztex_type) {
      case 0: zt += raw_tex[3]; break;
      case 1: zt += ((u32)raw_tex[3] << 8) | raw_tex[0]; break;
      default: zt += ((u32)raw_tex[0] << 16) | ((u32)raw_tex[1] << 8) | raw_tex[2]; break;
    }
    if (ps.ztex_op == 1) zt += z;
    z = zt & 0xFFFFFF;
  }

  // Fog
  if (ps.fog_sel) {
    float ze;
    if (!ps.fog_ortho) {  // perspective
      s32 denom = ps.fog_b_mag - (s32)(z >> ps.fog_b_exp);
      ze = denom ? (ps.fog_a * 16777215.0f) / (float)denom : 0.0f;
    } else {
      ze = ps.fog_a * ((float)z / 16777215.0f);
    }
    ze -= ps.fog_c;
    float fog = std::clamp(ze, 0.0f, 1.0f);
    switch (ps.fog_sel) {
      case 4: fog = 1.0f - std::pow(2.0f, -8.0f * fog); break;
      case 5: fog = 1.0f - std::pow(2.0f, -8.0f * fog * fog); break;
      case 6: fog = std::pow(2.0f, -8.0f * (1.0f - fog)); break;
      case 7: fog = std::pow(2.0f, -8.0f * (1.0f - fog) * (1.0f - fog)); break;
      default: break;
    }
    u32 fi = (u32)(fog * 256), inv = 256 - fi;
    out[0] = (u8)((out[0] * inv + fi * ps.fog_r) >> 8);
    out[1] = (u8)((out[1] * inv + fi * ps.fog_g) >> 8);
    out[2] = (u8)((out[2] * inv + fi * ps.fog_b) >> 8);
  }

  if (!ps.early_z && ps.z_enable && !EFBDepthTest(in.x, in.y, z, ps.z_func)) return false;
  if (ps.z_enable && ps.z_write) EFBWriteDepth(in.x, in.y, z);
  EFBBlend(in.x, in.y, out);
  return true;
}

// Fixed-point log2 (4 fractional bits) of a texel delta given with 7
// fractional bits, as the hardware computes texture LOD. Same result as
// taking the float exponent and the top 4 mantissa bits.
inline s32 FixedLog2(u32 m) {
  if (m == 0) return -2032;  // log2(0): smallest value, clamped by min LOD
  s32 p = 31 - __builtin_clz(m);
  u32 frac = ((m << (31 - p)) >> 27) & 0xF;
  return (p - 7) * 16 + (s32)frac;
}

}  // namespace

void ComputeLOD(u32 texmap, s32 dsdx, s32 dsdy, s32 dtdx, s32 dtdy, s32& lod, bool& linear) {
  u32 mode0 = g_bp[TexReg(0x80, texmap)], mode1 = g_bp[TexReg(0x84, texmap)];
  u32 asx = (u32)std::abs(dsdx), asy = (u32)std::abs(dsdy), atx = (u32)std::abs(dtdx), aty = (u32)std::abs(dtdy);
  u32 s_delta, t_delta;
  if (Bits(mode0, 8, 1)) {  // diagonal LOD
    s_delta = asx + asy;
    t_delta = atx + aty;
  } else {
    s_delta = std::max(asx, asy);
    t_delta = std::max(atx, aty);
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
// Attribute plane in 16.16 fixed point: value at the centre of pixel (x, y)
// is (c + dx * x + dy * y) >> 16. Integer only, so no float<->int
// conversions per pixel (very slow on the Xenon: load-hit-store stalls).
struct FixedPlane {
  s64 c, dx, dy;
};

FixedPlane MakeFixedPlane(const Slope& s) {
  FixedPlane p;
  double c = (double)s.f0 + (double)s.dfdx * (0.5 - (double)s.x0) + (double)s.dfdy * (0.5 - (double)s.y0);
  p.c = (s64)std::llround(c * 65536.0);
  p.dx = (s64)std::llround((double)s.dfdx * 65536.0);
  p.dy = (s64)std::llround((double)s.dfdy * 65536.0);
  return p;
}

struct TriSetup {
  int minx, maxx, miny, maxy;
  s64 C1, C2, C3;
  s32 DX12, DX23, DX31, DY12, DY23, DY31;
  bool flip;
  FixedPlane z, color[2][4];
  Slope w, tex[8][3];
};

std::vector<TriSetup> s_batch;  // triangles of the current draw call
std::vector<GpuVertex> s_gpu_batch;  // same, for a hardware backend
u32 s_tex_mask;
u64 s_batch_area = 0;

constexpr int SPAN = 8;  // perspective-correct texture coordinates every SPAN pixels

// Texel coordinates (7 fractional bits) of texgen c at pixel centre (x, y),
// perspective correct.
inline void EvalUV(const TriSetup& t, int x, int y, s32 out[8][2]) {
  float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
  float inv_w = 1.0f / t.w.At(cx, cy);
  for (u32 c = 0; c < s_num_texgens; c++) {
    float proj = inv_w;
    float q = t.tex[c][2].At(cx, cy) * inv_w;
    if (q != 0.0f) proj = inv_w / q;
    out[c][0] = (s32)(t.tex[c][0].At(cx, cy) * proj * 128.0f);
    out[c][1] = (s32)(t.tex[c][1].At(cx, cy) * proj * 128.0f);
  }
}

inline u8 PlaneU8(const FixedPlane& p, s64 x, s64 y) {
  s64 v = (p.c + p.dx * x + p.dy * y) >> 16;
  return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void RasterizeTriangle(const TriSetup& t, int task, int tasks, u32& pixels) {
  PixelInput px;
  memset(&px, 0, sizeof(px));  // unused color channels read as zero
  const u32 texgens = s_num_texgens, chans = std::min<u32>(s_num_chans, 2);
  // Edge steps for one pixel (coordinates are 28.4 fixed point)
  const s64 e1x = -(s64)t.DY12 * 16, e2x = -(s64)t.DY23 * 16, e3x = -(s64)t.DY31 * 16;
  const s64 e1y = (s64)t.DX12 * 16, e2y = (s64)t.DX23 * 16, e3y = (s64)t.DX31 * 16;
  const int x_start = t.minx & ~1;

  // Process 2x2 blocks so texture LOD can use screen-space derivatives.
  // Block rows are interleaved between workers, which therefore never touch
  // the same EFB pixels.
  for (int by = t.miny & ~1; by < t.maxy; by += 2) {
    if (tasks > 1 && ((by >> 1) % tasks) != task) continue;
    // Edge values at (x_start, by)
    s64 px0 = (s64)x_start << 4, py0 = (s64)by << 4;
    s64 e1 = t.C1 + t.DX12 * py0 - t.DY12 * px0;
    s64 e2 = t.C2 + t.DX23 * py0 - t.DY23 * px0;
    s64 e3 = t.C3 + t.DX31 * py0 - t.DY31 * px0;

    s32 uv_l[2][8][2], uv_r[2][8][2];  // span ends, rows by and by+1
    int span_x = -1000000;
    s32 span_recip = 0;  // 65536 / span length

    for (int bx = x_start; bx < t.maxx; bx += 2, e1 += 2 * e1x, e2 += 2 * e2x, e3 += 2 * e3x) {
      bool cover[2][2];
      bool any = false;
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          int x = bx + i, y = by + j;
          s64 f1 = e1 + i * e1x + j * e1y, f2 = e2 + i * e2x + j * e2y, f3 = e3 + i * e3x + j * e3y;
          bool in = t.flip ? (f1 < 0 && f2 < 0 && f3 < 0) : (f1 > 0 && f2 > 0 && f3 > 0);
          cover[i][j] = in && x >= t.minx && x < t.maxx && y >= t.miny && y < t.maxy;
          any |= cover[i][j];
        }
      if (!any) continue;

      // Texture coordinates of the block, interpolated within SPAN-pixel spans
      s32 uv[2][2][8][2];
      if (texgens) {
        if (bx >= span_x + SPAN || bx < span_x) {
          // Exact values at both ends of the span (kept inside the bounding
          // box: w can be meaningless far outside the triangle).
          span_x = bx;
          int n = std::min(SPAN, std::max(1, t.maxx - 1 - span_x));
          span_recip = 65536 / n;
          for (int j = 0; j < 2; j++) {
            int y = std::min(by + j, t.maxy - 1);
            EvalUV(t, span_x, y, uv_l[j]);
            EvalUV(t, span_x + n, y, uv_r[j]);
          }
        }
        int k0 = bx - span_x;
        for (int j = 0; j < 2; j++)
          for (int i = 0; i < 2; i++) {
            s64 f = (s64)(k0 + i) * span_recip;  // position in the span, 16.16
            for (u32 c = 0; c < texgens; c++)
              for (int a = 0; a < 2; a++) {
                s32 l = uv_l[j][c][a], r = uv_r[j][c][a];
                uv[i][j][c][a] = l + (s32)(((s64)(r - l) * f) >> 16);
              }
          }
        // LOD per TEV stage / indirect stage
        for (u32 st = 0; st <= s_num_stages; st++) {
          const StageConfig& cfg = s_stage[st];
          if (!cfg.tex_enable) continue;
          u32 tc = cfg.texcoord;
          ComputeLOD(cfg.texmap, uv[0][0][tc][0] - uv[1][0][tc][0], uv[0][0][tc][0] - uv[0][1][tc][0],
                     uv[0][0][tc][1] - uv[1][0][tc][1], uv[0][0][tc][1] - uv[0][1][tc][1], px.lod[st],
                     px.linear[st]);
        }
        for (u32 st = 0; st < s_num_ind; st++) {
          u32 map = Bits(s_iref, st * 6, 3), tc = Bits(s_iref, st * 6 + 3, 3);
          if (tc >= texgens) tc = 0;
          ComputeLOD(map, uv[0][0][tc][0] - uv[1][0][tc][0], uv[0][0][tc][0] - uv[0][1][tc][0],
                     uv[0][0][tc][1] - uv[1][0][tc][1], uv[0][0][tc][1] - uv[0][1][tc][1], px.ind_lod[st],
                     px.ind_linear[st]);
        }
      }

      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          if (!cover[i][j]) continue;
          int x = bx + i, y = by + j;
          px.x = x;
          px.y = y;
          s64 z = (t.z.c + t.z.dx * x + t.z.dy * y) >> 16;
          px.z = (u32)(z < 0 ? 0 : (z > 0xFFFFFF ? 0xFFFFFF : z));
          for (u32 c = 0; c < chans; c++)
            for (int k = 0; k < 4; k++) px.color[c][k] = PlaneU8(t.color[c][k], x, y);
          for (u32 c = 0; c < texgens; c++) {
            px.uv[c][0] = uv[i][j][c][0];
            px.uv[c][1] = uv[i][j][c][1];
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
  if (g_gpu) {
    // The backend decodes the TEV itself: only the textures are needed here.
    u32 tex_mask = 0;
    for (u32 stage = 0; stage <= s_num_stages; stage++) {
      u32 order = g_bp[BP_TREF + (stage >> 1)], shift = (stage & 1) ? 12 : 0;
      if (Bits(order, shift + 6, 1)) tex_mask |= 1u << Bits(order, shift, 3);
    }
    for (u32 st = 0; st < s_num_ind; st++) tex_mask |= 1u << Bits(s_iref, st * 6, 3);
    BindTextures(tex_mask);
    s_tex_mask = tex_mask;
    s_gpu_batch.clear();
    return;
  }
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
    c.c_a = (u8)Bits(c.cc, 12, 4), c.c_b = (u8)Bits(c.cc, 8, 4), c.c_c = (u8)Bits(c.cc, 4, 4), c.c_d = (u8)Bits(c.cc, 0, 4);
    c.c_bias = (u8)Bits(c.cc, 16, 2), c.c_op = (u8)Bits(c.cc, 18, 1), c.c_clamp = (u8)Bits(c.cc, 19, 1);
    c.c_scale = (u8)Bits(c.cc, 20, 2), c.c_dest = (u8)Bits(c.cc, 22, 2);
    c.a_a = (u8)Bits(c.ac, 13, 3), c.a_b = (u8)Bits(c.ac, 10, 3), c.a_c = (u8)Bits(c.ac, 7, 3), c.a_d = (u8)Bits(c.ac, 4, 3);
    c.a_bias = (u8)Bits(c.ac, 16, 2), c.a_op = (u8)Bits(c.ac, 18, 1), c.a_clamp = (u8)Bits(c.ac, 19, 1);
    c.a_scale = (u8)Bits(c.ac, 20, 2), c.a_dest = (u8)Bits(c.ac, 22, 2);
    c.tex_swap = s_ksel_swap[Bits(c.ac, 2, 2)];
    c.ras_swap = s_ksel_swap[Bits(c.ac, 0, 2)];
    c.ind = g_bp[BP_IND_CMD + stage] & 0x1FFFFF;
    u32 ksel = g_bp[BP_TEV_KSEL + (stage >> 1)];
    u32 kshift = (stage & 1) ? 14 : 4;
    S16x4 kc = KonstColor(Bits(ksel, kshift, 5));
    S16x4 ka = KonstColor(Bits(ksel, kshift + 5, 5));
    c.konst = {kc.r, kc.g, kc.b, ka.a};
  }
  // Pixel pipeline state
  {
    PixelState& ps = s_ps;
    u32 zmode = g_bp[BP_ZMODE];
    ps.z_enable = zmode & 1;
    ps.z_write = (zmode >> 4) & 1;
    ps.z_func = Bits(zmode, 1, 3);
    ps.early_z = Bits(g_bp[BP_ZCOMPARE], 6, 1) && !(g_bp[BP_ZTEX2] & 0xC);
    for (int a = 0; a < 256; a++) ps.alpha_pass[a] = AlphaTest(a);
    u32 last = s_num_stages;
    ps.last_cdest = Bits(g_bp[BP_TEV_COLOR_ENV + last * 2], 22, 2);
    ps.last_adest = Bits(g_bp[BP_TEV_COLOR_ENV + last * 2 + 1], 22, 2);
    u32 ztex2 = g_bp[BP_ZTEX2];
    ps.ztex_op = (ztex2 & 0xC) ? Bits(ztex2, 2, 2) : 0;
    ps.ztex_type = Bits(ztex2, 0, 2);
    ps.ztex_bias = g_bp[BP_ZTEX1] & 0xFFFFFF;
    u32 fog3 = g_bp[BP_FOG3];
    ps.fog_sel = Bits(fog3, 21, 3);
    ps.fog_ortho = Bits(fog3, 20, 1);
    ps.fog_a = FogFloat(g_bp[BP_FOG0]);
    ps.fog_c = FogFloat(fog3);
    ps.fog_b_mag = (s32)(g_bp[BP_FOG_B_MAG] & 0xFFFFFF);
    ps.fog_b_exp = g_bp[BP_FOG_B_EXP] & 0x1F;
    u32 fc = g_bp[BP_FOG_COLOR];
    ps.fog_r = (u8)(fc >> 16), ps.fog_g = (u8)(fc >> 8), ps.fog_b = (u8)fc;
  }
  PrepareBlend();
  u32 tex_mask = 0;
  for (u32 stage = 0; stage <= s_num_stages; stage++)
    if (s_stage[stage].tex_enable) tex_mask |= 1u << s_stage[stage].texmap;
  for (u32 st = 0; st < s_num_ind; st++) tex_mask |= 1u << Bits(s_iref, st * 6, 3);
  BindTextures(tex_mask);
  s_tex_mask = tex_mask;
  s_gpu_batch.clear();
  s_batch.clear();
  s_batch_area = 0;
}

void EndDraw() {
  if (g_gpu) {
    if (s_gpu_batch.empty()) return;
    static GpuDrawState st;
    memcpy(st.bp, g_bp, sizeof(st.bp));
    for (int i = 0; i < 4; i++) {
      const S16x4& r = g_tev_color_regs[i];
      const S16x4& k = g_tev_konst_regs[i];
      st.tev_regs[i][0] = r.r, st.tev_regs[i][1] = r.g, st.tev_regs[i][2] = r.b, st.tev_regs[i][3] = r.a;
      st.tev_konst[i][0] = k.r, st.tev_konst[i][1] = k.g, st.tev_konst[i][2] = k.b, st.tev_konst[i][3] = k.a;
    }
    st.tex_mask = s_tex_mask;
    for (u32 t = 0; t < 8; t++) {
      if (s_tex_mask & (1u << t))
        GetGpuTexture(t, st.tex[t]);
      else
        memset(&st.tex[t], 0, sizeof(st.tex[t]));
    }
    st.sc_left = s_sc_left, st.sc_top = s_sc_top, st.sc_right = s_sc_right, st.sc_bottom = s_sc_bottom;
    g_stats.pixels += (u32)(s_gpu_batch.size() / 3);
    g_gpu->Draw(st, s_gpu_batch.data(), (u32)s_gpu_batch.size());
    MarkEFBGpuDirty();
    s_gpu_batch.clear();
    return;
  }
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

  if (g_gpu) {
    const OutputVertex* vs[3] = {v0, v1, v2};
    for (int i = 0; i < 3; i++) {
      GpuVertex g;
      g.x = xs[i];
      g.y = ys[i];
      g.z = std::clamp(vs[i]->screen.z * (1.0f / 16777215.0f), 0.0f, 1.0f);
      g.w = vs[i]->proj[3];
      for (int c = 0; c < 2; c++)
        for (int k = 0; k < 4; k++) g.color[c][k] = vs[i]->color[c][k] * (1.0f / 255.0f);
      for (u32 n = 0; n < 8; n++) {
        g.tex[n][0] = vs[i]->texcoords[n].x;
        g.tex[n][1] = vs[i]->texcoords[n].y;
        g.tex[n][2] = vs[i]->texcoords[n].z;
      }
      s_gpu_batch.push_back(g);
    }
    return;
  }

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

  t.z = MakeFixedPlane(MakeSlope(v0->screen.z, v1->screen.z, v2->screen.z, ctx));
  float w[3] = {1.0f / v0->proj[3], 1.0f / v1->proj[3], 1.0f / v2->proj[3]};
  t.w = MakeSlope(w[0], w[1], w[2], ctx);
  for (u32 c = 0; c < 2; c++)
    for (int i = 0; i < 4; i++)
      t.color[c][i] = MakeFixedPlane(MakeSlope(v0->color[c][i], v1->color[c][i], v2->color[c][i], ctx));
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
