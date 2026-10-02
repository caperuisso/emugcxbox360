// emugcxbox360 - guest memory: MEM1, locked L2 cache, BAT translation, MMIO routing
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/common.h"

namespace Mem {

constexpr u32 MEM1_SIZE = 0x01800000;  // 24 MiB "Splash" 1T-SRAM
constexpr u32 MEM1_MASK = 0x01FFFFFF;
constexpr u32 L2_BASE = 0xE0000000;    // locked cache (16 KiB)
constexpr u32 L2_SIZE = 0x4000;
constexpr u32 MMIO_BASE = 0x0C000000;  // Flipper hardware registers
constexpr u32 WGP_ADDR = 0x0C008000;   // write-gather pipe (GX FIFO)

extern u8* g_mem1;
extern u8 g_l2[L2_SIZE];

void DoState(StateBuffer& s);
bool Init();
void Shutdown();
void Clear();

// Rebuild the 128 KiB block translation tables from the BAT SPRs.
void UpdateBATs();

// Effective-address accesses (data side). On a translation miss a DSI is raised.
u8 Read8(u32 ea);
u16 Read16(u32 ea);
u32 Read32(u32 ea);
u64 Read64(u32 ea);
void Write8(u32 ea, u8 v);
void Write16(u32 ea, u16 v);
void Write32(u32 ea, u32 v);
void Write64(u32 ea, u64 v);

// Instruction fetch (raises ISI on miss).
u32 ReadInstr(u32 ea);

// Translate an effective data address; returns false when unmapped.
bool TranslateData(u32 ea, u32& pa);
// Same for instruction fetches (IBATs, MSR.IR).
bool TranslateInstr(u32 ea, u32& pa);

// Direct pointer into RAM for DMA from hardware (physical address), or nullptr.
u8* PhysPtr(u32 pa, u32 len);

// Write tracking for caches of decoded guest data (textures). Every write to
// MEM1 stamps its 4 KiB page with the current g_write_stamp; a cache entry
// created with NewStamp() is stale once one of its pages has a stamp >= its own.
constexpr u32 PAGE_SHIFT = 12;
extern u32* g_page_stamp;
extern u32 g_write_stamp;
inline void MarkWritten(u32 pa, u32 len) {
  pa &= MEM1_MASK;
  if (!len || pa >= MEM1_SIZE) return;
  u32 end = pa + len > MEM1_SIZE ? MEM1_SIZE : pa + len;
  for (u32 page = pa >> PAGE_SHIFT; page <= (end - 1) >> PAGE_SHIFT; page++) g_page_stamp[page] = g_write_stamp;
}
inline u32 NewStamp() { return ++g_write_stamp; }
// True when no page of [pa, pa+len) was written since `stamp` was issued.
inline bool UnchangedSince(u32 pa, u32 len, u32 stamp) {
  pa &= MEM1_MASK;
  if (!len || pa >= MEM1_SIZE) return true;
  u32 end = pa + len > MEM1_SIZE ? MEM1_SIZE : pa + len;
  for (u32 page = pa >> PAGE_SHIFT; page <= (end - 1) >> PAGE_SHIFT; page++)
    if (g_page_stamp[page] >= stamp) return false;
  return true;
}

// Physical accesses used by loaders/HLE (MEM1 only).
inline u32 PhysRead32(u32 pa) { u8* p = PhysPtr(pa, 4); return p ? LoadBE32(p) : 0; }
inline void PhysWrite32(u32 pa, u32 v) { if (u8* p = PhysPtr(pa, 4)) { StoreBE32(p, v); MarkWritten(pa, 4); } }
inline void PhysWrite16(u32 pa, u16 v) { if (u8* p = PhysPtr(pa, 2)) { StoreBE16(p, v); MarkWritten(pa, 2); } }
inline void PhysWrite8(u32 pa, u8 v) { if (u8* p = PhysPtr(pa, 1)) { *p = v; MarkWritten(pa, 1); } }

}  // namespace Mem
