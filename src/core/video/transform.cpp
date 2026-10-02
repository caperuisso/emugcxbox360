// SPDX-License-Identifier: GPL-2.0-or-later
// XF transform unit: position/normal transform, lighting, texture coordinate generation.
#include <algorithm>
#include <cmath>

#include "core/video/video_internal.h"

namespace Video {

namespace {

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 Normalized(const Vec3& v) {
  float len2 = Dot(v, v);
  if (len2 <= 0.0f) return v;
  return Scale(v, 1.0f / std::sqrt(len2));
}

inline Vec3 MulMat34(const float* m, const Vec3& v) {
  return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3], m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
          m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]};
}
inline Vec3 MulMat33(const float* m, const Vec3& v) {
  return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
          m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

// Per-light data in XF memory (16 words per light)
struct Light {
  u8 color[4];  // RGBA
  Vec3 cosatt, distatt, pos, dir;
};

Light GetLight(u32 n) {
  u32 base = XF_LIGHTS + n * 0x10;
  Light l;
  u32 c = g_xf[base + 3];
  l.color[0] = (u8)(c >> 24);
  l.color[1] = (u8)(c >> 16);
  l.color[2] = (u8)(c >> 8);
  l.color[3] = (u8)c;
  const float* f = XFFloats(base + 4);
  l.cosatt = {f[0], f[1], f[2]};
  l.distatt = {f[3], f[4], f[5]};
  l.pos = {f[6], f[7], f[8]};
  l.dir = {f[9], f[10], f[11]};
  return l;
}

inline float SafeDivide(float n, float d) { return d == 0.0f ? (n > 0.0f ? 1.0f : 0.0f) : n / d; }

// Returns the light attenuation and leaves the normalised light direction in ldir.
float Attenuation(const Light& l, Vec3& ldir, const Vec3& normal, u32 chan) {
  u32 attn_func = Bits(chan, 9, 2);
  u32 diffuse_func = Bits(chan, 7, 2);
  switch (attn_func) {
    case 1: {  // specular
      ldir = Normalized(ldir);
      float attn = Dot(ldir, normal) >= 0.0f ? std::max(0.0f, Dot(l.dir, normal)) : 0.0f;
      Vec3 att_len = {1.0f, attn, attn * attn};
      Vec3 dist = diffuse_func ? Normalized(l.distatt) : l.distatt;
      return SafeDivide(std::max(0.0f, Dot(att_len, l.cosatt)), Dot(att_len, dist));
    }
    case 3: {  // spot
      float dist2 = Dot(ldir, ldir);
      float dist = std::sqrt(dist2);
      ldir = dist > 0.0f ? Scale(ldir, 1.0f / dist) : ldir;
      float attn = std::max(0.0f, Dot(ldir, l.dir));
      float cos_att = l.cosatt.x + l.cosatt.y * attn + l.cosatt.z * attn * attn;
      float dist_att = l.distatt.x + l.distatt.y * dist + l.distatt.z * dist2;
      return SafeDivide(std::max(0.0f, cos_att), dist_att);
    }
    default: {  // none / directional
      ldir = Normalized(ldir);
      if (ldir.x == 0.0f && ldir.y == 0.0f && ldir.z == 0.0f) ldir = normal;
      return 1.0f;
    }
  }
}

float DiffuseTerm(u32 chan, float attn, const Vec3& ldir, const Vec3& normal) {
  float dif = Dot(ldir, normal);
  switch (Bits(chan, 7, 2)) {
    case 0: return attn;
    case 1: return attn * dif;
    default: return attn * std::max(0.0f, dif);
  }
}

inline u32 LightMask(u32 chan) { return Bits(chan, 2, 4) | (Bits(chan, 11, 4) << 4); }

void Lighting(const InputVertex& in, OutputVertex& out) {
  for (int c = 0; c < 2; c++) {
    u32 color_chan = g_xf[XF_COLOR_CHAN + c];
    u32 alpha_chan = g_xf[XF_ALPHA_CHAN + c];
    u32 mat_reg = g_xf[XF_MAT_COLOR + c], amb_reg = g_xf[XF_AMB_COLOR + c];
    u8 mat[4], amb[4];
    for (int i = 0; i < 4; i++) {
      mat[i] = (u8)(mat_reg >> (24 - 8 * i));
      amb[i] = (u8)(amb_reg >> (24 - 8 * i));
    }
    u8 result[4];

    // Color (RGB)
    const u8* mat_src = Bits(color_chan, 0, 1) ? in.color[c] : mat;
    if (Bits(color_chan, 1, 1)) {
      const u8* amb_src = Bits(color_chan, 6, 1) ? in.color[c] : amb;
      float light[3] = {(float)amb_src[0], (float)amb_src[1], (float)amb_src[2]};
      u32 mask = LightMask(color_chan);
      for (u32 n = 0; n < 8; n++) {
        if (!(mask & (1u << n))) continue;
        Light l = GetLight(n);
        Vec3 ldir = Sub(l.pos, out.mv_position);
        float k = DiffuseTerm(color_chan, Attenuation(l, ldir, out.normal[0], color_chan), ldir, out.normal[0]);
        for (int i = 0; i < 3; i++) light[i] += l.color[i] * k;
      }
      for (int i = 0; i < 3; i++) {
        int li = std::clamp((int)light[i], 0, 255);
        result[i] = (u8)((mat_src[i] * (li + (li >> 7))) >> 8);
      }
    } else {
      for (int i = 0; i < 3; i++) result[i] = mat_src[i];
    }

    // Alpha
    u8 mat_a = Bits(alpha_chan, 0, 1) ? in.color[c][3] : mat[3];
    if (Bits(alpha_chan, 1, 1)) {
      float light = Bits(alpha_chan, 6, 1) ? (float)in.color[c][3] : (float)amb[3];
      u32 mask = LightMask(alpha_chan);
      for (u32 n = 0; n < 8; n++) {
        if (!(mask & (1u << n))) continue;
        Light l = GetLight(n);
        Vec3 ldir = Sub(l.pos, out.mv_position);
        light += l.color[3] * DiffuseTerm(alpha_chan, Attenuation(l, ldir, out.normal[0], alpha_chan), ldir,
                                         out.normal[0]);
      }
      int la = std::clamp((int)light, 0, 255);
      result[3] = (u8)((mat_a * (la + (la >> 7))) >> 8);
    } else {
      result[3] = mat_a;
    }
    for (int i = 0; i < 4; i++) out.color[c][i] = (float)result[i];
  }
}

void TexGen(const InputVertex& in, OutputVertex& out) {
  u32 num = std::min<u32>(Bits(g_xf[XF_NUM_TEXGEN], 0, 4), 8);
  bool dual = g_xf[XF_DUALTEX] & 1;
  for (u32 n = 0; n < num; n++) {
    u32 info = g_xf[XF_TEXMTX_INFO + n];
    u32 type = Bits(info, 4, 3);
    Vec3& dst = out.texcoords[n];
    if (type == 0) {  // regular
      u32 row = Bits(info, 7, 5);
      Vec3 src;
      if (row == 0) src = in.position;
      else if (row == 1) src = in.normal[0];
      else if (row == 3) src = in.normal[1];
      else if (row == 4) src = in.normal[2];
      else if (row >= 5 && row <= 12) src = {in.texcoords[row - 5][0], in.texcoords[row - 5][1], 1.0f};
      else src = {0.0f, 0.0f, 1.0f};
      if (std::isnan(src.x)) src.x = 1.0f;
      if (std::isnan(src.y)) src.y = 1.0f;
      if (std::isnan(src.z)) src.z = 1.0f;
      if (!Bits(info, 2, 1)) src.z = 1.0f;  // AB11 input form
      const float* m = XFFloats(XF_POS_MATRICES + in.texmtx[n] * 4);
      bool stq = Bits(info, 1, 1);
      dst.x = m[0] * src.x + m[1] * src.y + m[2] * src.z + m[3];
      dst.y = m[4] * src.x + m[5] * src.y + m[6] * src.z + m[7];
      dst.z = stq ? m[8] * src.x + m[9] * src.y + m[10] * src.z + m[11] : 1.0f;
      if (dual) {
        u32 post = g_xf[XF_POSTMTX_INFO + n];
        Vec3 t = Bits(post, 8, 1) ? Normalized(dst) : dst;
        dst = MulMat34(XFFloats(XF_POST_MATRICES + Bits(post, 0, 6) * 4), t);
      }
      if (dst.z == 0.0f) {
        dst.x = std::clamp(dst.x / 2.0f, -1.0f, 1.0f);
        dst.y = std::clamp(dst.y / 2.0f, -1.0f, 1.0f);
      }
    } else if (type == 1) {  // emboss
      Light l = GetLight(Bits(info, 15, 3));
      Vec3 ldir = Normalized(Sub(l.pos, out.mv_position));
      const Vec3& base = out.texcoords[Bits(info, 12, 3)];
      dst = {base.x + Dot(ldir, out.normal[1]), base.y + Dot(ldir, out.normal[2]), base.z};
    } else {  // color channel 0/1
      int c = type == 2 ? 0 : 1;
      dst = {out.color[c][0] / 255.0f, out.color[c][1] / 255.0f, 1.0f};
    }
  }
  // Normalised coordinates -> texel units (BP texcoord scale registers).
  for (u32 n = 0; n < num; n++) {
    out.texcoords[n].x *= (float)(Bits(g_bp[0x30 + n * 2], 0, 16) + 1);
    out.texcoords[n].y *= (float)(Bits(g_bp[0x31 + n * 2], 0, 16) + 1);
  }
}

}  // namespace

