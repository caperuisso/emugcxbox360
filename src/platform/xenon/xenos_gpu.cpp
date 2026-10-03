// emugcxbox360 - Xenos GPU output for the Xbox 360 frontend
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Uses libxenon's Xe driver with shaders assembled at run time by
// xenos_ucode.h (libxenon itself can only load XDK-compiled shaders).
#include "platform/xenon/xenos_gpu.h"

#include <cstdlib>
#include <cstring>

#include <xenos/edram.h>
#include <xenos/xe.h>
#include <xenos/xenos.h>
extern "C" {
#include <xenos/xe_internal.h>
}

#include <memory>
#include <unordered_map>
#include <vector>

#include "core/video/gpu_backend.h"
#include "xenos/gx_draw.h"
#include "xenos/shaders.h"

extern "C" void Xe_pRBMayKick(struct XenosDevice* xe);

namespace XenosGpu {

void SaveEFBBeforePresent();  // GX backend: keeps the EFB while the EDRAM shows the frame

namespace {

XenosDevice s_xe_storage;
XenosDevice* s_xe = nullptr;
XenosSurface* s_fb = nullptr;
XenosShader* s_blit_vs = nullptr;
XenosShader* s_blit_ps = nullptr;
XenosSurface* s_tex = nullptr;
XenosVertexBuffer* s_vb = nullptr;
int s_tex_w = 0, s_tex_h = 0;
bool s_efb_active = false;  // the EDRAM currently holds the emulated EFB

// Wraps run-time assembled microcode in libxenon's shader object (no XDK
// container: an empty header without constants, the code already instantiated).
XenosShader* CreateShader(const Xenos::BuiltShader& b) {
  XenosShaderHeader* hdr = (XenosShaderHeader*)calloc(1, sizeof(XenosShaderHeader) + sizeof(XenosShaderData));
  hdr->magic = 0x102a1100;
  XenosShader* s = (XenosShader*)calloc(1, sizeof(XenosShader));
  s->shader = hdr;
  s->program_control = b.program_control;
  s->context_misc = b.context_misc;
  u32 size = (u32)(b.code.size() * 4);
  void* p = Xe_pAlloc(s_xe, &s->shader_phys[0], size, 0x100);
  memcpy(p, b.code.data(), size);
  Xe_pSyncToDevice(s_xe, p, size);
  s->shader_instance[0] = p;
  s->shader_phys_size = size;
  s->size = size;
  return s;
}

// Tiny 3x5 font for the performance overlay, drawn into the uploaded image.
// Lines are separated by '\n'.
void DrawOverlay(uint32_t* pixels, int pitch_px, int w, int h, const char* text) {
  static const char kChars[] = "0123456789.% FPSDCGAV";
  static const uint16_t kGlyphs[] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF,
                                     0x7BCF, 0x0002, 0x52A5, 0x0000, 0x79E4, 0x7BE4, 0x79CF, 0x6B6E, 0x7927,
                                     0x796F, 0x2BED, 0x5B6A};
  const int scale = 2, ox = 4, oy = 3;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) pixels[y * pitch_px + x] = 0xFF000000;
  int line = 0, col0 = 0;
  for (int c = 0; text[c]; c++) {
    if (text[c] == '\n') {
      line++;
      col0 = c + 1;
      continue;
    }
    const char* pos = strchr(kChars, text[c]);
    if (!pos) continue;
    uint16_t g = kGlyphs[pos - kChars];
    int cx = c - col0;
    for (int r = 0; r < 5; r++)
      for (int col = 0; col < 3; col++) {
        if (!(g & (1 << (14 - (r * 3 + col))))) continue;
        for (int dy = 0; dy < scale; dy++)
          for (int dx = 0; dx < scale; dx++) {
            int x = ox + (cx * 4 + col) * scale + dx, y = oy + line * 6 * scale + r * scale + dy;
            if (x < w && y < h) pixels[y * pitch_px + x] = 0xFFFFFF00;
          }
      }
  }
}

}  // namespace

bool Init() {
  s_xe = &s_xe_storage;
  Xe_Init(s_xe);
  s_fb = Xe_GetFramebufferSurface(s_xe);
  Xe_SetRenderTarget(s_xe, s_fb);
  s_blit_vs = CreateShader(Xenos::BlitVertexShader());
  s_blit_ps = CreateShader(Xenos::BlitPixelShader());
  s_vb = Xe_CreateVertexBuffer(s_xe, 12 * 6 * sizeof(float));
  edram_init(s_xe);
  return true;
}

