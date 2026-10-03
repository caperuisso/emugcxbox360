// emugcxbox360 - Xenos shader microcode interpreter and simulated GPU backend
// SPDX-License-Identifier: GPL-2.0-or-later
#include "xenos/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "platform/platform.h"
#include "xenos/gx_draw.h"

namespace XenosSim {

namespace {

inline u32 B(u32 v, u32 lo, u32 n) { return (v >> lo) & ((1u << n) - 1); }

struct Vec4 {
  float v[4];
};

struct ShaderIO {
  // inputs
  const float (*consts)[4] = nullptr;
  const float* vb = nullptr;  // vertex buffer (floats)
  u32 vb_floats = 0;
  const Video::GpuTexture* tex = nullptr;  // 8 units
  // state
  Vec4 r[64];
  // outputs
  Vec4 exports[64];
  bool killed = false;
};


Vec4 Operand(ShaderIO& io, const u32 w[3], int i) {
  // i = 1, 2, 3
  u32 reg = (i == 1) ? B(w[2], 16, 8) : (i == 2) ? B(w[2], 8, 8) : B(w[2], 0, 8);
  bool temp = (i == 1) ? B(w[2], 31, 1) : (i == 2) ? B(w[2], 30, 1) : B(w[2], 29, 1);
  u32 swz = (i == 1) ? B(w[1], 16, 8) : (i == 2) ? B(w[1], 8, 8) : B(w[1], 0, 8);
  bool neg = (i == 1) ? B(w[1], 26, 1) : (i == 2) ? B(w[1], 25, 1) : B(w[1], 24, 1);
  Vec4 src;
  bool absval;
  if (temp) {
    src = io.r[reg & 0x3F];
    absval = reg & 0x80;
  } else {
    memcpy(src.v, io.consts[reg], sizeof(src.v));
    absval = B(w[0], 7, 1);
  }
  Vec4 out;
  for (u32 c = 0; c < 4; c++) {
    float f = src.v[((swz >> (2 * c)) + c) & 3];
    if (absval) f = std::fabs(f);
    if (neg) f = -f;
    out.v[c] = f;
  }
  return out;
}

// D3D9 "legacy" multiply: 0 * anything = 0
inline float Mul(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }

void ExecAlu(ShaderIO& io, const u32 w[3]) {
  u32 vdst = B(w[0], 0, 6), sdst = B(w[0], 8, 6);
  bool exp = B(w[0], 15, 1);
  u32 vmask = B(w[0], 16, 4), smask = B(w[0], 20, 4);
  bool vclamp = B(w[0], 24, 1), sclamp = B(w[0], 25, 1);
  u32 sop = B(w[0], 26, 6), vop = B(w[2], 24, 5);
  Vec4 a = Operand(io, w, 1), b = Operand(io, w, 2), c = Operand(io, w, 3);

  Vec4 vr = {{0, 0, 0, 0}};
  switch (vop) {
    case 0: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] + b.v[i]; break;
    case 1: for (int i = 0; i < 4; i++) vr.v[i] = Mul(a.v[i], b.v[i]); break;
    case 2: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] >= b.v[i] ? a.v[i] : b.v[i]; break;
    case 3: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
    case 4: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] == b.v[i] ? 1.0f : 0.0f; break;
    case 5: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] > b.v[i] ? 1.0f : 0.0f; break;
    case 6: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
    case 7: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] != b.v[i] ? 1.0f : 0.0f; break;
    case 8: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] - std::floor(a.v[i]); break;
    case 9: for (int i = 0; i < 4; i++) vr.v[i] = std::trunc(a.v[i]); break;
    case 10: for (int i = 0; i < 4; i++) vr.v[i] = std::floor(a.v[i]); break;
    case 11: for (int i = 0; i < 4; i++) vr.v[i] = Mul(a.v[i], b.v[i]) + c.v[i]; break;
    case 12: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] == 0.0f ? b.v[i] : c.v[i]; break;
    case 13: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] >= 0.0f ? b.v[i] : c.v[i]; break;
    case 14: for (int i = 0; i < 4; i++) vr.v[i] = a.v[i] > 0.0f ? b.v[i] : c.v[i]; break;
    case 15: {
      float d = Mul(a.v[0], b.v[0]) + Mul(a.v[1], b.v[1]) + Mul(a.v[2], b.v[2]) + Mul(a.v[3], b.v[3]);
      for (int i = 0; i < 4; i++) vr.v[i] = d;
      break;
    }
    case 16: {
      float d = Mul(a.v[0], b.v[0]) + Mul(a.v[1], b.v[1]) + Mul(a.v[2], b.v[2]);
      for (int i = 0; i < 4; i++) vr.v[i] = d;
      break;
    }
    case 24: case 25: case 26: case 27: {
      bool kill = false;
      for (int i = 0; i < 4; i++) {
        bool t = vop == 24 ? a.v[i] == b.v[i] : vop == 25 ? a.v[i] > b.v[i] : vop == 26 ? a.v[i] >= b.v[i] : a.v[i] != b.v[i];
        kill |= t;
      }
      if (kill) io.killed = true;
      for (int i = 0; i < 4; i++) vr.v[i] = kill ? 1.0f : 0.0f;
      break;
    }
    default:
      fprintf(stderr, "xenos sim: unsupported vector op %u\n", vop);
      break;
  }
  if (vclamp)
    for (float& f : vr.v) f = std::min(1.0f, std::max(0.0f, f));

  bool has_scalar = sop != 50;
  float sr = 0;
  if (has_scalar) {
    u32 swz = B(w[1], 0, 8);
    float sa = c.v[3], sb = c.v[0];  // already swizzled operand 3: a = .w, b = .x
    (void)swz;
    switch (sop) {
      case 0: sr = sa + sb; break;
      case 2: sr = Mul(sa, sb); break;
      case 5: sr = sa >= sb ? sa : sb; break;
      case 6: sr = sa < sb ? sa : sb; break;
      case 11: sr = sa - std::floor(sa); break;
      case 13: sr = std::floor(sa); break;
      case 14: sr = std::exp2(sa); break;
      case 16: sr = sa == 1.0f ? 0.0f : std::log2(sa); break;
      case 19: sr = sa == 1.0f ? 1.0f : 1.0f / sa; break;
      case 22: sr = sa == 1.0f ? 1.0f : 1.0f / std::sqrt(sa); break;
      case 25: sr = sa - sb; break;
      case 40: sr = std::sqrt(sa); break;
      default: fprintf(stderr, "xenos sim: unsupported scalar op %u\n", sop); break;
    }
    if (sclamp) sr = std::min(1.0f, std::max(0.0f, sr));
  }

  if (exp) {
    Vec4& e = io.exports[vdst];
    for (int i = 0; i < 4; i++) {
      bool vm = vmask & (1 << i), sm = smask & (1 << i);
      if (vm && sm) e.v[i] = 1.0f;
      else if (vm) e.v[i] = vr.v[i];
      else if (sm) e.v[i] = sr;
    }
  } else {
    for (int i = 0; i < 4; i++)
      if (vmask & (1 << i)) io.r[vdst].v[i] = vr.v[i];
    if (has_scalar)
      for (int i = 0; i < 4; i++)
        if (smask & (1 << i)) io.r[sdst].v[i] = sr;
  }
}

