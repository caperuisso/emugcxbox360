// emugcxbox360 - translation of GX draws into Xenos shaders and render state
// SPDX-License-Identifier: GPL-2.0-or-later
//
// TEV stages become a pixel shader working on floats in 0..1 (GX 8-bit values
// / 255). The arithmetic follows the software TEV (raster.cpp) closely but not
// bit-exactly: the 8-bit wrap of the a/b/c operands is a saturate, the
// rounding constants of the hardware lerp are ignored, and unclamped results
// are not limited to 11 bits.
//
// Pixel shader interpolators: r0 = color 0, r1 = color 1, r2+n = texcoord n
// (s, t, q in texels), then (z * w, w) when fog needs the depth.
#include "xenos/gx_draw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include "core/video/video_internal.h"

namespace XenosGx {

using namespace Xenos;
using Video::Bits;

namespace {

// ---- Pixel shader constants ----
enum : u32 {
  C_TEV_REGS = 0,    // 4: TEV color registers
  C_KONST = 4,       // 16: konst color of each stage
  C_TEX_SCALE = 20,  // 8: (1/width, 1/height) per texture unit
  C_MISC = 28,       // (0, 1, 0.5, 255)
  C_ALPHA_REF = 29,  // (ref0, ref1, 0, 0) in 0..255
  C_PACK24 = 30,     // (1, 256, 65536, 0)
  C_PACK16 = 31,     // (1, 256, 0, 0)
  C_PACK8 = 32,      // (1, 0, 0, 0)
  C_SCALE = 33,      // (1, 2, 4, 0.5)
  C_FOG = 34,        // perspective: (A * 16777215, b_magnitude, 16777215 / 2^b_shift, C); ortho: (A, -, -, C)
  C_FOG_COLOR = 35,  // (r, g, b, 0)
  C_FOG_EXP = 36,    // (-8, 0, 0, 0)
};
static_assert(C_FOG_EXP < NUM_PS_CONSTS, "constant layout");

const u8 kKonstFixed[8] = {255, 223, 191, 159, 128, 96, 64, 32};

void KonstColor(const Video::GpuDrawState& st, u32 sel, float out[4]) {
  if (sel < 8) {
    float v = kKonstFixed[sel] / 255.0f;
    out[0] = out[1] = out[2] = out[3] = v;
  } else if (sel >= 12 && sel <= 15) {
    for (int i = 0; i < 4; i++) out[i] = st.tev_konst[sel - 12][i] / 255.0f;
  } else if (sel >= 16) {
    float v = st.tev_konst[(sel - 16) & 3][(sel - 16) >> 2] / 255.0f;
    out[0] = out[1] = out[2] = out[3] = v;
  } else {
    out[0] = out[1] = out[2] = out[3] = 0;
  }
}

float FogFloat(u32 v) {
  u32 mant = Bits(v, 0, 11), exp = Bits(v, 11, 8), sign = Bits(v, 19, 1);
  u32 bits = (sign << 31) | (exp << 23) | (mant << 12);
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

struct StageInfo {
  u32 texmap, texcoord, ras_chan, cc, ac;
  bool tex_enable;
  u32 tex_swap[4], ras_swap[4];
};

struct DrawInfo {
  u32 num_stages;  // number of TEV stages (1..16)
  u32 num_texgens, num_chans;
  StageInfo stage[16];
  u32 alpha_cmp;   // BP alpha compare without the reference values
  u32 fog_sel;
  bool fog_ortho;
  bool fog;
};

void Decode(const Video::GpuDrawState& st, DrawInfo& d) {
  const u32* bp = st.bp;
  u32 genmode = bp[Video::BP_GENMODE];
  d.num_texgens = Bits(genmode, 0, 4);
  d.num_chans = Bits(genmode, 4, 3);
  d.num_stages = Bits(genmode, 10, 4) + 1;
  u32 swap[4][4];
  for (int t = 0; t < 4; t++) {
    u32 k0 = bp[Video::BP_TEV_KSEL + t * 2], k1 = bp[Video::BP_TEV_KSEL + t * 2 + 1];
    swap[t][0] = Bits(k0, 0, 2);
    swap[t][1] = Bits(k0, 2, 2);
    swap[t][2] = Bits(k1, 0, 2);
    swap[t][3] = Bits(k1, 2, 2);
  }
  for (u32 s = 0; s < d.num_stages; s++) {
    StageInfo& c = d.stage[s];
    u32 order = bp[Video::BP_TREF + (s >> 1)];
    u32 shift = (s & 1) ? 12 : 0;
    c.texmap = Bits(order, shift, 3);
    c.texcoord = Bits(order, shift + 3, 3);
    c.tex_enable = Bits(order, shift + 6, 1);
    c.ras_chan = Bits(order, shift + 7, 3);
    if (c.texcoord >= d.num_texgens) c.texcoord = 0;
    c.cc = bp[Video::BP_TEV_COLOR_ENV + s * 2] & 0xFFFFFF;
    c.ac = bp[Video::BP_TEV_COLOR_ENV + s * 2 + 1] & 0xFFFFFF;
    memcpy(c.tex_swap, swap[Bits(c.ac, 2, 2)], sizeof(c.tex_swap));
    memcpy(c.ras_swap, swap[Bits(c.ac, 0, 2)], sizeof(c.ras_swap));
  }
  d.alpha_cmp = bp[Video::BP_ALPHA_COMPARE] & 0xFF0000;
  u32 fog3 = bp[Video::BP_FOG3];
  d.fog_sel = Bits(fog3, 21, 3);
  d.fog_ortho = Bits(fog3, 20, 1);
  d.fog = d.fog_sel != 0;
}

// ---- Shader caches ----
std::unordered_map<std::string, std::unique_ptr<ShaderEntry>>* s_ps_cache = nullptr;
std::unordered_map<std::string, std::unique_ptr<ShaderEntry>>* s_vs_cache = nullptr;

std::string KeyOf(const std::vector<u32>& words) {
  return std::string((const char*)words.data(), words.size() * 4);
}

// ---- Vertex shader: passes position, 2 colors, texcoords and fog depth ----
ShaderEntry* GetVertexShader(u32 texgens, bool fog) {
  if (!s_vs_cache) s_vs_cache = new std::unordered_map<std::string, std::unique_ptr<ShaderEntry>>();
  std::string key = KeyOf({texgens, fog ? 1u : 0u});
  auto it = s_vs_cache->find(key);
  if (it != s_vs_cache->end()) return it->second.get();

  u32 stride = 12 + texgens * 3 + (fog ? 2 : 0);
  ShaderBuilder b(false);
  b.VFetch(1, 0, 95, VFMT_32_32_32_32_FLOAT, 0, stride);
  b.VFetch(2, 0, 95, VFMT_32_32_32_32_FLOAT, 4, stride);
  b.VFetch(3, 0, 95, VFMT_32_32_32_32_FLOAT, 8, stride);
  for (u32 n = 0; n < texgens; n++)
    b.VFetch(4 + n, 0, 95, VFMT_32_32_32_FLOAT, 12 + n * 3, stride, FetchSwz(0, 1, 2, 5));
  if (fog) b.VFetch(4 + texgens, 0, 95, VFMT_32_32_FLOAT, 12 + texgens * 3, stride, FETCH_XY01);
  b.UseTemps(4 + texgens + (fog ? 1 : 0));
  b.BeginPhase(PHASE_POSITION);
  b.MovExport(EXPORT_POSITION, 0xF, Src::R(1));
  b.BeginPhase(PHASE_INTERP);
  u32 interps = 2 + texgens + (fog ? 1 : 0);
  for (u32 i = 0; i < interps; i++) b.MovExport(i, 0xF, Src::R(2 + i));
  auto e = std::make_unique<ShaderEntry>();
  e->shader = {b.Build(), VertexProgramControl(b.TempCount(), interps), 0};
  ShaderEntry* p = e.get();
  (*s_vs_cache)[key] = std::move(e);
  return p;
}

// ---- Pixel shader: the TEV ----
struct PsRegs {
  u32 interps;
  u32 reg[4], tex, ras, a, b, c, d, x, y, out;
};

Src ColorArg(const PsRegs& r, u32 stage, u32 arg) {
  switch (arg) {
    case 0: case 2: case 4: case 6: return Src::R(r.reg[arg / 2]);
    case 1: case 3: case 5: case 7: return Src::R(r.reg[arg / 2], SWZ_WWWW);
    case 8: return Src::R(r.tex);
    case 9: return Src::R(r.tex, SWZ_WWWW);
    case 10: return Src::R(r.ras);
    case 11: return Src::R(r.ras, SWZ_WWWW);
    case 12: return Src::C(C_MISC, SWZ_YYYY);  // 1
    case 13: return Src::C(C_MISC, SWZ_ZZZZ);  // 0.5
    case 14: return Src::C(C_KONST + stage);
    default: return Src::C(C_MISC, SWZ_XXXX);  // 0
  }
}

Src AlphaArg(const PsRegs& r, u32 stage, u32 arg) {
  switch (arg) {
    case 0: case 1: case 2: case 3: return Src::R(r.reg[arg], SWZ_WWWW);
    case 4: return Src::R(r.tex, SWZ_WWWW);
    case 5: return Src::R(r.ras, SWZ_WWWW);
    case 6: return Src::C(C_KONST + stage, SWZ_WWWW);
    default: return Src::C(C_MISC, SWZ_XXXX);
  }
}

// dst.rgb = color operand, dst.a = alpha operand
void LoadOperand(ShaderBuilder& b, u32 dst, const Src& color, const Src& alpha, bool clamp) {
  if (color.is_temp == alpha.is_temp && color.reg == alpha.reg && color.swizzle == SWZ_XYZW &&
      alpha.swizzle == SWZ_WWWW) {
    b.Mov(dst, 0xF, color, clamp);
    return;
  }
  b.Mov(dst, 0x7, color, clamp);
  b.Mov(dst, 0x8, alpha, clamp);
}

// One combiner group (color: mask 0x7, alpha: mask 0x8) in lerp mode.
void Lerp(ShaderBuilder& b, const PsRegs& r, u32 mask, u32 bias, u32 op, u32 scale, u32 dest, bool clamp) {
  // y = d +- lerp
  Src lerp = Src::R(r.x);
  b.Vec(V_ADD, r.y, mask, Src::R(r.d), op ? lerp.Neg() : lerp);
  if (bias == 1) b.Vec(V_ADD, r.y, mask, Src::R(r.y), Src::C(C_MISC, SWZ_ZZZZ));
  if (bias == 2) b.Vec(V_ADD, r.y, mask, Src::R(r.y), Src::C(C_MISC, SWZ_ZZZZ).Neg());
  static const u32 kScaleSwz[4] = {SWZ_XXXX, SWZ_YYYY, SWZ_ZZZZ, SWZ_WWWW};  // 1, 2, 4, 0.5
  if (scale == 0)
    b.Mov(r.reg[dest], mask, Src::R(r.y), clamp);
  else
    b.Vec(V_MUL, r.reg[dest], mask, Src::R(r.y), Src::C(C_SCALE, kScaleSwz[scale]), Src(), clamp);
}

// One combiner group in compare mode: dest = d + (a OP b ? c : 0).
void Compare(ShaderBuilder& b, const PsRegs& r, u32 mask, bool alpha_group, u32 op, u32 scale, u32 dest,
             bool clamp) {
  bool per_component = scale == 3;
  if (per_component) {
    b.Vec(op ? V_SEQ : V_SGT, r.y, mask, Src::R(r.a), Src::R(r.b));
  } else {
    // Packed compare of the color operands (R8, GR16 or BGR24), on values
    // quantized back to 8 bits.
    u32 pack = scale == 0 ? C_PACK8 : (scale == 1 ? C_PACK16 : C_PACK24);
    b.Vec(V_MAD, r.y, 0x7, Src::R(r.a), Src::C(C_MISC, SWZ_WWWW), Src::C(C_MISC, SWZ_ZZZZ));
    b.Vec(V_FLOOR, r.y, 0x7, Src::R(r.y));
    b.Vec(V_DP3, r.y, 0x1, Src::R(r.y), Src::C(pack));
    b.Vec(V_MAD, r.out, 0x7, Src::R(r.b), Src::C(C_MISC, SWZ_WWWW), Src::C(C_MISC, SWZ_ZZZZ));
    b.Vec(V_FLOOR, r.out, 0x7, Src::R(r.out));
    b.Vec(V_DP3, r.out, 0x1, Src::R(r.out), Src::C(pack));
    b.Vec(op ? V_SEQ : V_SGT, r.y, mask, Src::R(r.y, SWZ_XXXX), Src::R(r.out, SWZ_XXXX));
  }
  (void)alpha_group;
  b.Vec(V_MAD, r.reg[dest], mask, Src::R(r.y), Src::R(r.c), Src::R(r.d), clamp);
}

// Alpha test comparison of the quantized alpha (r.y.x) with a reference
// (C_ALPHA_REF component `ref`), result 0/1 in r.c component `out_comp`.
void AlphaCompare(ShaderBuilder& b, const PsRegs& r, u32 func, u32 ref, u32 out_mask) {
  Src a = Src::R(r.y, SWZ_XXXX);
  Src rf = Src::C(C_ALPHA_REF, ref == 0 ? SWZ_XXXX : SWZ_YYYY);
  switch (func) {
    case 0: b.Mov(r.c, out_mask, Src::C(C_MISC, SWZ_XXXX)); break;  // never
    case 1: b.Vec(V_SGT, r.c, out_mask, rf, a); break;             // a < ref
    case 2: b.Vec(V_SEQ, r.c, out_mask, a, rf); break;
    case 3: b.Vec(V_SGE, r.c, out_mask, rf, a); break;             // a <= ref
    case 4: b.Vec(V_SGT, r.c, out_mask, a, rf); break;
    case 5: b.Vec(V_SNE, r.c, out_mask, a, rf); break;
    case 6: b.Vec(V_SGE, r.c, out_mask, a, rf); break;
    default: b.Mov(r.c, out_mask, Src::C(C_MISC, SWZ_YYYY)); break;  // always
  }
}

// Constant result of the alpha test when both comparisons are trivial:
// 1 = always passes, 0 = always fails, -1 = depends on alpha.
int AlphaTestFold(u32 cmp) {
  u32 f0 = Bits(cmp, 16, 3), f1 = Bits(cmp, 19, 3), logic = Bits(cmp, 22, 2);
  auto trivial = [](u32 f) { return f == 0 ? 0 : (f == 7 ? 1 : -1); };
  int t0 = trivial(f0), t1 = trivial(f1);
  switch (logic) {
    case 0:  // and
      if (t0 == 0 || t1 == 0) return 0;
      if (t0 == 1 && t1 == 1) return 1;
      return -1;
    case 1:  // or
      if (t0 == 1 || t1 == 1) return 1;
      if (t0 == 0 && t1 == 0) return 0;
      return -1;
    default:
      if (t0 >= 0 && t1 >= 0) return logic == 2 ? (t0 != t1) : (t0 == t1);
      return -1;
  }
}

ShaderEntry* GetPixelShader(const DrawInfo& d) {
  if (!s_ps_cache) s_ps_cache = new std::unordered_map<std::string, std::unique_ptr<ShaderEntry>>();
  std::vector<u32> keyw;
  keyw.push_back(d.num_stages | (d.num_texgens << 8) | (d.num_chans << 12) | (d.fog_sel << 16) |
                 ((d.fog_ortho ? 1u : 0u) << 20));
  keyw.push_back(d.alpha_cmp);
  for (u32 s = 0; s < d.num_stages; s++) {
    const StageInfo& c = d.stage[s];
    keyw.push_back(c.cc);
    keyw.push_back(c.ac);
    keyw.push_back(c.texmap | (c.texcoord << 3) | ((c.tex_enable ? 1u : 0u) << 6) | (c.ras_chan << 7) |
                   (c.tex_swap[0] << 10) | (c.tex_swap[1] << 12) | (c.tex_swap[2] << 14) | (c.tex_swap[3] << 16) |
                   (c.ras_swap[0] << 18) | (c.ras_swap[1] << 20) | (c.ras_swap[2] << 22) | (c.ras_swap[3] << 24));
  }
  std::string key = KeyOf(keyw);
  auto it = s_ps_cache->find(key);
  if (it != s_ps_cache->end()) return it->second.get();

  PsRegs r;
  r.interps = 2 + d.num_texgens + (d.fog ? 1 : 0);
  u32 t = r.interps;
  for (int i = 0; i < 4; i++) r.reg[i] = t++;
  r.tex = t++, r.ras = t++, r.a = t++, r.b = t++, r.c = t++, r.d = t++, r.x = t++, r.y = t++, r.out = t++;

  ShaderBuilder b(true);
  b.UseTemps(t);
  for (int i = 0; i < 4; i++) b.Mov(r.reg[i], 0xF, Src::C(C_TEV_REGS + i));

  for (u32 s = 0; s < d.num_stages; s++) {
    const StageInfo& c = d.stage[s];
    // Texture
    if (c.tex_enable && d.num_texgens > 0) {
      u32 tc = 2 + c.texcoord;
      b.Scalar(S_RCP, r.x, 0x1, Src::R(tc, SWZ_ZZZZ));                        // 1 / q
      b.Vec(V_MUL, r.y, 0x3, Src::R(tc), Src::R(r.x, SWZ_XXXX));              // s/q, t/q (texels)
      b.Vec(V_MUL, r.y, 0x3, Src::R(r.y), Src::C(C_TEX_SCALE + c.texmap));    // normalized
      b.TFetch2D(r.tex, r.y, c.texmap);
      u32 sw = Swz(c.tex_swap[0], c.tex_swap[1], c.tex_swap[2], c.tex_swap[3]);
      if (sw != SWZ_XYZW) b.Mov(r.tex, 0xF, Src::R(r.tex, sw));
    } else {
      b.Mov(r.tex, 0xF, Src::C(C_MISC, SWZ_XXXX));
    }
    // Rasterized color
    if (c.ras_chan <= 1)
      b.Mov(r.ras, 0xF, Src::R(c.ras_chan, Swz(c.ras_swap[0], c.ras_swap[1], c.ras_swap[2], c.ras_swap[3])));
    else
      b.Mov(r.ras, 0xF, Src::C(C_MISC, SWZ_XXXX));  // alpha bump (indirect texturing) not supported yet

    u32 c_a = Bits(c.cc, 12, 4), c_b = Bits(c.cc, 8, 4), c_c = Bits(c.cc, 4, 4), c_d = Bits(c.cc, 0, 4);
    u32 a_a = Bits(c.ac, 13, 3), a_b = Bits(c.ac, 10, 3), a_c = Bits(c.ac, 7, 3), a_d = Bits(c.ac, 4, 3);
    LoadOperand(b, r.a, ColorArg(r, s, c_a), AlphaArg(r, s, a_a), true);
    LoadOperand(b, r.b, ColorArg(r, s, c_b), AlphaArg(r, s, a_b), true);
    LoadOperand(b, r.c, ColorArg(r, s, c_c), AlphaArg(r, s, a_c), true);
    LoadOperand(b, r.d, ColorArg(r, s, c_d), AlphaArg(r, s, a_d), false);

    u32 cbias = Bits(c.cc, 16, 2), cop = Bits(c.cc, 18, 1), cclamp = Bits(c.cc, 19, 1), cscale = Bits(c.cc, 20, 2),
        cdest = Bits(c.cc, 22, 2);
    u32 abias = Bits(c.ac, 16, 2), aop = Bits(c.ac, 18, 1), aclamp = Bits(c.ac, 19, 1), ascale = Bits(c.ac, 20, 2),
        adest = Bits(c.ac, 22, 2);
    if (cbias != 3 || abias != 3) {
      // x = a + (b - a) * c
      b.Vec(V_ADD, r.x, 0xF, Src::R(r.b), Src::R(r.a).Neg());
      b.Vec(V_MAD, r.x, 0xF, Src::R(r.x), Src::R(r.c), Src::R(r.a));
    }
    if (cbias != 3)
      Lerp(b, r, 0x7, cbias, cop, cscale, cdest, cclamp);
    else
      Compare(b, r, 0x7, false, cop, cscale, cdest, cclamp);
    if (abias != 3)
      Lerp(b, r, 0x8, abias, aop, ascale, adest, aclamp);
    else
      Compare(b, r, 0x8, true, aop, ascale, adest, aclamp);
  }

  // Result of the last stage
  const StageInfo& last = d.stage[d.num_stages - 1];
  u32 cdest = Bits(last.cc, 22, 2), adest = Bits(last.ac, 22, 2);
  b.Mov(r.out, 0x7, Src::R(r.reg[cdest]), true);
  b.Mov(r.out, 0x8, Src::R(r.reg[adest]), true);

  // Alpha test
  int fold = AlphaTestFold(d.alpha_cmp);
  if (fold != 1) {
    if (fold == 0) {
      b.Vec(V_KILLEQ, r.x, 0x1, Src::C(C_MISC, SWZ_XXXX), Src::C(C_MISC, SWZ_XXXX));
    } else {
      // y.x = round(alpha * 255)
      b.Vec(V_MAD, r.y, 0x1, Src::R(r.out, SWZ_WWWW), Src::C(C_MISC, SWZ_WWWW), Src::C(C_MISC, SWZ_ZZZZ));
      b.Vec(V_FLOOR, r.y, 0x1, Src::R(r.y));
      AlphaCompare(b, r, Bits(d.alpha_cmp, 16, 3), 0, 0x1);
      AlphaCompare(b, r, Bits(d.alpha_cmp, 19, 3), 1, 0x2);
      switch (Bits(d.alpha_cmp, 22, 2)) {
        case 0: b.Vec(V_MUL, r.c, 0x4, Src::R(r.c, SWZ_XXXX), Src::R(r.c, SWZ_YYYY)); break;
        case 1: b.Vec(V_MAX, r.c, 0x4, Src::R(r.c, SWZ_XXXX), Src::R(r.c, SWZ_YYYY)); break;
        case 2: b.Vec(V_SNE, r.c, 0x4, Src::R(r.c, SWZ_XXXX), Src::R(r.c, SWZ_YYYY)); break;
        default: b.Vec(V_SEQ, r.c, 0x4, Src::R(r.c, SWZ_XXXX), Src::R(r.c, SWZ_YYYY)); break;
      }
      b.Vec(V_KILLEQ, r.x, 0x1, Src::R(r.c, SWZ_ZZZZ), Src::C(C_MISC, SWZ_XXXX));
    }
  }

  // Fog
  if (d.fog) {
    u32 fz = 2 + d.num_texgens;
    b.Scalar(S_RCP, r.x, 0x1, Src::R(fz, SWZ_YYYY));                    // 1 / w
    b.Vec(V_MUL, r.y, 0x1, Src::R(fz, SWZ_XXXX), Src::R(r.x, SWZ_XXXX));  // z (screen, 0..1)
    if (!d.fog_ortho) {
      // ze = A' / (b_mag - z * K) - C
      b.Vec(V_MAD, r.y, 0x1, Src::R(r.y, SWZ_XXXX), Src::C(C_FOG, SWZ_ZZZZ).Neg(), Src::C(C_FOG, SWZ_YYYY));
      b.Scalar(S_RCP, r.x, 0x1, Src::R(r.y, SWZ_XXXX));
      b.Vec(V_MAD, r.y, 0x1, Src::R(r.x, SWZ_XXXX), Src::C(C_FOG, SWZ_XXXX), Src::C(C_FOG, SWZ_WWWW).Neg(), true);
    } else {
      b.Vec(V_MAD, r.y, 0x1, Src::R(r.y, SWZ_XXXX), Src::C(C_FOG, SWZ_XXXX), Src::C(C_FOG, SWZ_WWWW).Neg(), true);
    }
    // y.x = fog factor (saturated); exponential variants
    switch (d.fog_sel) {
      case 4: case 5: case 6: case 7: {
        bool inv = d.fog_sel >= 6;  // 2^(-8 (1 - f)) forms
        if (inv) b.Vec(V_ADD, r.y, 0x1, Src::C(C_MISC, SWZ_YYYY), Src::R(r.y, SWZ_XXXX).Neg());
        if (d.fog_sel == 5 || d.fog_sel == 7) b.Vec(V_MUL, r.y, 0x1, Src::R(r.y, SWZ_XXXX), Src::R(r.y, SWZ_XXXX));
        b.Vec(V_MUL, r.y, 0x1, Src::R(r.y, SWZ_XXXX), Src::C(C_FOG_EXP, SWZ_XXXX));
        b.Scalar(S_EXP, r.y, 0x1, Src::R(r.y, SWZ_XXXX));
        if (!inv) b.Vec(V_ADD, r.y, 0x1, Src::C(C_MISC, SWZ_YYYY), Src::R(r.y, SWZ_XXXX).Neg());
        break;
      }
      default: break;
    }
    // out.rgb += (fog_color - out.rgb) * f
    b.Vec(V_ADD, r.x, 0x7, Src::C(C_FOG_COLOR), Src::R(r.out).Neg());
    b.Vec(V_MAD, r.out, 0x7, Src::R(r.x), Src::R(r.y, SWZ_XXXX), Src::R(r.out));
  }

  b.BeginPhase(PHASE_INTERP);
  b.MovExport(EXPORT_COLOR0, 0xF, Src::R(r.out), true);

  auto e = std::make_unique<ShaderEntry>();
  e->shader = {b.Build(), PixelProgramControl(b.TempCount()), 4};
  ShaderEntry* p = e.get();
  (*s_ps_cache)[key] = std::move(e);
  return p;
}

// ---- Render state ----
u32 BlendFactor(u32 f, bool is_src, bool dst_alpha) {
  switch (f) {
    case 0: return BLEND_ZERO;
    case 1: return BLEND_ONE;
    case 2: return is_src ? BLEND_DESTCOLOR : BLEND_SRCCOLOR;
    case 3: return is_src ? BLEND_INVDESTCOLOR : BLEND_INVSRCCOLOR;
    case 4: return BLEND_SRCALPHA;
    case 5: return BLEND_INVSRCALPHA;
    case 6: return dst_alpha ? BLEND_DESTALPHA : BLEND_ONE;
    default: return dst_alpha ? BLEND_INVDESTALPHA : BLEND_ZERO;
  }
}
// Color factors used on the alpha channel
u32 AlphaFactor(u32 f) {
  switch (f) {
    case BLEND_SRCCOLOR: return BLEND_SRCALPHA;
    case BLEND_INVSRCCOLOR: return BLEND_INVSRCALPHA;
    case BLEND_DESTCOLOR: return BLEND_DESTALPHA;
    case BLEND_INVDESTCOLOR: return BLEND_INVDESTALPHA;
    default: return f;
  }
}

void MakeRenderState(const Video::GpuDrawState& st, RenderState& rs) {
  const u32* bp = st.bp;
  u32 zmode = bp[Video::BP_ZMODE];
  rs.z_enable = zmode & 1;
  rs.z_func = Bits(zmode, 1, 3);
  rs.z_write = rs.z_enable && Bits(zmode, 4, 1);
  u32 blend = bp[Video::BP_BLENDMODE];
  bool dst_alpha = Bits(bp[Video::BP_ZCOMPARE], 0, 3) == 1;
  u32 src = BLEND_ONE, dst = BLEND_ZERO, op = BLENDOP_ADD;
  if (Bits(blend, 11, 1)) {  // subtract: dst - src
    src = BLEND_ONE, dst = BLEND_ONE, op = BLENDOP_REVSUBTRACT;
  } else if (Bits(blend, 0, 1)) {
    src = BlendFactor(Bits(blend, 8, 3), true, dst_alpha);
    dst = BlendFactor(Bits(blend, 5, 3), false, dst_alpha);
  } else if (Bits(blend, 1, 1)) {
    switch (Bits(blend, 12, 4)) {
      case 0: src = BLEND_ZERO, dst = BLEND_ZERO; break;  // clear
      case 5: src = BLEND_ZERO, dst = BLEND_ONE; break;   // noop
      default: break;                                     // copy (other ops approximated)
    }
  }
  rs.color_src = src, rs.color_dst = dst, rs.color_op = op;
  rs.alpha_src = AlphaFactor(src), rs.alpha_dst = AlphaFactor(dst), rs.alpha_op = op;
  if (!Bits(blend, 3, 1)) rs.color_src = BLEND_ZERO, rs.color_dst = BLEND_ONE, rs.color_op = BLENDOP_ADD;
  if (!Bits(blend, 4, 1)) rs.alpha_src = BLEND_ZERO, rs.alpha_dst = BLEND_ONE, rs.alpha_op = BLENDOP_ADD;
  rs.sc_left = st.sc_left, rs.sc_top = st.sc_top, rs.sc_right = st.sc_right, rs.sc_bottom = st.sc_bottom;
}

inline void PutPosition(float* p, float x, float y, float z, float w) {
  float nx = x / (EFB_W * 0.5f) - 1.0f, ny = 1.0f - y / (EFB_H * 0.5f);
  p[0] = nx * w, p[1] = ny * w, p[2] = z * w, p[3] = w;
}

}  // namespace

void Prepare(const Video::GpuDrawState& st, const Video::GpuVertex* v, u32 count, PreparedDraw& out) {
  DrawInfo d;
  Decode(st, d);
  out.ps = GetPixelShader(d);
  out.vs = GetVertexShader(d.num_texgens, d.fog);
  MakeRenderState(st, out.rs);

  // Constants
  float (*c)[4] = out.ps_consts;
  memset(c, 0, sizeof(out.ps_consts));
  for (int i = 0; i < 4; i++)
    for (int k = 0; k < 4; k++) c[C_TEV_REGS + i][k] = st.tev_regs[i][k] / 255.0f;
  for (u32 s = 0; s < d.num_stages; s++) {
    u32 ksel = st.bp[Video::BP_TEV_KSEL + (s >> 1)];
    u32 kshift = (s & 1) ? 14 : 4;
    float kc[4], ka[4];
    KonstColor(st, Bits(ksel, kshift, 5), kc);
    KonstColor(st, Bits(ksel, kshift + 5, 5), ka);
    c[C_KONST + s][0] = kc[0], c[C_KONST + s][1] = kc[1], c[C_KONST + s][2] = kc[2], c[C_KONST + s][3] = ka[3];
  }
  out.tex_mask = 0;
  for (u32 s = 0; s < d.num_stages; s++)
    if (d.stage[s].tex_enable && d.num_texgens > 0) out.tex_mask |= 1u << d.stage[s].texmap;
  for (u32 t = 0; t < 8; t++) {
    const Video::GpuTexture& tx = st.tex[t];
    c[C_TEX_SCALE + t][0] = tx.width ? 1.0f / tx.width : 0.0f;
    c[C_TEX_SCALE + t][1] = tx.height ? 1.0f / tx.height : 0.0f;
  }
  c[C_MISC][0] = 0, c[C_MISC][1] = 1, c[C_MISC][2] = 0.5f, c[C_MISC][3] = 255;
  u32 ac = st.bp[Video::BP_ALPHA_COMPARE];
  c[C_ALPHA_REF][0] = (float)Bits(ac, 0, 8), c[C_ALPHA_REF][1] = (float)Bits(ac, 8, 8);
  c[C_PACK24][0] = 1, c[C_PACK24][1] = 256, c[C_PACK24][2] = 65536;
  c[C_PACK16][0] = 1, c[C_PACK16][1] = 256;
  c[C_PACK8][0] = 1;
  c[C_SCALE][0] = 1, c[C_SCALE][1] = 2, c[C_SCALE][2] = 4, c[C_SCALE][3] = 0.5f;
  if (d.fog) {
    float a = FogFloat(st.bp[Video::BP_FOG0]), fc = FogFloat(st.bp[Video::BP_FOG3]);
    u32 bexp = st.bp[Video::BP_FOG_B_EXP] & 0x1F;
    if (!d.fog_ortho) {
      c[C_FOG][0] = a * 16777215.0f;
      c[C_FOG][1] = (float)(st.bp[Video::BP_FOG_B_MAG] & 0xFFFFFF);
      c[C_FOG][2] = 16777215.0f / (float)(1u << bexp);
    } else {
      c[C_FOG][0] = a;
    }
    c[C_FOG][3] = fc;
    u32 col = st.bp[Video::BP_FOG_COLOR];
    c[C_FOG_COLOR][0] = ((col >> 16) & 0xFF) / 255.0f;
    c[C_FOG_COLOR][1] = ((col >> 8) & 0xFF) / 255.0f;
    c[C_FOG_COLOR][2] = (col & 0xFF) / 255.0f;
    c[C_FOG_EXP][0] = -8.0f;
  }

  // Vertices
  u32 stride = 12 + d.num_texgens * 3 + (d.fog ? 2 : 0);
  out.stride = stride;
  out.vertex_count = count;
  out.vertices.resize((size_t)stride * count);
  float* p = out.vertices.data();
  for (u32 i = 0; i < count; i++, p += stride) {
    const Video::GpuVertex& g = v[i];
    PutPosition(p, g.x, g.y, g.z, g.w);
    memcpy(p + 4, g.color[0], 4 * sizeof(float));
    memcpy(p + 8, g.color[1], 4 * sizeof(float));
    for (u32 n = 0; n < d.num_texgens; n++) memcpy(p + 12 + n * 3, g.tex[n], 3 * sizeof(float));
    if (d.fog) {
      p[12 + d.num_texgens * 3] = g.z * g.w;
      p[12 + d.num_texgens * 3 + 1] = g.w;
    }
  }
}

void PrepareClear(int x0, int y0, int x1, int y1, bool color, bool alpha, bool depth, u32 rgba, u32 z24,
                  PreparedDraw& out) {
  static ShaderEntry* ps = nullptr;
  if (!ps) {
    // oC0 = interpolator 0 (the vertex color)
    ShaderBuilder b(true);
    b.BeginPhase(PHASE_INTERP);
    b.MovExport(EXPORT_COLOR0, 0xF, Src::R(0));
    ps = new ShaderEntry();
    ps->shader = {b.Build(), PixelProgramControl(2), 4};  // registers r0-r1 hold the interpolators
  }
  out.ps = ps;
  out.vs = GetVertexShader(0, false);
  memset(out.ps_consts, 0, sizeof(out.ps_consts));
  out.tex_mask = 0;
  RenderState& rs = out.rs;
  rs.z_enable = depth;
  rs.z_write = depth;
  rs.z_func = 7;
  rs.color_src = color ? BLEND_ONE : BLEND_ZERO, rs.color_dst = color ? BLEND_ZERO : BLEND_ONE;
  rs.alpha_src = alpha ? BLEND_ONE : BLEND_ZERO, rs.alpha_dst = alpha ? BLEND_ZERO : BLEND_ONE;
  rs.color_op = rs.alpha_op = BLENDOP_ADD;
  rs.sc_left = x0, rs.sc_top = y0, rs.sc_right = x1, rs.sc_bottom = y1;
  float z = (z24 & 0xFFFFFF) / 16777215.0f;
  float col[4] = {((rgba >> 24) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
                  (rgba & 0xFF) / 255.0f};
  const float xs[6] = {(float)x0, (float)x1, (float)x0, (float)x1, (float)x1, (float)x0};
  const float ys[6] = {(float)y0, (float)y0, (float)y1, (float)y0, (float)y1, (float)y1};
  out.stride = 12;
  out.vertex_count = 6;
  out.vertices.assign(12 * 6, 0.0f);
  for (int i = 0; i < 6; i++) {
    float* p = &out.vertices[i * 12];
    PutPosition(p, xs[i], ys[i], z, 1.0f);
    memcpy(p + 4, col, sizeof(col));
  }
}

u32 ShaderCount() { return (s_ps_cache ? (u32)s_ps_cache->size() : 0) + (s_vs_cache ? (u32)s_vs_cache->size() : 0); }

}  // namespace XenosGx
