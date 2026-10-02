// emugcxbox360 - fixed shaders of the Xenos backend
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "xenos/ucode.h"

namespace Xenos {

struct BuiltShader {
  std::vector<uint32_t> code;
  uint32_t program_control;
  uint32_t context_misc;
};

// Screen blit: vertices are {x, y, z, w, u, v} floats (stream 0, vf95);
// position goes to oPos, uv to interpolator 0.
inline BuiltShader BlitVertexShader() {
  ShaderBuilder b(false);
  b.VFetch(1, 0, 95, VFMT_32_32_32_32_FLOAT, 0, 6);
  b.VFetch(2, 0, 95, VFMT_32_32_FLOAT, 4, 6, FETCH_XY01);
  b.UseTemps(3);
  b.BeginPhase(PHASE_POSITION);
  b.MovExport(EXPORT_POSITION, 0xF, Src::R(1));
  b.BeginPhase(PHASE_INTERP);
  b.MovExport(0, 0xF, Src::R(2));
  return {b.Build(), VertexProgramControl(b.TempCount(), 1), 0};
}

// oC0 = texture 0 at interpolator 0's uv
inline BuiltShader BlitPixelShader() {
  ShaderBuilder b(true);
  b.TFetch2D(0, 0, 0);
  b.BeginPhase(PHASE_INTERP);
  b.MovExport(EXPORT_COLOR0, 0xF, Src::R(0));
  return {b.Build(), PixelProgramControl(b.TempCount()), 4};
}

}  // namespace Xenos