namespace {

XenosSurface* s_overlay_tex = nullptr;
char s_overlay_text[64] = "";
bool s_overlay_dirty = true;

// Draws `tex` (texture coordinates u0..u1, v0..v1) fitted to the screen with a
// 4:3 aspect, plus the overlay, then shows the frame.
void DrawPicture(XenosSurface* tex, float u0, float v0, float u1, float v1) {
  if (s_efb_active) SaveEFBBeforePresent();
  Xe_SetRenderTarget(s_xe, s_fb);
  float sw = (float)s_fb->width, sh = (float)s_fb->height;
  float dh = sh, dw = sh * 4.0f / 3.0f;
  if (dw > sw) {
    dw = sw;
    dh = sw * 3.0f / 4.0f;
  }
  float x0 = -dw / sw, x1 = dw / sw, y0 = dh / sh, y1 = -dh / sh;
  // overlay quad in the top left corner (160 x 32 texels shown 1:1)
  float ox0 = x0 + 16.0f / sw * 2, oy0 = y0 - 16.0f / sh * 2;
  float ox1 = ox0 + 160.0f / sw * 2, oy1 = oy0 - 32.0f / sh * 2;
  const float quad[12][6] = {
      {x0, y0, 0, 1, u0, v0}, {x1, y0, 0, 1, u1, v0}, {x0, y1, 0, 1, u0, v1},
      {x1, y0, 0, 1, u1, v0}, {x1, y1, 0, 1, u1, v1}, {x0, y1, 0, 1, u0, v1},
      {ox0, oy0, 0, 1, 0, 0}, {ox1, oy0, 0, 1, 1, 0}, {ox0, oy1, 0, 1, 0, 1},
      {ox1, oy0, 0, 1, 1, 0}, {ox1, oy1, 0, 1, 1, 1}, {ox0, oy1, 0, 1, 0, 1},
  };
  void* v = Xe_VB_Lock(s_xe, s_vb, 0, sizeof(quad), XE_LOCK_WRITE);
  memcpy(v, quad, sizeof(quad));
  Xe_VB_Unlock(s_xe, s_vb);

  Xe_InvalidateState(s_xe);
  Xe_SetClearColor(s_xe, 0xFF000000);
  Xe_SetZEnable(s_xe, 0);
  Xe_SetCullMode(s_xe, XE_CULL_NONE);
  Xe_SetShader(s_xe, SHADER_TYPE_PIXEL, s_blit_ps, 0);
  Xe_SetShader(s_xe, SHADER_TYPE_VERTEX, s_blit_vs, 0);
  Xe_SetStreamSource(s_xe, 0, s_vb, 0, 6);
  Xe_SetTexture(s_xe, 0, tex);
  Xe_DrawPrimitive(s_xe, XE_PRIMTYPE_TRIANGLELIST, 0, 2);
  if (s_overlay_text[0]) {
    if (!s_overlay_tex) {
      s_overlay_tex = Xe_CreateTexture(s_xe, 160, 32, 1, XE_FMT_8888 | XE_FMT_ARGB, 0);
      s_overlay_tex->use_filtering = 0;
      s_overlay_tex->u_addressing = XE_TEXADDR_CLAMP;
      s_overlay_tex->v_addressing = XE_TEXADDR_CLAMP;
    }
    if (s_overlay_dirty) {
      uint8_t* d = (uint8_t*)Xe_Surface_LockRect(s_xe, s_overlay_tex, 0, 0, 0, 0, XE_LOCK_WRITE);
      for (int y = 0; y < 32; y++) memset(d + y * s_overlay_tex->wpitch, 0, 160 * 4);
      DrawOverlay((uint32_t*)d, s_overlay_tex->wpitch / 4, 160, 32, s_overlay_text);
      Xe_Surface_Unlock(s_xe, s_overlay_tex);
      s_overlay_dirty = false;
    }
    Xe_SetTexture(s_xe, 0, s_overlay_tex);
    Xe_DrawPrimitive(s_xe, XE_PRIMTYPE_TRIANGLELIST, 6, 2);
  }
  Xe_Resolve(s_xe);
  Xe_Sync(s_xe);
  s_efb_active = false;  // the EDRAM now holds the framebuffer
}

}  // namespace

void SetOverlay(const char* text) {
  if (strncmp(text, s_overlay_text, sizeof(s_overlay_text) - 1) == 0) return;
  strncpy(s_overlay_text, text, sizeof(s_overlay_text) - 1);
  s_overlay_dirty = true;
}

