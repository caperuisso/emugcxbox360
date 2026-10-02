// emugcxbox360 - Gekko (PowerPC 750CXe derivative) CPU state
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/common.h"

// MSR bits
enum : u32 {
  MSR_POW = 0x40000,
  MSR_ILE = 0x10000,
  MSR_EE = 0x8000,
  MSR_PR = 0x4000,
  MSR_FP = 0x2000,
  MSR_ME = 0x1000,
  MSR_FE0 = 0x800,
  MSR_SE = 0x400,
  MSR_BE = 0x200,
  MSR_FE1 = 0x100,
  MSR_IP = 0x40,
  MSR_IR = 0x20,
  MSR_DR = 0x10,
  MSR_RI = 0x2,
  MSR_LE = 0x1,
};

// XER bits
enum : u32 {
  XER_SO = 0x80000000,
  XER_OV = 0x40000000,
  XER_CA = 0x20000000,
};

// Pending exception flags
enum : u32 {
  EXC_DSI = 1 << 0,
  EXC_ISI = 1 << 1,
  EXC_PROGRAM = 1 << 2,
  EXC_FPU_UNAVAILABLE = 1 << 3,
  EXC_SYSCALL = 1 << 4,
  EXC_ALIGNMENT = 1 << 5,
  EXC_EXTERNAL = 1 << 6,
  EXC_DECREMENTER = 1 << 7,
  EXC_SYNC_MASK = EXC_DSI | EXC_ISI | EXC_PROGRAM | EXC_FPU_UNAVAILABLE | EXC_SYSCALL | EXC_ALIGNMENT,
};

// Special purpose registers
enum : u32 {
  SPR_XER = 1,
  SPR_LR = 8,
  SPR_CTR = 9,
  SPR_DSISR = 18,
  SPR_DAR = 19,
  SPR_DEC = 22,
  SPR_SDR1 = 25,
  SPR_SRR0 = 26,
  SPR_SRR1 = 27,
  SPR_TBL_R = 268,
  SPR_TBU_R = 269,
  SPR_SPRG0 = 272,
  SPR_EAR = 282,
  SPR_TBL_W = 284,
  SPR_TBU_W = 285,
  SPR_PVR = 287,
  SPR_IBAT0U = 528,
  SPR_DBAT0U = 536,
  SPR_GQR0 = 912,
  SPR_HID2 = 920,
  SPR_WPAR = 921,
  SPR_DMAU = 922,
  SPR_DMAL = 923,
  SPR_MMCR0 = 952,
  SPR_HID0 = 1008,
  SPR_HID1 = 1009,
  SPR_L2CR = 1017,
};

// Program exception reasons (SRR1 bits)
enum : u32 {
  PROGRAM_ILLEGAL = 0x80000,
  PROGRAM_PRIVILEGED = 0x40000,
  PROGRAM_TRAP = 0x20000,
};

// A floating point register holds two values (paired singles). Stored as raw
// IEEE double bits so lfd/stfd round-trip exactly.
struct PairedReg {
  u64 ps0;
  u64 ps1;
  double d0() const { return BitCast<double>(ps0); }
  double d1() const { return BitCast<double>(ps1); }
  void set0(double v) { ps0 = BitCast<u64>(v); }
  void set1(double v) { ps1 = BitCast<u64>(v); }
  void fill(double v) { ps0 = ps1 = BitCast<u64>(v); }
};

struct CPUState {
  u32 gpr[32];
  PairedReg fpr[32];
  u32 pc;
  u32 npc;
  u32 cr;
  u32 xer;
  u32 msr;
  u32 fpscr;
  u32 sr[16];
  u32 spr[1024];  // LR/CTR live here too (spr[8], spr[9])

  u64 cycles;      // global emulated time in CPU cycles
  u32 exceptions;  // pending EXC_* flags
  u32 program_reason;

  bool reserve;
  u32 reserve_addr;

  // Timebase and decrementer bookkeeping
  u64 tb_offset;
  u32 dec_start;
  u64 dec_start_tb;
};

extern CPUState cpu;

#define LR (cpu.spr[SPR_LR])
#define CTR (cpu.spr[SPR_CTR])

namespace CPU {

void Init();
void Reset();

// Executes instructions until cpu.cycles reaches CoreTiming::g_slice_end.
void Run();
// Executes until pc == stop_pc (used by HLE boot to call guest functions).
bool RunUntil(u32 stop_pc, u64 max_instructions);
// Executes exactly one instruction (tests / debugging).
void Step();

void RaiseException(u32 exc);
void SetExternalInterrupt(bool active);
void CheckExceptions();

u64 GetTimeBase();
void SetTimeBase(u64 tb);
u32 GetDecrementer();
void SetDecrementer(u32 value);

// Cycles charged per interpreted instruction (tunable speed hack).
extern u32 g_cycles_per_instruction;
// Set by the interpreter when it detects an idle polling loop.
extern bool g_idle;
extern bool g_idle_skipping;  // option, on by default

}  // namespace CPU

namespace Interpreter {
void Init();
void Execute(u32 inst);
const char* GetOpName(u32 inst);
// SPR hooks shared with the CPU core
u32 ReadSPR(u32 n);
void WriteSPR(u32 n, u32 v);
}  // namespace Interpreter
