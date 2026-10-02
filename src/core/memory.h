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

// Direct pointer into RAM for DMA from hardware (physical address), or nullptr.
u8* PhysPtr(u32 pa, u32 len);

// Physical accesses used by loaders/HLE (MEM1 only).
inline u32 PhysRead32(u32 pa) { u8* p = PhysPtr(pa, 4); return p ? LoadBE32(p) : 0; }
inline void PhysWrite32(u32 pa, u32 v) { if (u8* p = PhysPtr(pa, 4)) StoreBE32(p, v); }
inline void PhysWrite16(u32 pa, u16 v) { if (u8* p = PhysPtr(pa, 2)) StoreBE16(p, v); }
inline void PhysWrite8(u32 pa, u8 v) { if (u8* p = PhysPtr(pa, 1)) *p = v; }

}  // namespace Mem
