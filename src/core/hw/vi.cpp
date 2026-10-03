// SPDX-License-Identifier: GPL-2.0-or-later
// Video Interface: beam counter, display interrupts and XFB scan-out.
#include <algorithm>
#include <vector>

#include "core/coretiming.h"
#include "core/hw/hw.h"
#include "core/memory.h"
#include "core/video/gpu_backend.h"
#include "core/state.h"
#include "platform/platform.h"

namespace VI {

bool g_field_done = false;

namespace {
enum Reg : u32 {
  VTR = 0x00,
  DCR = 0x02,
  TFBL_HI = 0x1C,
  TFBL_LO = 0x1E,
  BFBL_HI = 0x24,
  BFBL_LO = 0x26,
  DPV = 0x2C,
  DPH = 0x2E,
  DI0_HI = 0x30,
  PICCONF = 0x48,
  VICLK = 0x6C,
};

u16 s_regs[0x40];
bool s_di_status[4];
u32 s_line;  // 1-based beam position within the frame
int s_line_event = -1;
std::vector<u32> s_frame;

u16 R(u32 off) { return s_regs[off >> 1]; }

bool IsPAL() { return ((R(DCR) >> 8) & 3) == 1; }
bool IsProgressive() { return R(VICLK) & 1; }
bool IsNonInterlaced() { return (R(DCR) >> 2) & 1; }

// Lines counted by the beam in one full VI cycle, and that cycle's duration.
u32 LinesPerCycle() {
  u32 lines = IsPAL() ? 625 : 525;
  if (IsNonInterlaced() && !IsProgressive()) lines = (lines + 1) / 2;
  return lines;
}
u32 CyclesPerLine() {
  // Interlaced: one cycle = two fields = 1/30 s (1/25 PAL). Otherwise 1/60 (1/50).
  bool two_fields = !IsNonInterlaced() && !IsProgressive();
  u32 fields_per_sec = IsPAL() ? 50 : 60;
  u64 cycle = (u64)CPU_CLOCK * (two_fields ? 2 : 1) / fields_per_sec;
  return (u32)(cycle / LinesPerCycle());
}

u32 FrameBufferAddr(u32 hi_off) {
  u32 v = ((u32)R(hi_off) << 16) | R(hi_off + 2);
  u32 addr = v & 0x00FFFFFF;
  if (v & 0x10000000) addr <<= 5;  // POFF: page offset mode
  return addr;
}

inline u8 Clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (u8)v); }

void PresentXFB() {
  if (!g_host) return;
  u32 wpl = (R(PICCONF) >> 8) & 0x7F;  // width in 16-pixel units
  u32 std = R(PICCONF) & 0xFF;         // stride in 16-pixel units
  u32 acv = (R(VTR) >> 4) & 0x3FF;     // active lines per field
  u32 width = wpl * 16, stride_px = std * 16;
  bool interlaced = !IsNonInterlaced() && !IsProgressive();
  u32 height = interlaced ? acv * 2 : acv;
  if (width == 0 || height == 0 || width > 720 || height > 576) {
    width = 640;
    height = 480;
    stride_px = 640;
    interlaced = false;
  }
  if (stride_px == 0) stride_px = width;
  u32 top = FrameBufferAddr(TFBL_HI), bottom = FrameBufferAddr(BFBL_HI);

  s_frame.resize((size_t)width * height);
  // Until the game enables the VI and points it at a framebuffer, show black
  // rather than scanning out whatever sits at physical address 0.
  if (!(R(DCR) & 1) || top == 0) {
    std::fill(s_frame.begin(), s_frame.end(), 0xFF000000u);
    g_host->PresentFrame(s_frame.data(), (int)width, (int)height);
    return;
  }
  // A GPU backend may present the copy itself (no YUV round trip)
  if (Video::g_gpu && Video::g_gpu->PresentXFB(top, (int)width, (int)height)) return;
  for (u32 y = 0; y < height; y++) {
    u32 line_addr;
    if (interlaced)
      line_addr = ((y & 1) ? bottom : top) + (y >> 1) * stride_px * 2;
    else
      line_addr = top + y * stride_px * 2;
    const u8* src = Mem::PhysPtr(line_addr, width * 2);
    u32* dst = &s_frame[(size_t)y * width];
    if (!src) {
      memset(dst, 0, width * 4);
      continue;
    }
    // YUYV 4:2:2 -> RGB (BT.601, studio range)
    for (u32 x = 0; x < width; x += 2, src += 4) {
      int y0 = src[0] - 16, u = src[1] - 128, y1 = src[2] - 16, v = src[3] - 128;
      int rv = 409 * v, gu = -100 * u - 208 * v, bu = 516 * u;
      int c0 = 298 * y0 + 128, c1 = 298 * y1 + 128;
      dst[x] = 0xFF000000u | (Clamp8((c0 + rv) >> 8) << 16) | (Clamp8((c0 + gu) >> 8) << 8) |
               Clamp8((c0 + bu) >> 8);
      dst[x + 1] = 0xFF000000u | (Clamp8((c1 + rv) >> 8) << 16) | (Clamp8((c1 + gu) >> 8) << 8) |
                   Clamp8((c1 + bu) >> 8);
    }
  }
  g_host->PresentFrame(s_frame.data(), (int)width, (int)height);
}