float FetchSel(u32 sel, const float* comps, u32 n, float keep) {
  if (sel < 4) return sel < n ? comps[sel] : (sel == 3 ? 1.0f : 0.0f);
  if (sel == 4) return 0.0f;
  if (sel == 5) return 1.0f;
  return keep;
}

void Sample(const Video::GpuTexture& t, float u, float v, float out[4]) {
  if (!t.rgba || t.width <= 0) {
    out[0] = out[1] = out[2] = out[3] = 0;
    return;
  }
  auto wrap = [](int c, int size, u32 mode) {
    switch (mode) {
      case 1: c %= size; if (c < 0) c += size; return c;
      case 2: {
        int p = 2 * size;
        c %= p;
        if (c < 0) c += p;
        return c < size ? c : p - 1 - c;
      }
      default: return std::clamp(c, 0, size - 1);
    }
  };
  auto texel = [&](int x, int y, float o[4]) {
    x = wrap(x, t.width, t.wrap_s);
    y = wrap(y, t.height, t.wrap_t);
    const u8* p = t.rgba + ((size_t)y * t.width + x) * 4;
    for (int i = 0; i < 4; i++) o[i] = p[i] / 255.0f;
  };
  float x = u * t.width - 0.5f, y = v * t.height - 0.5f;
  if (!t.linear) {
    texel((int)std::floor(x + 0.5f), (int)std::floor(y + 0.5f), out);
    return;
  }
  int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
  float fx = x - x0, fy = y - y0;
  float a[4], b[4], c[4], d[4];
  texel(x0, y0, a);
  texel(x0 + 1, y0, b);
  texel(x0, y0 + 1, c);
  texel(x0 + 1, y0 + 1, d);
  for (int i = 0; i < 4; i++)
    out[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy;
}

void ExecFetch(ShaderIO& io, const u32 w[3]) {
  u32 op = B(w[0], 0, 5);
  u32 src = B(w[0], 5, 6), dst = B(w[0], 12, 6);
  u32 dswz = B(w[1], 0, 12);
  float comps[4] = {0, 0, 0, 0};
  u32 n = 4;
  if (op == 0) {
    u32 sel = B(w[0], 30, 2);
    u32 index = (u32)io.r[src].v[sel];
    u32 fmt = B(w[1], 16, 6), stride = B(w[2], 0, 8), offset = B(w[2], 8, 23);
    n = fmt == 37 ? 2 : (fmt == 57 ? 3 : 4);
    u32 base = index * stride + offset;
    for (u32 i = 0; i < n; i++) comps[i] = base + i < io.vb_floats ? io.vb[base + i] : 0.0f;
  } else if (op == 1) {
    u32 unit = B(w[0], 20, 5);
    u32 sw = B(w[0], 26, 6);
    float u = io.r[src].v[sw & 3], v = io.r[src].v[(sw >> 2) & 3];
    Sample(io.tex[unit & 7], u, v, comps);
  } else {
    fprintf(stderr, "xenos sim: unsupported fetch op %u\n", op);
  }
  Vec4& d = io.r[dst];
  Vec4 old = d;
  for (u32 i = 0; i < 4; i++) d.v[i] = FetchSel(B(dswz, 3 * i, 3), comps, n, old.v[i]);
}

void Run(const std::vector<u32>& code, ShaderIO& io) {
  // Control flow: pairs of instructions in 3 dwords, until an exec_end.
  for (size_t cfi = 0; cfi + 3 <= code.size(); cfi += 3) {
    u32 d0 = code[cfi], d1 = code[cfi + 1], d2 = code[cfi + 2];
    u32 cf[2][2] = {{d0, d1 & 0xFFFF}, {(d1 >> 16) | (d2 << 16), d2 >> 16}};
    for (auto& c : cf) {
      u32 op = B(c[1], 12, 4);
      if (op == 1 || op == 2) {
        u32 addr = B(c[0], 0, 12), count = B(c[0], 12, 3), seq = B(c[0], 16, 12);
        for (u32 k = 0; k < count; k++) {
          const u32* w = &code[(addr + k) * 3];
          if ((seq >> (2 * k)) & 1)
            ExecFetch(io, w);
          else
            ExecAlu(io, w);
        }
        if (op == 2) return;
      } else if (op != 0 && op != 12) {
        fprintf(stderr, "xenos sim: unsupported control flow op %u\n", op);
        return;
      }
    }
  }
}

// ---- Output merger ----
float Factor(u32 f, const float* src, const float* dst, int ch) {
  switch (f) {
    case XenosGx::BLEND_ZERO: return 0;
    case XenosGx::BLEND_ONE: return 1;
    case XenosGx::BLEND_SRCCOLOR: return src[ch];
    case XenosGx::BLEND_INVSRCCOLOR: return 1 - src[ch];
    case XenosGx::BLEND_SRCALPHA: return src[3];
    case XenosGx::BLEND_INVSRCALPHA: return 1 - src[3];
    case XenosGx::BLEND_DESTCOLOR: return dst[ch];
    case XenosGx::BLEND_INVDESTCOLOR: return 1 - dst[ch];
    case XenosGx::BLEND_DESTALPHA: return dst[3];
    default: return 1 - dst[3];
  }
}

float BlendOp(u32 op, float s, float d) {
  switch (op) {
    case XenosGx::BLENDOP_SUBTRACT: return s - d;
    case XenosGx::BLENDOP_REVSUBTRACT: return d - s;
    default: return s + d;
  }
}

bool DepthCompare(u32 f, float a, float b) {
  switch (f) {
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

class SimBackend : public Video::GpuBackend {
 public:
  SimBackend() : m_color(XenosGx::EFB_W * XenosGx::EFB_H), m_depth(XenosGx::EFB_W * XenosGx::EFB_H) {
    for (auto& c : m_color) c = {0, 0, 0, 1};
    for (auto& d : m_depth) d = 1.0f;
  }

  void Draw(const Video::GpuDrawState& st, const Video::GpuVertex* v, u32 count) override {
    XenosGx::PreparedDraw pd;
    XenosGx::Prepare(st, v, count, pd);
    Execute(pd, st.tex);
  }

  void ClearEFB(int x0, int y0, int x1, int y1, bool color, bool alpha, bool depth, u32 rgba, u32 z24) override {
    XenosGx::PreparedDraw pd;
    XenosGx::PrepareClear(x0, y0, x1, y1, color, alpha, depth, rgba, z24, pd);
    Execute(pd, nullptr);
  }

  // XFB copies stay in the backend and are presented directly (as the real
  // Xenos backend does), so this path is exercised on the PC too.
  bool CopyToXFB(u32 dest, int sx, int sy, int w, int h, int out_h) override {
    Frame& f = m_xfb[dest];
    f.w = w;
    f.h = out_h;
    f.argb.resize((size_t)w * out_h);
    for (int oy = 0; oy < out_h; oy++) {
      int y = std::min(XenosGx::EFB_H - 1, std::max(0, sy + std::min(h - 1, oy * h / std::max(1, out_h))));
      for (int x = 0; x < w; x++) {
        int ex = std::min(XenosGx::EFB_W - 1, std::max(0, sx + x));
        const float* c = m_color[(size_t)y * XenosGx::EFB_W + ex].v;
        u32 r = (u32)std::lround(std::min(1.0f, std::max(0.0f, c[0])) * 255.0f);
        u32 g = (u32)std::lround(std::min(1.0f, std::max(0.0f, c[1])) * 255.0f);
        u32 b = (u32)std::lround(std::min(1.0f, std::max(0.0f, c[2])) * 255.0f);
        f.argb[(size_t)oy * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
      }
    }
    m_last = dest;
    return true;
  }

  bool PresentXFB(u32 addr, int width, int height) override {
    auto it = m_xfb.find(addr);
    if (it == m_xfb.end()) it = m_xfb.find(m_last);
    if (it == m_xfb.end() || !g_host) return false;
    const Frame& f = it->second;
    m_present.assign((size_t)width * height, 0xFF000000u);
    for (int y = 0; y < std::min(height, f.h); y++)
      for (int x = 0; x < std::min(width, f.w); x++) m_present[(size_t)y * width + x] = f.argb[(size_t)y * f.w + x];
    g_host->PresentFrame(m_present.data(), width, height);
    return true;
  }

  void ReadEFBRect(int x0, int y0, int x1, int y1, bool color, bool depth, u32* cbuf, u32* zbuf) override {
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++) {
        size_t i = (size_t)y * XenosGx::EFB_W + x;
        if (color) {
          const float* c = m_color[i].v;
          u8 b[4];
          for (int k = 0; k < 4; k++) b[k] = (u8)std::lround(std::min(1.0f, std::max(0.0f, c[k])) * 255.0f);
          cbuf[i] = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
        }
        if (depth) zbuf[i] = (u32)std::lround(std::min(1.0f, std::max(0.0f, m_depth[i])) * 16777215.0f);
      }
  }

  void ReadEFB(u32* color, u32* depth) override {
    for (size_t i = 0; i < m_color.size(); i++) {
      const float* c = m_color[i].v;
      u8 b[4];
      for (int k = 0; k < 4; k++) b[k] = (u8)std::lround(std::min(1.0f, std::max(0.0f, c[k])) * 255.0f);
      color[i] = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
      depth[i] = (u32)std::lround(std::min(1.0f, std::max(0.0f, m_depth[i])) * 16777215.0f);
    }
  }

 private:
  struct VsOut {
    float pos[4];
    Vec4 interp[16];
  };

  void Execute(const XenosGx::PreparedDraw& pd, const Video::GpuTexture* tex) {
    static const float kZero[256][4] = {};
    std::vector<VsOut> out(pd.vertex_count);
    for (u32 i = 0; i < pd.vertex_count; i++) {
      ShaderIO io;
      io.consts = kZero;
      io.vb = pd.vertices.data();
      io.vb_floats = (u32)pd.vertices.size();
      memset(io.r, 0, sizeof(io.r));
      memset(io.exports, 0, sizeof(io.exports));
      io.r[0].v[0] = (float)i;
      Run(pd.vs->shader.code, io);
      memcpy(out[i].pos, io.exports[62].v, sizeof(out[i].pos));
      for (int k = 0; k < 16; k++) out[i].interp[k] = io.exports[k];
    }
    float consts[256][4] = {};
    memcpy(consts, pd.ps_consts, sizeof(pd.ps_consts));
    static const Video::GpuTexture kNoTex[8] = {};
    for (u32 t = 0; t + 2 < pd.vertex_count; t += 3) Triangle(pd, &out[t], consts, tex ? tex : kNoTex);
  }

  void Triangle(const XenosGx::PreparedDraw& pd, const VsOut* v, float (*consts)[4], const Video::GpuTexture* tex) {
    float sx[3], sy[3], sz[3], iw[3];
    for (int i = 0; i < 3; i++) {
      float w = v[i].pos[3];
      if (w <= 0) return;
      iw[i] = 1.0f / w;
      sx[i] = (v[i].pos[0] * iw[i] + 1.0f) * (XenosGx::EFB_W * 0.5f);
      sy[i] = (1.0f - v[i].pos[1] * iw[i]) * (XenosGx::EFB_H * 0.5f);
      sz[i] = v[i].pos[2] * iw[i];
    }
    double area = ((double)sx[1] - sx[0]) * ((double)sy[2] - sy[0]) - ((double)sx[2] - sx[0]) * ((double)sy[1] - sy[0]);
    if (area == 0) return;
    // Edge functions; the top-left rule decides pixels exactly on an edge.
    auto edge = [&](int i, int j, double px, double py) {
      return ((double)sx[j] - sx[i]) * (py - sy[i]) - ((double)sy[j] - sy[i]) * (px - sx[i]);
    };
    auto top_left = [&](int i, int j) {
      double dx = (double)sx[j] - sx[i], dy = (double)sy[j] - sy[i];
      if (area < 0) dx = -dx, dy = -dy;
      return (dy == 0 && dx > 0) || dy < 0;
    };
    const XenosGx::RenderState& rs = pd.rs;
    int minx = std::max(rs.sc_left, (int)std::floor(std::min({sx[0], sx[1], sx[2]})));
    int maxx = std::min(rs.sc_right - 1, (int)std::ceil(std::max({sx[0], sx[1], sx[2]})));
    int miny = std::max(rs.sc_top, (int)std::floor(std::min({sy[0], sy[1], sy[2]})));
    int maxy = std::min(rs.sc_bottom - 1, (int)std::ceil(std::max({sy[0], sy[1], sy[2]})));
    minx = std::max(minx, 0), miny = std::max(miny, 0);
    maxx = std::min(maxx, XenosGx::EFB_W - 1), maxy = std::min(maxy, XenosGx::EFB_H - 1);
    bool tl0 = top_left(1, 2), tl1 = top_left(2, 0), tl2 = top_left(0, 1);
    for (int y = miny; y <= maxy; y++)
      for (int x = minx; x <= maxx; x++) {
        // Pixel centres at integer coordinates (Direct3D 9 convention)
        double px = x, py = y;
        double e0 = edge(1, 2, px, py), e1 = edge(2, 0, px, py), e2 = edge(0, 1, px, py);
        if (area < 0) e0 = -e0, e1 = -e1, e2 = -e2;
        if (e0 < 0 || e1 < 0 || e2 < 0) continue;
        if ((e0 == 0 && !tl0) || (e1 == 0 && !tl1) || (e2 == 0 && !tl2)) continue;
        double aa = std::fabs(area);
        float b0 = (float)(e0 / aa), b1 = (float)(e1 / aa), b2 = (float)(e2 / aa);
        float z = b0 * sz[0] + b1 * sz[1] + b2 * sz[2];
        float p0 = b0 * iw[0], p1 = b1 * iw[1], p2 = b2 * iw[2], ps = p0 + p1 + p2;
        p0 /= ps, p1 /= ps, p2 /= ps;
        ShaderIO io;
        io.consts = consts;
        io.tex = tex;
        memset(io.r, 0, sizeof(io.r));
        memset(io.exports, 0, sizeof(io.exports));
        for (int k = 0; k < 16; k++)
          for (int c = 0; c < 4; c++)
            io.r[k].v[c] = p0 * v[0].interp[k].v[c] + p1 * v[1].interp[k].v[c] + p2 * v[2].interp[k].v[c];
        Run(pd.ps->shader.code, io);
        if (io.killed) continue;
        size_t idx = (size_t)y * XenosGx::EFB_W + x;
        if (rs.z_enable && !DepthCompare(rs.z_func, z, m_depth[idx])) continue;
        if (rs.z_write) m_depth[idx] = z;
        float src[4], dst[4];
        for (int c = 0; c < 4; c++) src[c] = std::min(1.0f, std::max(0.0f, io.exports[0].v[c]));
        memcpy(dst, m_color[idx].v, sizeof(dst));
        float res[4];
        for (int c = 0; c < 3; c++)
          res[c] = BlendOp(rs.color_op, src[c] * Factor(rs.color_src, src, dst, c), dst[c] * Factor(rs.color_dst, src, dst, c));
        res[3] = BlendOp(rs.alpha_op, src[3] * Factor(rs.alpha_src, src, dst, 3), dst[3] * Factor(rs.alpha_dst, src, dst, 3));
        for (int c = 0; c < 4; c++) m_color[idx].v[c] = std::round(std::min(1.0f, std::max(0.0f, res[c])) * 255.0f) / 255.0f;
      }
  }

  std::vector<Vec4> m_color;
  std::vector<float> m_depth;
  struct Frame {
    int w = 0, h = 0;
    std::vector<u32> argb;
  };
  std::map<u32, Frame> m_xfb;
  u32 m_last = 0;
  std::vector<u32> m_present;
};

}  // namespace

void Install() { Video::g_gpu = new SimBackend(); }

}  // namespace XenosSim