void Present(const uint32_t* argb, int width, int height, const char* overlay) {
  if (!s_xe || width <= 0 || height <= 0) return;
  if (!s_tex || s_tex_w != width || s_tex_h != height) {
    if (s_tex) Xe_DestroyTexture(s_xe, s_tex);
    s_tex = Xe_CreateTexture(s_xe, width, height, 1, XE_FMT_8888 | XE_FMT_ARGB, 0);
    s_tex->use_filtering = 1;
    s_tex->u_addressing = XE_TEXADDR_CLAMP;
    s_tex->v_addressing = XE_TEXADDR_CLAMP;
    s_tex_w = width;
    s_tex_h = height;
  }
  uint8_t* dst = (uint8_t*)Xe_Surface_LockRect(s_xe, s_tex, 0, 0, 0, 0, XE_LOCK_WRITE);
  for (int y = 0; y < height; y++) memcpy(dst + y * s_tex->wpitch, argb + y * width, width * 4);
  Xe_Surface_Unlock(s_xe, s_tex);
  if (overlay) SetOverlay(overlay);
  DrawPicture(s_tex, 0, 0, 1, 1);
}

// ---- GX rendering backend ----
namespace {

constexpr int EFB_W = XenosGx::EFB_W, EFB_H = XenosGx::EFB_H;
constexpr int VB_RING_BYTES = 8 << 20;
// libxenon kicks the GPU every 1024 command dwords, each kick using ~35 of
// the 8192 primary ring entries: the primary ring wraps after ~240K dwords
// and nothing checks that the GPU caught up. Sync well before that.
constexpr int SYNC_COMMAND_DWORDS = (512 << 10) / 4;

class XenosBackend : public Video::GpuBackend {
 public:
  struct XfbCopy {
    XenosSurface* surf = nullptr;
    u32 addr = 0;
    int sx = 0, sy = 0, w = 0, h = 0;
  };