void UpdateInterrupt() {
  bool active = false;
  for (int i = 0; i < 4; i++) {
    bool enabled = (R(DI0_HI + i * 4) >> 12) & 1;
    if (s_di_status[i] && enabled) active = true;
  }
  PI::SetInterrupt(PI::INT_VI, active);
}

void FieldDone() {
  PresentXFB();
  SI::UpdatePolling();
  g_field_done = true;
}

void LineCallback(u64, s64 late) {
  u32 lines = LinesPerCycle();
  s_line++;
  if (s_line > lines) s_line = 1;

  bool changed = false;
  for (int i = 0; i < 4; i++) {
    u32 vct = R(DI0_HI + i * 4) & 0x3FF;
    if (vct == s_line) {
      s_di_status[i] = true;
      changed = true;
    }
  }
  if (changed) UpdateInterrupt();

  bool two_fields = !IsNonInterlaced() && !IsProgressive();
  if (s_line == 1 || (two_fields && s_line == lines / 2 + 1)) FieldDone();

  CoreTiming::ScheduleEvent(s_line_event, (s64)CyclesPerLine() - late);
}
}  // namespace

void DoState(StateBuffer& s) {
  s.Marker("VI");
  s.Do(s_regs);
  s.Do(s_di_status);
  s.Do(s_line);
}

void Init() { s_line_event = CoreTiming::RegisterEvent("VI line", LineCallback); }

void Reset() {
  memset(s_regs, 0, sizeof(s_regs));
  memset(s_di_status, 0, sizeof(s_di_status));
  // NTSC 640x480 interlaced timings. Scan-out stays disabled until boot code
  // calls SetBootTVMode, as the IPL would.
  s_regs[VTR >> 1] = (240 << 4) | 6;
  s_regs[DCR >> 1] = 0;
  s_regs[PICCONF >> 1] = (40 << 8) | 40;
  s_line = 1;
  g_field_done = false;
  CoreTiming::RemoveEvent(s_line_event);
  CoreTiming::ScheduleEvent(s_line_event, CyclesPerLine());
}

// Register state the IPL leaves behind (values from Dolphin's VideoInterface::Preset).
// The SDK relies on it: VI mode changes are only applied from the retrace
// interrupt handler, so DI0/DI1 must already be armed when the game starts.
void SetBootTVMode(bool pal) {
  auto set32 = [](u32 off, u32 v) {
    s_regs[off >> 1] = (u16)(v >> 16);
    s_regs[(off >> 1) + 1] = (u16)v;
  };
  s_regs[VTR >> 1] = 6;                       // EQU=6, ACV=0
  s_regs[DCR >> 1] = 1 | (pal ? 0x100 : 0);   // enabled, interlaced, NTSC/PAL
  set32(0x04, (71u << 24) | (105u << 16) | 429u);       // HTR0: HCS, HCE, HLW
  set32(0x08, 64u | (162u << 7) | (373u << 17));        // HTR1: HSY, HBE640, HBS640
  set32(0x0C, (5u << 16) | 502u);                       // VTO: PSB, PRB
  set32(0x10, (4u << 16) | 503u);                       // VTE
  set32(0x14, 12u | (520u << 5) | (12u << 16) | (520u << 21));  // BBOI
  set32(0x18, 13u | (519u << 5) | (13u << 16) | (519u << 21));  // BBEI
  set32(0x1C, 0);                                       // no framebuffer yet
  set32(0x24, 0);
  set32(DI0_HI, (1u << 28) | (263u << 16) | 430u);      // DI0: enabled, line 263
  set32(DI0_HI + 4, (1u << 28) | (1u << 16) | 1u);      // DI1: enabled, line 1
  set32(DI0_HI + 8, 0);
  set32(DI0_HI + 12, 0);
  s_regs[PICCONF >> 1] = (40 << 8) | 40;
  memset(s_di_status, 0, sizeof(s_di_status));
  UpdateInterrupt();
}

u16 Read16(u32 off) {
  if (off >= 0x80) return 0;
  switch (off) {
    case DPV: return (u16)s_line;
    case DPH: return 1;
    case DI0_HI:
    case DI0_HI + 4:
    case DI0_HI + 8:
    case DI0_HI + 12: {
      int i = (off - DI0_HI) / 4;
      return (R(off) & 0x7FFF) | (s_di_status[i] ? 0x8000 : 0);
    }
    default: return R(off);
  }
}

void Write16(u32 off, u16 v) {
  if (off >= 0x80) return;
  switch (off) {
    case DI0_HI:
    case DI0_HI + 4:
    case DI0_HI + 8:
    case DI0_HI + 12: {
      int i = (off - DI0_HI) / 4;
      if (!(v & 0x8000)) s_di_status[i] = false;
      s_regs[off >> 1] = v & 0x7FFF;
      UpdateInterrupt();
      return;
    }
    case DPV:
    case DPH: return;
    default: s_regs[off >> 1] = v; return;
  }
}

}  // namespace VI
