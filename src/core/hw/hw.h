// emugcxbox360 - Flipper hardware blocks and MMIO routing
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "core/common.h"

namespace HW {
void Init();
void Reset();
u16 Read16(u32 pa);
u32 Read32(u32 pa);
void Write8(u32 pa, u8 v);
void Write16(u32 pa, u16 v);
void Write32(u32 pa, u32 v);

// Write-gather pipe (0xCC008000): CPU writes destined for the GX FIFO.
void WriteGatherPipe(const u8* data, u32 len);
void ResetGatherPipe();
u32 GatherPipeBytes();
}  // namespace HW

// Processor Interface: interrupt controller + CPU-side GX FIFO pointers
namespace PI {
enum Interrupt : u32 {
  INT_ERROR = 0x1,
  INT_RSW = 0x2,
  INT_DI = 0x4,
  INT_SI = 0x8,
  INT_EXI = 0x10,
  INT_AI = 0x20,
  INT_DSP = 0x40,
  INT_MI = 0x80,
  INT_VI = 0x100,
  INT_PE_TOKEN = 0x200,
  INT_PE_FINISH = 0x400,
  INT_CP = 0x800,
  INT_DEBUG = 0x1000,
  INT_HSP = 0x2000,
  RESET_SWITCH_STATE = 0x10000,
};
void Reset();
void SetInterrupt(u32 cause, bool active);
u32 Read32(u32 off);
void Write32(u32 off, u32 v);
// Pushes one 32-byte block into the CPU FIFO in RAM.
void FifoWriteBlock(const u8* block);
}  // namespace PI

// Video Interface
namespace VI {
void Init();
void Reset();
u16 Read16(u32 off);
void Write16(u32 off, u16 v);
// Set by the VI when a field has been presented; cleared by System.
extern bool g_field_done;
// Leaves the VI enabled in NTSC or PAL like the IPL does before starting a game.
void SetBootTVMode(bool pal);
}  // namespace VI

// Serial Interface (controllers)
namespace SI {
void Reset();
u32 Read32(u32 off);
void Write32(u32 off, u32 v);
void UpdatePolling();  // called once per field
}  // namespace SI

// External Interface (memory cards, IPL/RTC/SRAM)
namespace EXI {
void Init();
void Reset();
u32 Read32(u32 off);
void Write32(u32 off, u32 v);
}  // namespace EXI

// DVD Interface
namespace DI {
void Reset();
bool OpenDisc(const std::string& path);
void CloseDisc();
bool HasDisc();
bool ReadDisc(u64 offset, void* dst, u32 len);
u32 Read32(u32 off);
void Write32(u32 off, u32 v);
}  // namespace DI

// DSP interface, ARAM DMA and audio DMA (the DSP core itself is HLE'd later)
namespace DSP {
u8* ARAMPtr();
u32 ARAMSize();
void Init();
void Reset();
u16 Read16(u32 off);
void Write16(u32 off, u16 v);
}  // namespace DSP

// Audio Interface (streaming + sample counter)
namespace AI {
void Init();
void Reset();
u32 Read32(u32 off);
void Write32(u32 off, u32 v);
u32 GetDSPSampleRate();
}  // namespace AI

// Memory Interface (protection registers, stubbed)
namespace MI {
void Reset();
u16 Read16(u32 off);
void Write16(u32 off, u16 v);
}  // namespace MI

// Command Processor, Pixel Engine and the GX command stream
namespace GX {
void Reset();
u16 CPRead16(u32 off);
void CPWrite16(u32 off, u16 v);
u16 PERead16(u32 off);
void PEWrite16(u32 off, u16 v);
// Feeds raw FIFO bytes to the command parser.
void ProcessFifo(const u8* data, u32 len);
}  // namespace GX
