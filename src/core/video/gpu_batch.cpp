// emugcxbox360 - merges consecutive GPU draws that share the same state
// SPDX-License-Identifier: GPL-2.0-or-later
//
// GX games submit many small primitives (a strip or a fan each) with no state
// change in between. Each draw costs the backend a full state comparison,
// constant upload and draw packet, so draws whose GpuDrawState is identical
// are concatenated (triangle lists keep their order, so the result is the
// same) and submitted as one before any other backend operation.
#include <cstring>
#include <vector>

#include "core/video/gpu_backend.h"

namespace Video {

namespace {

class BatchingBackend : public GpuBackend {
 public:
  explicit BatchingBackend(GpuBackend* inner) : m_inner(inner) { m_vertices.reserve(MAX_VERTICES); }

  void Draw(const GpuDrawState& state, const GpuVertex* v, u32 count) override {
    if (!count) return;
    if (m_vertices.empty() || m_vertices.size() + count > MAX_VERTICES || !SameState(state)) {
      Flush();
      if (count > MAX_VERTICES) {
        m_inner->Draw(state, v, count);
        return;
      }
      memcpy(&m_state, &state, sizeof(state));
    }
    m_vertices.insert(m_vertices.end(), v, v + count);
  }
  void ClearEFB(int x0, int y0, int x1, int y1, bool color, bool alpha, bool depth, u32 rgba, u32 z24) override {
    Flush();
    m_inner->ClearEFB(x0, y0, x1, y1, color, alpha, depth, rgba, z24);
  }
  void ReadEFB(u32* color, u32* depth) override {
    Flush();
    m_inner->ReadEFB(color, depth);
  }
  void ReadEFBRect(int x0, int y0, int x1, int y1, bool color, bool depth, u32* color_buf, u32* depth_buf) override {
    Flush();
    m_inner->ReadEFBRect(x0, y0, x1, y1, color, depth, color_buf, depth_buf);
  }
  bool CopyToXFB(u32 dest, int sx, int sy, int w, int h, int out_h) override {
    Flush();
    return m_inner->CopyToXFB(dest, sx, sy, w, h, out_h);
  }
  bool PresentXFB(u32 addr, int width, int height) override {
    Flush();
    return m_inner->PresentXFB(addr, width, height);
  }

  void Flush() override {
    if (m_vertices.empty()) return;
    m_inner->Draw(m_state, m_vertices.data(), (u32)m_vertices.size());
    m_vertices.clear();
  }

 private:
  // Large enough to merge a whole display list, small enough for one
  // backend vertex upload.
  static constexpr size_t MAX_VERTICES = 24 * 1024;

  bool SameState(const GpuDrawState& s) const {
    if (s.tex_mask != m_state.tex_mask || s.sc_left != m_state.sc_left || s.sc_top != m_state.sc_top ||
        s.sc_right != m_state.sc_right || s.sc_bottom != m_state.sc_bottom)
      return false;
    if (memcmp(s.tev_regs, m_state.tev_regs, sizeof(s.tev_regs)) || memcmp(s.tev_konst, m_state.tev_konst, sizeof(s.tev_konst)))
      return false;
    for (u32 t = 0; t < 8; t++) {
      if (!(s.tex_mask & (1u << t))) continue;
      const GpuTexture &a = s.tex[t], &b = m_state.tex[t];
      if (a.rgba != b.rgba || a.id != b.id || a.width != b.width || a.height != b.height || a.wrap_s != b.wrap_s ||
          a.wrap_t != b.wrap_t || a.linear != b.linear)
        return false;
    }
    return memcmp(s.bp, m_state.bp, sizeof(s.bp)) == 0;
  }

  GpuBackend* m_inner;
  GpuDrawState m_state;
  std::vector<GpuVertex> m_vertices;
};

}  // namespace

GpuBackend* WithDrawBatching(GpuBackend* backend) { return new BatchingBackend(backend); }

}  // namespace Video
