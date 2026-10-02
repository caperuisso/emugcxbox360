// SPDX-License-Identifier: GPL-2.0-or-later
// Primitive assembly, clipping, culling and viewport transform.
#include <cmath>

#include "core/video/video_internal.h"

namespace Video {

namespace {

enum Primitive { QUADS = 0, QUADS2 = 1, TRIANGLES = 2, STRIP = 3, FAN = 4, LINES = 5, LINESTRIP = 6, POINTS = 7 };

u32 s_primitive;
u32 s_count;
OutputVertex s_first, s_prev, s_prev2, s_quad[3];

void ToScreen(OutputVertex& v) {
  const float* vp = XFFloats(XF_VIEWPORT);  // wd, ht, zRange, xOrig, yOrig, farZ
  float inv_w = 1.0f / v.proj[3];
  v.screen.x = v.proj[0] * inv_w * vp[0] + vp[3];
  v.screen.y = v.proj[1] * inv_w * vp[1] + vp[4];
  v.screen.z = v.proj[2] * inv_w * vp[2] + vp[5];
}

OutputVertex Lerp(const OutputVertex& a, const OutputVertex& b, float t) {
  OutputVertex r;
  for (int i = 0; i < 4; i++) r.proj[i] = a.proj[i] + (b.proj[i] - a.proj[i]) * t;
  r.mv_position = a.mv_position;
  for (int n = 0; n < 3; n++) r.normal[n] = a.normal[n];
  for (int c = 0; c < 2; c++)
    for (int i = 0; i < 4; i++) r.color[c][i] = a.color[c][i] + (b.color[c][i] - a.color[c][i]) * t;
  for (int n = 0; n < 8; n++) {
    r.texcoords[n].x = a.texcoords[n].x + (b.texcoords[n].x - a.texcoords[n].x) * t;
    r.texcoords[n].y = a.texcoords[n].y + (b.texcoords[n].y - a.texcoords[n].y) * t;
    r.texcoords[n].z = a.texcoords[n].z + (b.texcoords[n].z - a.texcoords[n].z) * t;
  }
  return r;
}

// Clip planes: near (w > eps) plus a guard band keeping screen coordinates in
// a range the fixed-point rasterizer handles.
constexpr float GUARD = 8.0f;
constexpr int NUM_PLANES = 5;
inline float PlaneDist(const OutputVertex& v, int plane) {
  const float* p = v.proj;
  switch (plane) {
    case 0: return p[3] - 1e-5f;
    case 1: return GUARD * p[3] - p[0];
    case 2: return GUARD * p[3] + p[0];
    case 3: return GUARD * p[3] - p[1];
    default: return GUARD * p[3] + p[1];
  }
}

bool IsBackface(const OutputVertex& v0, const OutputVertex& v1, const OutputVertex& v2) {
  float x0 = v0.proj[0], x1 = v1.proj[0], x2 = v2.proj[0];
  float y0 = v0.proj[1], y1 = v1.proj[1], y2 = v2.proj[1];
  float w0 = v0.proj[3], w1 = v1.proj[3], w2 = v2.proj[3];
  float normal_z = (x0 * w2 - x2 * w0) * y1 + (x2 * y0 - x0 * y2) * w1 + (y2 * w0 - y0 * w2) * x1;
  bool back = normal_z <= 0.0f;
  if (XFFloat(XF_VIEWPORT + 1) > 0) back = !back;
  return back;
}

void ProcessTriangle(const OutputVertex& a, const OutputVertex& b, const OutputVertex& c) {
  // Trivial rejection when all vertices are outside the same plane
  for (int p = 0; p < NUM_PLANES; p++)
    if (PlaneDist(a, p) < 0 && PlaneDist(b, p) < 0 && PlaneDist(c, p) < 0) return;

  bool back = IsBackface(a, b, c);
  u32 cull = Bits(g_bp[BP_GENMODE], 14, 2);  // 0 none, 1 back, 2 front, 3 all
  if ((!back && (cull == 1 || cull == 3)) || (back && (cull == 2 || cull == 3))) return;

  // Rasterize with a consistent (front facing) winding.
  OutputVertex poly[16], tmp[16];
  int n = 3;
  poly[0] = a;
  poly[1] = back ? c : b;
  poly[2] = back ? b : c;

  for (int p = 0; p < NUM_PLANES && n >= 3; p++) {
    bool all_in = true;
    for (int i = 0; i < n; i++)
      if (PlaneDist(poly[i], p) < 0) all_in = false;
    if (all_in) continue;
    int m = 0;
    for (int i = 0; i < n && m < 15; i++) {
      const OutputVertex& cur = poly[i];
      const OutputVertex& nxt = poly[(i + 1) % n];
      float dc = PlaneDist(cur, p), dn = PlaneDist(nxt, p);
      if (dc >= 0) tmp[m++] = cur;
      if ((dc >= 0) != (dn >= 0) && m < 15) tmp[m++] = Lerp(cur, nxt, dc / (dc - dn));
    }
    n = m;
    for (int i = 0; i < n; i++) poly[i] = tmp[i];
  }
  if (n < 3) return;
  for (int i = 0; i < n; i++) ToScreen(poly[i]);
  for (int i = 1; i + 1 < n; i++) {
    g_stats.triangles++;
    DrawTriangle(&poly[0], &poly[i], &poly[i + 1]);
  }
}

// Lines and points are expanded to screen-space quads (two triangles).
void DrawQuadScreen(OutputVertex q[4]) {
  g_stats.triangles += 2;
  DrawTriangle(&q[0], &q[1], &q[2]);
  DrawTriangle(&q[0], &q[2], &q[3]);
}

void ProcessLine(OutputVertex a, OutputVertex b) {
  if (a.proj[3] <= 0 || b.proj[3] <= 0) return;  // no near clipping for lines yet
  ToScreen(a);
  ToScreen(b);
  float half = Bits(g_bp[BP_LINEPTWIDTH], 0, 8) / 12.0f;
  float dx = b.screen.x - a.screen.x, dy = b.screen.y - a.screen.y;
  float px = 0, py = 0;
  if (std::fabs(dx) > std::fabs(dy))
    py = half;
  else
    px = half;
  OutputVertex q[4] = {a, b, b, a};
  q[0].screen.x -= px; q[0].screen.y -= py;
  q[1].screen.x -= px; q[1].screen.y -= py;
  q[2].screen.x += px; q[2].screen.y += py;
  q[3].screen.x += px; q[3].screen.y += py;
  DrawQuadScreen(q);
}

void ProcessPoint(OutputVertex a) {
  if (a.proj[3] <= 0) return;
  ToScreen(a);
  float r = Bits(g_bp[BP_LINEPTWIDTH], 8, 8) / 12.0f;
  OutputVertex q[4] = {a, a, a, a};
  q[0].screen.x -= r; q[0].screen.y -= r;
  q[1].screen.x += r; q[1].screen.y -= r;
  q[2].screen.x += r; q[2].screen.y += r;
  q[3].screen.x -= r; q[3].screen.y += r;
  DrawQuadScreen(q);
}

}  // namespace

void BeginPrimitive(u32 primitive) {
  s_primitive = primitive;
  s_count = 0;
}

void AddVertex(const OutputVertex& v) {
  switch (s_primitive) {
    case QUADS:
    case QUADS2:
      if (s_count % 4 < 3) {
        s_quad[s_count % 4] = v;
      } else {
        ProcessTriangle(s_quad[0], s_quad[1], s_quad[2]);
        ProcessTriangle(s_quad[0], s_quad[2], v);
      }
      break;
    case TRIANGLES:
      if (s_count % 3 < 2)
        s_quad[s_count % 3] = v;
      else
        ProcessTriangle(s_quad[0], s_quad[1], v);
      break;
    case STRIP:
      if (s_count >= 2) {
        if (s_count & 1)
          ProcessTriangle(s_prev, s_prev2, v);
        else
          ProcessTriangle(s_prev2, s_prev, v);
      }
      s_prev2 = s_prev;
      s_prev = v;
      break;
    case FAN:
      if (s_count == 0) s_first = v;
      if (s_count >= 2) ProcessTriangle(s_first, s_prev, v);
      s_prev = v;
      break;
    case LINES:
      if (s_count & 1)
        ProcessLine(s_prev, v);
      else
        s_prev = v;
      break;
    case LINESTRIP:
      if (s_count >= 1) ProcessLine(s_prev, v);
      s_prev = v;
      break;
    case POINTS:
      ProcessPoint(v);
      break;
  }
  s_count++;
}

void EndPrimitive() {}

}  // namespace Video
