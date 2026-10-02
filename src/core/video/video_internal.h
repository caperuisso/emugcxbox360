// emugcxbox360 - internals shared by the GX software pipeline
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/video/video.h"

namespace Video {

// ---- Register field helpers ----
inline u32 Bits(u32 v, u32 shift, u32 count) { return (v >> shift) & ((1u << count) - 1); }
inline float XFFloat(u32 addr) { return BitCast<float>(g_xf[addr]); }
inline const float* XFFloats(u32 addr) { return reinterpret_cast<const float*>(&g_xf[addr]); }

// BP registers
enum : u32 {
  BP_GENMODE = 0x00,
  BP_IND_MTX = 0x06,
  BP_IND_IMASK = 0x0F,
  BP_IND_CMD = 0x10,
  BP_SCISSOR_TL = 0x20,
  BP_SCISSOR_BR = 0x21,
  BP_LINEPTWIDTH = 0x22,
  BP_IREF = 0x27,
  BP_TREF = 0x28,
  BP_SU_SSIZE = 0x30,
  BP_ZMODE = 0x40,
  BP_BLENDMODE = 0x41,
  BP_CONSTANTALPHA = 0x42,
  BP_ZCOMPARE = 0x43,
  BP_EFB_TL = 0x49,
  BP_EFB_WH = 0x4A,
  BP_EFB_ADDR = 0x4B,
  BP_EFB_STRIDE = 0x4D,
  BP_COPY_YSCALE = 0x4E,
  BP_CLEAR_AR = 0x4F,
  BP_CLEAR_GB = 0x50,
  BP_CLEAR_Z = 0x51,
  BP_COPY_FILTER0 = 0x53,
  BP_SCISSOR_OFFSET = 0x59,
  BP_RAS1_SS0 = 0x25,
  BP_TEV_COLOR_ENV = 0xC0,
  BP_TEV_REGS = 0xE0,
  BP_FOG_RANGE = 0xE8,
  BP_FOG0 = 0xEE,
  BP_FOG_B_MAG = 0xEF,
  BP_FOG_B_EXP = 0xF0,
  BP_FOG3 = 0xF1,
  BP_FOG_COLOR = 0xF2,
  BP_ALPHA_COMPARE = 0xF3,
  BP_ZTEX1 = 0xF4,
  BP_ZTEX2 = 0xF5,
  BP_TEV_KSEL = 0xF6,
};

// XF registers
enum : u32 {
  XF_POS_MATRICES = 0x000,
  XF_NORMAL_MATRICES = 0x400,
  XF_POST_MATRICES = 0x500,
  XF_LIGHTS = 0x600,
  XF_CLIP_DISABLE = 0x1005,
  XF_INVTXSPEC = 0x1008,
  XF_NUM_CHAN = 0x1009,
  XF_AMB_COLOR = 0x100A,
  XF_MAT_COLOR = 0x100C,
  XF_COLOR_CHAN = 0x100E,
  XF_ALPHA_CHAN = 0x1010,
  XF_DUALTEX = 0x1012,
  XF_MATRIX_INDEX_A = 0x1018,
  XF_MATRIX_INDEX_B = 0x1019,
  XF_VIEWPORT = 0x101A,
  XF_PROJECTION = 0x1020,
  XF_NUM_TEXGEN = 0x103F,
  XF_TEXMTX_INFO = 0x1040,
  XF_POSTMTX_INFO = 0x1050,
};

// BP register address of texture unit n (0..7) for a given set (0x80, 0x84, ...)
inline u32 TexReg(u32 base, u32 n) { return base + (n & 3) + ((n & 4) ? 0x20 : 0); }

// ---- Vertices ----
struct Vec3 {
  float x, y, z;
};

struct InputVertex {
  Vec3 position;
  Vec3 normal[3];
  u8 color[2][4];  // RGBA
  float texcoords[8][2];
  u8 posmtx;
  u8 texmtx[8];
};

struct OutputVertex {
  Vec3 mv_position;      // eye space
  float proj[4];         // clip space x, y, z, w
  Vec3 screen;           // after perspective divide + viewport
  Vec3 normal[3];
  float color[2][4];     // RGBA 0..255
  Vec3 texcoords[8];
};

// transform.cpp
void TransformVertex(const InputVertex& in, OutputVertex& out);

// setup.cpp: primitive assembly, clipping, culling
void BeginPrimitive(u32 primitive);
void AddVertex(const OutputVertex& v);
void EndPrimitive();

// raster.cpp
void DrawTriangle(const OutputVertex* v0, const OutputVertex* v1, const OutputVertex* v2);
void UpdateScissor();

// texture.cpp
void SampleTexture(u32 texmap, s32 s, s32 t, s32 lod, bool linear, u8 out[4]);
void ComputeLOD(u32 texmap, float dsdx, float dsdy, float dtdx, float dtdy, s32& lod, bool& linear);
void LoadTLUT(u32 value);
void PreloadTMEM(u32 value);
void TextureReset();

// Save state pieces
void RasterDoState(StateBuffer& s);
void TextureDoState(StateBuffer& s);
void EFBDoState(StateBuffer& s);

// efb.cpp
void EFBReset();
bool EFBDepthTest(int x, int y, u32 z);
void EFBWriteDepth(int x, int y, u32 z);
void EFBBlend(int x, int y, const u8 rgba[4]);
void EFBCopy(u32 cmd);

}  // namespace Video
