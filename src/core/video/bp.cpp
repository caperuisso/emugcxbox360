// SPDX-License-Identifier: GPL-2.0-or-later
// BP register writes and their side effects.
#include "core/video/video_internal.h"

namespace Video {

void SetTevRegister(u32 bp_reg, u32 value);  // raster.cpp

void LoadBP(u32 value) {
  u32 reg = value >> 24;
  u32 data = value & 0x00FFFFFF;
  // The mask register (0xFE) applies to the next write only.
  if (reg != 0xFE && g_bp[0xFE] != 0x00FFFFFF) {
    data = (g_bp[reg] & ~g_bp[0xFE]) | (data & g_bp[0xFE]);
    g_bp[0xFE] = 0x00FFFFFF;
  }
  // TEV color registers have two banks (normal/konst) behind one address.
  if (reg >= BP_TEV_REGS && reg < BP_TEV_REGS + 8) {
    SetTevRegister(reg, data);
    g_bp[reg] = data;
    return;
  }
  g_bp[reg] = data;

  switch (reg) {
    case BP_SCISSOR_TL:
    case BP_SCISSOR_BR:
    case BP_SCISSOR_OFFSET:
      UpdateScissor();
      break;
    case 0x45:  // draw done
      SignalDrawDone();
      break;
    case 0x47:  // token with interrupt
      SignalToken((u16)data, true);
      break;
    case 0x48:  // token
      SignalToken((u16)data, false);
      break;
    case 0x52:  // EFB copy / clear
      EFBCopy(data);
      break;
    case 0x63:  // TMEM preload
      PreloadTMEM(data);
      break;
    case 0x65:  // TLUT load
      LoadTLUT(data);
      break;
    default:
      break;
  }
}

}  // namespace Video