  bool Init() {
    m_efb_copy = Xe_CreateTexture(s_xe, EFB_W, EFB_H, 1, XE_FMT_8888 | XE_FMT_ARGB, 1);
    m_efb_copy->use_filtering = 0;
    m_efb_copy->u_addressing = XE_TEXADDR_CLAMP;
    m_efb_copy->v_addressing = XE_TEXADDR_CLAMP;
    m_depth_copy = Xe_CreateTexture(s_xe, EFB_W, EFB_H, 1, XE_FMT_8888 | XE_FMT_ARGB, 1);
    m_vb = Xe_CreateVertexBuffer(s_xe, VB_RING_BYTES);
    m_restore_vb = Xe_CreateVertexBuffer(s_xe, 6 * 6 * sizeof(float));
    const float quad[6][6] = {
        {-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {-1, -1, 0, 1, 0, 1},
        {1, 1, 0, 1, 1, 0}, {1, -1, 0, 1, 1, 1}, {-1, -1, 0, 1, 0, 1},
    };
    void* v = Xe_VB_Lock(s_xe, m_restore_vb, 0, sizeof(quad), XE_LOCK_WRITE);
    memcpy(v, quad, sizeof(quad));
    Xe_VB_Unlock(s_xe, m_restore_vb);
    return m_efb_copy && m_vb;
  }

  void Draw(const Video::GpuDrawState& st, const Video::GpuVertex* v, u32 count) override {
    XenosGx::Prepare(st, v, count, m_pd);
    Submit(m_pd, st.tex);
  }

  void ClearEFB(int x0, int y0, int x1, int y1, bool color, bool alpha, bool depth, u32 rgba, u32 z24) override {
    XenosGx::PrepareClear(x0, y0, x1, y1, color, alpha, depth, rgba, z24, m_pd);
    Submit(m_pd, nullptr);
  }

  // Copies the EDRAM EFB to memory so it can be restored after a present.
  void SaveEFB() {
    Xe_ResolveInto(s_xe, m_efb_copy, XE_SOURCE_COLOR, 0);
    m_efb_copy_valid = true;
  }

  // XFB copies stay on the GPU: the EFB is resolved into a surface kept per
  // guest XFB address and presented directly by PresentXFB.
  bool CopyToXFB(u32 dest, int sx, int sy, int w, int h, int out_h) override {
    ActivateEFB();
    XfbCopy* slot = nullptr;
    for (XfbCopy& c : m_xfb)
      if (c.surf && c.addr == dest) slot = &c;
    if (!slot) {
      slot = &m_xfb[m_xfb_next];
      m_xfb_next = (m_xfb_next + 1) % kXfbSlots;
      if (!slot->surf) {
        slot->surf = Xe_CreateTexture(s_xe, EFB_W, EFB_H, 1, XE_FMT_8888 | XE_FMT_ARGB, 1);
        slot->surf->use_filtering = 1;
        slot->surf->u_addressing = XE_TEXADDR_CLAMP;
        slot->surf->v_addressing = XE_TEXADDR_CLAMP;
      }
    }
    slot->addr = dest;
    slot->sx = sx, slot->sy = sy, slot->w = w, slot->h = h;
    Xe_ResolveInto(s_xe, slot->surf, XE_SOURCE_COLOR, 0);
    m_xfb_last = slot;
    (void)out_h;
    // Present now: the game has finished the frame. Presenting later from the
    // VI (50/60 Hz, while the next frame is being drawn) would lose the EFB
    // depth, since the screen picture goes through the same EDRAM.
    Show(slot);
    return true;
  }

  void Show(XfbCopy* f) {
    DrawPicture(f->surf, (float)f->sx / EFB_W, (float)f->sy / EFB_H, (float)(f->sx + f->w) / EFB_W,
                (float)(f->sy + f->h) / EFB_H);
    m_xfb_shown = f;
  }

  // Frames are shown when copied (see CopyToXFB); the VI only needs to know
  // that the backend handles the display.
  bool PresentXFB(u32 addr, int width, int height) override {
    (void)addr;
    (void)width;
    (void)height;
    return m_xfb_last != nullptr;
  }

  void ReadEFBRect(int x0, int y0, int x1, int y1, bool color, bool depth, u32* cbuf, u32* zbuf) override {
    ActivateEFB();
    if (color) Xe_ResolveInto(s_xe, m_efb_copy, XE_SOURCE_COLOR, 0);
    if (depth) Xe_ResolveInto(s_xe, m_depth_copy, XE_SOURCE_DS, 0);
    Sync();
    const u32* src = color ? (const u32*)Xe_Surface_LockRect(s_xe, m_efb_copy, 0, 0, 0, 0, XE_LOCK_READ) : nullptr;
    const u32* zsrc = depth ? (const u32*)Xe_Surface_LockRect(s_xe, m_depth_copy, 0, 0, 0, 0, XE_LOCK_READ) : nullptr;
    int pitch = ((EFB_W + 31) >> 5) << 5;
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++) {
        int idx = (((y >> 5) * 32 * pitch + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2) + (((x & 31) >> 2) << 3) +
                    (((y & 31) >> 1) << 6)) ^
                   ((y & 8) << 2));
        if (src) {
          u32 argb = src[idx];
          cbuf[y * EFB_W + x] = (argb << 8) | (argb >> 24);
        }
        if (zsrc) zbuf[y * EFB_W + x] = zsrc[idx] >> 8;
      }
    if (zsrc) Xe_Surface_Unlock(s_xe, m_depth_copy);
    if (src) {
      Xe_Surface_Unlock(s_xe, m_efb_copy);
      m_efb_copy_valid = true;
    }
  }