void TransformVertex(const InputVertex& in, OutputVertex& out) {
  // Position
  out.mv_position = MulMat34(XFFloats(XF_POS_MATRICES + in.posmtx * 4), in.position);
  const float* p = XFFloats(XF_PROJECTION);
  const Vec3& v = out.mv_position;
  if (g_xf[XF_PROJECTION + 6] == 0) {  // perspective
    out.proj[0] = p[0] * v.x + p[1] * v.z;
    out.proj[1] = p[2] * v.y + p[3] * v.z;
    out.proj[2] = (p[4] * v.z + p[5]) * (1.0f - 1e-7f);
    out.proj[3] = -v.z;
  } else {  // orthographic
    out.proj[0] = p[0] * v.x + p[1];
    out.proj[1] = p[2] * v.y + p[3];
    out.proj[2] = p[4] * v.z + p[5];
    out.proj[3] = 1.0f;
  }
  // Normals (only the first is normalised: binormals scale emboss mapping)
  const float* nm = XFFloats(XF_NORMAL_MATRICES + (in.posmtx & 31) * 3);
  for (int n = 0; n < 3; n++) out.normal[n] = MulMat33(nm, in.normal[n]);
  out.normal[0] = Normalized(out.normal[0]);

  Lighting(in, out);
  TexGen(in, out);
}

}  // namespace Video
