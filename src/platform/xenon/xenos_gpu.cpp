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

#include "platform/xenon/xenos_shaders.h"

namespace XenosGpu {

namespace {

XenosDevice s_xe_storage;
XenosDevice* s_xe = nullptr;
XenosSurface* s_fb = nullptr;
XenosShader* s_blit_vs = nullptr;
XenosShader* s_blit_ps = nullptr;
XenosSurface* s_tex = nullptr;
XenosVertexBuffer* s_vb = nullptr;
int s_tex_w = 0, s_tex_h = 0;

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
void DrawOverlay(uint32_t* pixels, int pitch_px, int w, int h, const char* text) {
  static const char kChars[] = "0123456789.% FPSD";
  static const uint16_t kGlyphs[] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF,
                                     0x7BCF, 0x0002, 0x52A5, 0x0000, 0x79E4, 0x7BE4, 0x79CF, 0x6B6E};
  const int scale = 2, ox = 8, oy = 8;
  int len = (int)strlen(text);
  for (int y = oy - 2; y < oy + 5 * scale + 2 && y < h; y++)
    for (int x = ox - 2; x < ox + len * 4 * scale + 2 && x < w; x++) pixels[y * pitch_px + x] = 0xFF000000;
  for (int c = 0; c < len; c++) {
    const char* pos = strchr(kChars, text[c]);
    if (!pos) continue;
    uint16_t g = kGlyphs[pos - kChars];
    for (int r = 0; r < 5; r++)
      for (int col = 0; col < 3; col++) {
        if (!(g & (1 << (14 - (r * 3 + col))))) continue;
        for (int dy = 0; dy < scale; dy++)
          for (int dx = 0; dx < scale; dx++) {
            int x = ox + (c * 4 + col) * scale + dx, y = oy + r * scale + dy;
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
  s_vb = Xe_CreateVertexBuffer(s_xe, 6 * 6 * sizeof(float));
  edram_init(s_xe);
  return true;
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

  // Upload the picture
  uint8_t* dst = (uint8_t*)Xe_Surface_LockRect(s_xe, s_tex, 0, 0, 0, 0, XE_LOCK_WRITE);
  for (int y = 0; y < height; y++) memcpy(dst + y * s_tex->wpitch, argb + y * width, width * 4);
  if (overlay && overlay[0]) DrawOverlay((uint32_t*)dst, s_tex->wpitch / 4, width, height, overlay);
  Xe_Surface_Unlock(s_xe, s_tex);

  // 4:3 picture fitted to the screen height, centred (normalized device coordinates)
  float sw = (float)s_fb->width, sh = (float)s_fb->height;
  float dh = sh, dw = sh * 4.0f / 3.0f;
  if (dw > sw) {
    dw = sw;
    dh = sw * 3.0f / 4.0f;
  }
  float x0 = -dw / sw, x1 = dw / sw, y0 = dh / sh, y1 = -dh / sh;
  const float quad[6][6] = {
      {x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1},
      {x1, y0, 0, 1, 1, 0}, {x1, y1, 0, 1, 1, 1}, {x0, y1, 0, 1, 0, 1},
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
  Xe_SetTexture(s_xe, 0, s_tex);
  Xe_DrawPrimitive(s_xe, XE_PRIMTYPE_TRIANGLELIST, 0, 2);
  Xe_Resolve(s_xe);
  Xe_Sync(s_xe);
}

}  // namespace XenosGpu
