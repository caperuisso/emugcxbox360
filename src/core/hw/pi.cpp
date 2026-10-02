// SPDX-License-Identifier: GPL-2.0-or-later
// Processor Interface (interrupt controller, CPU FIFO) and Memory Interface stub.
#include "core/gekko/cpu.h"
#include "core/hw/hw.h"
#include "core/memory.h"
#include "core/state.h"

namespace PI {

namespace {
u32 s_cause;
u32 s_mask;
u32 s_fifo_base, s_fifo_end, s_fifo_wptr;
u32 s_reset_code;

void Update() { CPU::SetExternalInterrupt((s_cause & s_mask) != 0); }
}  // namespace

void DoState(StateBuffer& s) {
  s.Marker("PI");
  s.Do(s_cause);
  s.Do(s_mask);
  s.Do(s_fifo_base);
  s.Do(s_fifo_end);
  s.Do(s_fifo_wptr);
  s.Do(s_reset_code);
}

void Reset() {
  s_cause = RESET_SWITCH_STATE;  // reset button released
  s_mask = 0;
  s_fifo_base = s_fifo_end = s_fifo_wptr = 0;
  s_reset_code = 0;
  Update();
}

void SetInterrupt(u32 cause, bool active) {
  if (active)
    s_cause |= cause;
  else
    s_cause &= ~cause;
  Update();
}

u32 Read32(u32 off) {
  switch (off) {
    case 0x00: return s_cause;
    case 0x04: return s_mask;
    case 0x0C: return s_fifo_base;
    case 0x10: return s_fifo_end;
    case 0x14: return s_fifo_wptr;
    case 0x24: return s_reset_code;
    case 0x2C: return 0x246500B1;  // Flipper revision C
    default: return 0;
  }
}

void Write32(u32 off, u32 v) {
  switch (off) {
    case 0x00:
      // Only latched causes are acknowledged here; device lines are level driven.
      s_cause &= ~(v & (INT_ERROR | INT_RSW | INT_DEBUG | INT_HSP));
      Update();
      break;
    case 0x04:
      s_mask = v;
      Update();
      break;
    case 0x0C: s_fifo_base = v & 0x03FFFFE0; break;
    case 0x10: s_fifo_end = v & 0x03FFFFE0; break;
    case 0x14: s_fifo_wptr = v & 0x03FFFFE0; break;
    case 0x24: s_reset_code = v; break;
    default: break;
  }
}

void FifoWriteBlock(const u8* block) {
  if (u8* dst = Mem::PhysPtr(s_fifo_wptr & 0x03FFFFE0, 32)) {
    memcpy(dst, block, 32);
    Mem::MarkWritten(s_fifo_wptr & 0x03FFFFE0, 32);
  }
  u32 ptr = (s_fifo_wptr & 0x03FFFFE0) + 32;
  u32 wrap = s_fifo_wptr & 0x20000000;
  if (s_fifo_end && ptr > s_fifo_end) {
    ptr = s_fifo_base;
    wrap = 0x20000000;
  }
  s_fifo_wptr = ptr | wrap;
  GX::ProcessFifo(block, 32);
}

}  // namespace PI

namespace MI {

namespace {
u16 s_regs[0x80];
}

void Reset() { memset(s_regs, 0, sizeof(s_regs)); }
void DoState(StateBuffer& s) {
  s.Marker("MI");
  s.Do(s_regs);
}
u16 Read16(u32 off) { return off < 0x100 ? s_regs[off >> 1] : 0; }
void Write16(u32 off, u16 v) {
  if (off < 0x100) s_regs[off >> 1] = v;
}

}  // namespace MI