  void ReadEFB(u32* color, u32* depth) override {
    ActivateEFB();
    Xe_ResolveInto(s_xe, m_efb_copy, XE_SOURCE_COLOR, 0);
    Xe_ResolveInto(s_xe, m_depth_copy, XE_SOURCE_DS, 0);
    Sync();
    const u32* src = (const u32*)Xe_Surface_LockRect(s_xe, m_efb_copy, 0, 0, 0, 0, XE_LOCK_READ);
    const u32* zsrc = (const u32*)Xe_Surface_LockRect(s_xe, m_depth_copy, 0, 0, 0, 0, XE_LOCK_READ);
    int pitch = ((EFB_W + 31) >> 5) << 5;
    for (int y = 0; y < EFB_H; y++)
      for (int x = 0; x < EFB_W; x++) {
        // 32 bpp tiled layout (same as the console framebuffer)
        int idx = (((y >> 5) * 32 * pitch + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2) + (((x & 31) >> 2) << 3) +
                    (((y & 31) >> 1) << 6)) ^
                   ((y & 8) << 2));
        u32 argb = src[idx];
        color[y * EFB_W + x] = (argb << 8) | (argb >> 24);  // -> 0xRRGGBBAA
        depth[y * EFB_W + x] = zsrc[idx] >> 8;               // D24S8: depth in the top 24 bits
      }
    Xe_Surface_Unlock(s_xe, m_depth_copy);
    Xe_Surface_Unlock(s_xe, m_efb_copy);
    m_efb_copy_valid = true;
  }

 private:
  struct GpuTex {
    XenosSurface* surf;
    u32 last_use;
  };

  void Sync() {
    Xe_Sync(s_xe);
    m_vb_offset = 0;
    m_commands_since_sync = 0;
  }

  // Makes the EFB the render target again (after a frame was presented),
  // restoring its color from the last read-back.
  void ActivateEFB() {
    if (s_efb_active) return;
    if (!m_efb_rt) {
      m_efb_rt = Xe_CreateTexture(s_xe, EFB_W, EFB_H, 1, XE_FMT_8888 | XE_FMT_ARGB, 1);
    }
    Xe_SetRenderTarget(s_xe, m_efb_rt);
    Xe_InvalidateState(s_xe);
    m_rs_valid = false;
    for (BoundTex& bt : m_bound) bt = BoundTex();
    Xe_SetCullMode(s_xe, XE_CULL_NONE);
    s_efb_active = true;
    if (m_efb_copy_valid) {
      Xe_SetZEnable(s_xe, 0);
      Xe_SetBlendControl(s_xe, XE_BLEND_ONE, XE_BLENDOP_ADD, XE_BLEND_ZERO, XE_BLEND_ONE, XE_BLENDOP_ADD, XE_BLEND_ZERO);
      Xe_SetScissor(s_xe, 0, -1, -1, -1, -1);
      Xe_SetShader(s_xe, SHADER_TYPE_PIXEL, s_blit_ps, 0);
      Xe_SetShader(s_xe, SHADER_TYPE_VERTEX, s_blit_vs, 0);
      Xe_SetStreamSource(s_xe, 0, m_restore_vb, 0, 6);
      Xe_SetTexture(s_xe, 0, m_efb_copy);
      Xe_DrawPrimitive(s_xe, XE_PRIMTYPE_TRIANGLELIST, 0, 2);
    }
  }

  XenosSurface* Texture(const Video::GpuTexture& t) {
    auto it = m_textures.find(t.id);
    if (it != m_textures.end()) {
      it->second.last_use = m_frame;
      return it->second.surf;
    }
    if (m_textures.size() > 512) EvictTextures();
    XenosSurface* surf = Xe_CreateTexture(s_xe, t.width, t.height, 1, XE_FMT_8888 | XE_FMT_ARGB, 0);
    u8* dst = (u8*)Xe_Surface_LockRect(s_xe, surf, 0, 0, 0, 0, XE_LOCK_WRITE);
    for (int y = 0; y < t.height; y++) {
      u32* row = (u32*)(dst + y * surf->wpitch);
      const u8* s = t.rgba + (size_t)y * t.width * 4;
      for (int x = 0; x < t.width; x++, s += 4) row[x] = ((u32)s[3] << 24) | ((u32)s[0] << 16) | ((u32)s[1] << 8) | s[2];
    }
    Xe_Surface_Unlock(s_xe, surf);
    m_textures[t.id] = {surf, m_frame};
    return surf;
  }

  void EvictTextures() {
    Sync();  // nothing may still read the surfaces
    for (auto& kv : m_textures) Xe_DestroyTexture(s_xe, kv.second.surf);
    m_textures.clear();
    for (BoundTex& bt : m_bound) bt = BoundTex();
  }

  static int Addressing(u32 gx_wrap) {
    switch (gx_wrap) {
      case 1: return XE_TEXADDR_WRAP;
      case 2: return XE_TEXADDR_MIRROR;
      default: return XE_TEXADDR_CLAMP;
    }
  }

  XenosShader* ShaderObject(XenosGx::ShaderEntry* e) {
    if (!e->backend_object) e->backend_object = CreateShader(e->shader);
    return (XenosShader*)e->backend_object;
  }

  void Submit(const XenosGx::PreparedDraw& pd, const Video::GpuTexture* tex) {
    if (!pd.vertex_count) return;
    ActivateEFB();
    u32 bytes = pd.vertex_count * pd.stride * 4;
    if (bytes > (u32)VB_RING_BYTES) return;
    if (m_vb_offset + bytes > (u32)VB_RING_BYTES || m_commands_since_sync > SYNC_COMMAND_DWORDS) Sync();
    void* dst = Xe_VB_Lock(s_xe, m_vb, m_vb_offset, bytes, XE_LOCK_WRITE);
    memcpy(dst, pd.vertices.data(), bytes);
    Xe_VB_Unlock(s_xe, m_vb);

    const XenosGx::RenderState& rs = pd.rs;
    // Render state: only when it differs from the previous draw (each libxenon
    // setter marks its register group for upload)
    if (!m_rs_valid || memcmp(&rs, &m_rs, sizeof(rs)) != 0) {
      Xe_SetZEnable(s_xe, rs.z_enable || rs.z_write);
      Xe_SetZFunc(s_xe, rs.z_enable ? rs.z_func : 7);
      Xe_SetZWrite(s_xe, rs.z_write);
      Xe_SetBlendControl(s_xe, rs.color_src, rs.color_op, rs.color_dst, rs.alpha_src, rs.alpha_op, rs.alpha_dst);
      Xe_SetScissor(s_xe, 1, rs.sc_left, rs.sc_top, rs.sc_right, rs.sc_bottom);
      m_rs = rs;
      m_rs_valid = true;
    }
    Xe_SetShader(s_xe, SHADER_TYPE_PIXEL, ShaderObject(pd.ps), 0);
    Xe_SetShader(s_xe, SHADER_TYPE_VERTEX, ShaderObject(pd.vs), 0);
    // constants: only the used range, and only when they changed
    u32 nconst = pd.ps->const_count ? pd.ps->const_count : XenosGx::NUM_PS_CONSTS;
    if (nconst > m_consts_valid || memcmp(m_consts, pd.ps_consts, nconst * 16) != 0) {
      Xe_SetPixelShaderConstantF(s_xe, 0, &pd.ps_consts[0][0], nconst);
      memcpy(m_consts, pd.ps_consts, nconst * 16);
      m_consts_valid = nconst;
    }
    for (u32 t = 0; t < 8; t++) {
      if (!(pd.tex_mask & (1u << t)) || !tex || !tex[t].rgba) continue;
      XenosSurface* surf = Texture(tex[t]);
      int filt = tex[t].linear ? 1 : 0, u = Addressing(tex[t].wrap_s), v = Addressing(tex[t].wrap_t);
      BoundTex& bt = m_bound[t];
      if (bt.surf == surf && bt.filt == filt && bt.u == u && bt.v == v) continue;
      surf->use_filtering = filt;
      surf->u_addressing = u;
      surf->v_addressing = v;
      Xe_SetTexture(s_xe, t, surf);
      bt = {surf, filt, u, v};
    }
    Xe_SetStreamSource(s_xe, 0, m_vb, m_vb_offset, pd.stride);
    int wptr = s_xe->rb_secondary_wptr;
    Xe_DrawPrimitive(s_xe, XE_PRIMTYPE_TRIANGLELIST, 0, pd.vertex_count / 3);
    Xe_pRBMayKick(s_xe);
    int used = s_xe->rb_secondary_wptr - wptr;
    m_commands_since_sync += used > 0 ? used : 64;
    m_vb_offset += (bytes + 31) & ~31u;
  }

  static constexpr int kXfbSlots = 3;
  XfbCopy m_xfb[kXfbSlots];
  int m_xfb_next = 0;
  XfbCopy* m_xfb_last = nullptr;
  XfbCopy* m_xfb_shown = nullptr;
  XenosGx::PreparedDraw m_pd;
  float m_consts[XenosGx::NUM_PS_CONSTS][4];
  XenosGx::RenderState m_rs;
  bool m_rs_valid = false;
  struct BoundTex {
    XenosSurface* surf = nullptr;
    int filt = -1, u = -1, v = -1;
  };
  BoundTex m_bound[8];
  u32 m_consts_valid = 0;  // leading constants known to be uploaded
  XenosSurface* m_efb_rt = nullptr;
  XenosSurface* m_efb_copy = nullptr;
  XenosSurface* m_depth_copy = nullptr;
  bool m_efb_copy_valid = false;
  XenosVertexBuffer* m_vb = nullptr;
  XenosVertexBuffer* m_restore_vb = nullptr;
  u32 m_vb_offset = 0;
  int m_commands_since_sync = 0;
  u32 m_frame = 0;
  std::unordered_map<u32, GpuTex> m_textures;
};

XenosBackend* s_backend = nullptr;

}  // namespace

void SaveEFBBeforePresent() {
  if (s_backend) s_backend->SaveEFB();
}

bool InstallBackend() {
  if (!s_xe) return false;
  s_backend = new XenosBackend();
  if (!s_backend->Init()) return false;
  Video::g_gpu = s_backend;
  return true;
}

}  // namespace XenosGpu
