// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/memory.h"

#include <cstdlib>

#include "core/gekko/cpu.h"
#include "core/hw/hw.h"
#include "core/video/video.h"

namespace Mem {

u8* g_mem1 = nullptr;
u8 g_l2[L2_SIZE];

namespace {
// One entry per 128 KiB block of the 4 GiB effective space.
// Value: physical block base | 1 when valid, 0 when unmapped.
constexpr u32 BAT_BLOCKS = 1u << 15;
u32 s_dbat[BAT_BLOCKS];
u32 s_ibat[BAT_BLOCKS];

void BuildTable(u32* table, int first_spr) {
  memset(table, 0, sizeof(u32) * BAT_BLOCKS);
  for (int i = 0; i < 4; i++) {
    u32 upper = cpu.spr[first_spr + i * 2];
    u32 lower = cpu.spr[first_spr + i * 2 + 1];
    if (!(upper & 3)) continue;  // neither Vs nor Vp
    u32 bl = (upper >> 2) & 0x7FF;
    u32 bepi = (upper >> 17) & ~bl;
    u32 brpn = (lower >> 17) & ~bl;
    for (u32 k = 0; k <= bl; k++) {
      if (k & ~bl) continue;
      table[(bepi | k) & (BAT_BLOCKS - 1)] = (((brpn | k) << 17)) | 1;
    }
  }
}

inline bool Translate(const u32* table, bool relocate, u32 ea, u32& pa) {
  if (!relocate) {
    pa = ea;
    return true;
  }
  u32 e = table[ea >> 17];
  if (UNLIKELY(!e)) return false;
  pa = (e & ~1u) | (ea & 0x1FFFF);
  return true;
}

void DataFault(u32 ea, bool store) {
  cpu.spr[SPR_DAR] = ea;
  cpu.spr[SPR_DSISR] = (1u << 30) | (store ? (1u << 25) : 0);  // "no translation"
  CPU::RaiseException(EXC_DSI);
}

inline u8* RamPtr(u32 pa) {
  if (pa < MEM1_SIZE) return g_mem1 + pa;
  if ((pa & ~(L2_SIZE - 1)) == L2_BASE) return g_l2 + (pa & (L2_SIZE - 1));
  return nullptr;
}

inline bool IsMMIO(u32 pa) { return (pa & 0xFFFF0000) == MMIO_BASE; }

template <typename T>
T ReadPhys(u32 pa) {
  if (u8* p = RamPtr(pa)) {
    if (sizeof(T) == 1) return *p;
    if (sizeof(T) == 2) return (T)LoadBE16(p);
    if (sizeof(T) == 4) return (T)LoadBE32(p);
    return (T)LoadBE64(p);
  }
  if (IsMMIO(pa)) {
    if (sizeof(T) == 1) return (T)(HW::Read16(pa & ~1u) >> ((pa & 1) ? 0 : 8));
    if (sizeof(T) == 2) return (T)HW::Read16(pa);
    if (sizeof(T) == 4) return (T)HW::Read32(pa);
    return (T)(((u64)HW::Read32(pa) << 32) | HW::Read32(pa + 4));
  }
  if ((pa & 0xFF000000) == 0x08000000) {  // EFB peek
    u32 x = (pa & 0xFFF) >> 2, y = (pa >> 12) & 0x3FF;
    u32 v = (pa & 0x400000) ? Video::PeekEFBDepth(x, y) : Video::PeekEFBColor(x, y);
    return (T)v;
  }
  static int warn = 0;
  if (warn++ < 32) LOG("Mem: unmapped read%d @%08x (pc=%08x)\n", (int)sizeof(T) * 8, pa, cpu.pc);
  return 0;
}

template <typename T>
void WritePhys(u32 pa, T v) {
  if (u8* p = RamPtr(pa)) {
    if (sizeof(T) == 1) *p = (u8)v;
    else if (sizeof(T) == 2) StoreBE16(p, (u16)v);
    else if (sizeof(T) == 4) StoreBE32(p, (u32)v);
    else StoreBE64(p, (u64)v);
    return;
  }
  if (IsMMIO(pa)) {
    if ((pa & 0xFFFFF000) == WGP_ADDR) {
      u8 buf[8];
      if (sizeof(T) == 1) buf[0] = (u8)v;
      else if (sizeof(T) == 2) StoreBE16(buf, (u16)v);
      else if (sizeof(T) == 4) StoreBE32(buf, (u32)v);
      else StoreBE64(buf, (u64)v);
      HW::WriteGatherPipe(buf, sizeof(T));
      return;
    }
    if (sizeof(T) == 1) HW::Write8(pa, (u8)v);
    else if (sizeof(T) == 2) HW::Write16(pa, (u16)v);
    else if (sizeof(T) == 4) HW::Write32(pa, (u32)v);
    else {
      HW::Write32(pa, (u32)((u64)v >> 32));
      HW::Write32(pa + 4, (u32)v);
    }
    return;
  }
  if ((pa & 0xFF000000) == 0x08000000) {  // EFB poke
    u32 x = (pa & 0xFFF) >> 2, y = (pa >> 12) & 0x3FF;
    if (pa & 0x400000)
      Video::PokeEFBDepth(x, y, (u32)v);
    else
      Video::PokeEFBColor(x, y, (u32)v);
    return;
  }
  static int warn = 0;
  if (warn++ < 32) LOG("Mem: unmapped write%d @%08x (pc=%08x)\n", (int)sizeof(T) * 8, pa, cpu.pc);
}

template <typename T>
T ReadEA(u32 ea) {
  u32 pa;
  if (UNLIKELY(!Translate(s_dbat, cpu.msr & MSR_DR, ea, pa))) {
    DataFault(ea, false);
    return 0;
  }
  return ReadPhys<T>(pa);
}

template <typename T>
void WriteEA(u32 ea, T v) {
  u32 pa;
  if (UNLIKELY(!Translate(s_dbat, cpu.msr & MSR_DR, ea, pa))) {
    DataFault(ea, true);
    return;
  }
  WritePhys<T>(pa, v);
}
}  // namespace

bool Init() {
  if (!g_mem1) g_mem1 = (u8*)malloc(MEM1_SIZE);
  if (!g_mem1) return false;
  Clear();
  return true;
}

void Shutdown() {
  free(g_mem1);
  g_mem1 = nullptr;
}

void Clear() {
  memset(g_mem1, 0, MEM1_SIZE);
  memset(g_l2, 0, sizeof(g_l2));
}

void UpdateBATs() {
  BuildTable(s_ibat, SPR_IBAT0U);
  BuildTable(s_dbat, SPR_DBAT0U);
}

u8 Read8(u32 ea) { return ReadEA<u8>(ea); }
u16 Read16(u32 ea) { return ReadEA<u16>(ea); }
u32 Read32(u32 ea) { return ReadEA<u32>(ea); }
u64 Read64(u32 ea) { return ReadEA<u64>(ea); }
void Write8(u32 ea, u8 v) { WriteEA<u8>(ea, v); }
void Write16(u32 ea, u16 v) { WriteEA<u16>(ea, v); }
void Write32(u32 ea, u32 v) { WriteEA<u32>(ea, v); }
void Write64(u32 ea, u64 v) { WriteEA<u64>(ea, v); }

bool TranslateData(u32 ea, u32& pa) { return Translate(s_dbat, cpu.msr & MSR_DR, ea, pa); }

u32 ReadInstr(u32 ea) {
  u32 pa;
  if (UNLIKELY(!Translate(s_ibat, cpu.msr & MSR_IR, ea, pa))) {
    CPU::RaiseException(EXC_ISI);
    return 0;
  }
  if (LIKELY(pa < MEM1_SIZE)) return LoadBE32(g_mem1 + pa);
  if (u8* p = RamPtr(pa)) return LoadBE32(p);
  CPU::RaiseException(EXC_ISI);
  return 0;
}

u8* PhysPtr(u32 pa, u32 len) {
  if ((pa & ~(L2_SIZE - 1)) == L2_BASE)
    return (pa & (L2_SIZE - 1)) + len <= L2_SIZE ? g_l2 + (pa & (L2_SIZE - 1)) : nullptr;
  pa &= 0x1FFFFFFF;  // accept cached/uncached virtual mirrors too
  if ((u64)pa + len <= MEM1_SIZE) return g_mem1 + pa;
  return nullptr;
}

}  // namespace Mem
