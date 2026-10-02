// SPDX-License-Identifier: GPL-2.0-or-later
// MMIO dispatch: maps 0xCC00xxxx physical addresses to hardware blocks.
#include "core/hw/hw.h"

#include "core/gekko/cpu.h"

namespace HW {

namespace {
u8 s_gather[64];
u32 s_gather_len = 0;

enum Block { CP, PE, VI_, PI_, MI_, DSP_, DI_, SI_, EXI_, AI_, NONE };

Block Decode(u32 pa, u32& off) {
  u32 a = pa & 0xFFFF;
  switch (a >> 12) {
    case 0x0: off = a & 0xFFF; return CP;
    case 0x1: off = a & 0xFFF; return PE;
    case 0x2: off = a & 0xFFF; return VI_;
    case 0x3: off = a & 0xFFF; return PI_;
    case 0x4: off = a & 0xFFF; return MI_;
    case 0x5: off = a & 0x1FF; return DSP_;
    case 0x6:
      off = a & 0x3FF;
      switch ((a >> 10) & 3) {
        case 0: return DI_;
        case 1: return SI_;
        case 2: return EXI_;
        default: return AI_;
      }
    default: return NONE;
  }
}

bool Is32Bit(Block b) { return b == PI_ || b == DI_ || b == SI_ || b == EXI_ || b == AI_; }

u16 Read16Native(Block b, u32 off) {
  switch (b) {
    case CP: return GX::CPRead16(off);
    case PE: return GX::PERead16(off);
    case VI_: return VI::Read16(off);
    case MI_: return MI::Read16(off);
    case DSP_: return DSP::Read16(off);
    default: return 0;
  }
}

void Write16Native(Block b, u32 off, u16 v) {
  switch (b) {
    case CP: GX::CPWrite16(off, v); break;
    case PE: GX::PEWrite16(off, v); break;
    case VI_: VI::Write16(off, v); break;
    case MI_: MI::Write16(off, v); break;
    case DSP_: DSP::Write16(off, v); break;
    default: break;
  }
}

u32 Read32Native(Block b, u32 off) {
  switch (b) {
    case PI_: return PI::Read32(off);
    case DI_: return DI::Read32(off);
    case SI_: return SI::Read32(off);
    case EXI_: return EXI::Read32(off);
    case AI_: return AI::Read32(off);
    default: return 0;
  }
}

void Write32Native(Block b, u32 off, u32 v) {
  switch (b) {
    case PI_: PI::Write32(off, v); break;
    case DI_: DI::Write32(off, v); break;
    case SI_: SI::Write32(off, v); break;
    case EXI_: EXI::Write32(off, v); break;
    case AI_: AI::Write32(off, v); break;
    default: break;
  }
}

void Unmapped(const char* what, u32 pa) {
  static int n = 0;
  if (n++ < 32) LOG("HW: unmapped %s @%08x (pc=%08x)\n", what, pa, cpu.pc);
}
}  // namespace

void Init() {
  VI::Init();
  EXI::Init();
  DSP::Init();
  AI::Init();
}

void Reset() {
  s_gather_len = 0;
  PI::Reset();
  VI::Reset();
  SI::Reset();
  EXI::Reset();
  DI::Reset();
  DSP::Reset();
  AI::Reset();
  MI::Reset();
  GX::Reset();
}

u16 Read16(u32 pa) {
  u32 off;
  Block b = Decode(pa, off);
  if (b == NONE) {
    Unmapped("read16", pa);
    return 0;
  }
  if (Is32Bit(b)) {
    u32 v = Read32Native(b, off & ~3u);
    return (off & 2) ? (u16)v : (u16)(v >> 16);
  }
  return Read16Native(b, off);
}

u32 Read32(u32 pa) {
  u32 off;
  Block b = Decode(pa, off);
  if (b == NONE) {
    Unmapped("read32", pa);
    return 0;
  }
  if (Is32Bit(b)) return Read32Native(b, off);
  return ((u32)Read16Native(b, off) << 16) | Read16Native(b, off + 2);
}

void Write16(u32 pa, u16 v) {
  u32 off;
  Block b = Decode(pa, off);
  if (b == NONE) {
    Unmapped("write16", pa);
    return;
  }
  if (Is32Bit(b)) {
    // Rare: merge the halfword into the current register value.
    u32 cur = Read32Native(b, off & ~3u);
    u32 nv = (off & 2) ? ((cur & 0xFFFF0000u) | v) : ((cur & 0xFFFFu) | ((u32)v << 16));
    Write32Native(b, off & ~3u, nv);
    return;
  }
  Write16Native(b, off, v);
}

void Write32(u32 pa, u32 v) {
  u32 off;
  Block b = Decode(pa, off);
  if (b == NONE) {
    Unmapped("write32", pa);
    return;
  }
  if (Is32Bit(b)) {
    Write32Native(b, off, v);
    return;
  }
  Write16Native(b, off, (u16)(v >> 16));
  Write16Native(b, off + 2, (u16)v);
}

void Write8(u32 pa, u8 v) {
  u32 off;
  Block b = Decode(pa, off);
  if (b == NONE) {
    Unmapped("write8", pa);
    return;
  }
  u32 shift = (3 - (off & 3)) * 8;
  u32 cur = Read32(pa & ~3u);
  u32 nv = (cur & ~(0xFFu << shift)) | ((u32)v << shift);
  if (Is32Bit(b)) {
    Write32Native(b, off & ~3u, nv);
  } else {
    u32 o = off & ~1u;
    u16 half = (off & 2) ? (u16)nv : (u16)(nv >> 16);
    Write16Native(b, o, half);
  }
}

void WriteGatherPipe(const u8* data, u32 len) {
  memcpy(s_gather + s_gather_len, data, len);
  s_gather_len += len;
  while (s_gather_len >= 32) {
    PI::FifoWriteBlock(s_gather);
    s_gather_len -= 32;
    memmove(s_gather, s_gather + 32, s_gather_len);
  }
}

void ResetGatherPipe() { s_gather_len = 0; }

u32 GatherPipeBytes() { return s_gather_len; }

}  // namespace HW
