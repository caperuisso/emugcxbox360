// emugcxbox360 - interpreter internals shared between instruction groups
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/gekko/cpu.h"
#include "core/memory.h"

namespace Interpreter {

using OpFn = void (*)(u32 inst);

// Sub-tables are indexed by the 10-bit extended opcode, bits 21..30.
enum Table { T_PRIMARY, T_4, T_19, T_31, T_59, T_63, T_COUNT };

void Reg(int table, u32 ext, OpFn fn, const char* name);
// A-form: 5-bit extended opcode, the register field above it is free.
void RegA(int table, u32 ext5, OpFn fn, const char* name);
// XO-form in table 31: registers both the OE=0 and OE=1 encodings.
void RegXO(u32 ext9, OpFn fn, const char* name);

void RegisterInteger();
void RegisterLoadStore();
void RegisterFloat();
void RegisterPaired();

// Instruction fields (PowerPC bit 0 is the MSB)
inline u32 OPCD(u32 i) { return i >> 26; }
inline u32 RD(u32 i) { return (i >> 21) & 31; }
inline u32 RS(u32 i) { return (i >> 21) & 31; }
inline u32 RA(u32 i) { return (i >> 16) & 31; }
inline u32 RB(u32 i) { return (i >> 11) & 31; }
inline u32 RC(u32 i) { return (i >> 6) & 31; }
inline u32 SH(u32 i) { return (i >> 11) & 31; }
inline u32 MB(u32 i) { return (i >> 6) & 31; }
inline u32 ME(u32 i) { return (i >> 1) & 31; }
inline u32 UIMM(u32 i) { return i & 0xFFFF; }
inline s32 SIMM(u32 i) { return (s16)(i & 0xFFFF); }
inline bool Rc(u32 i) { return i & 1; }
inline bool OE(u32 i) { return (i >> 10) & 1; }
inline u32 CRFD(u32 i) { return (i >> 23) & 7; }
inline u32 CRFS(u32 i) { return (i >> 18) & 7; }
inline u32 SPRN(u32 i) { return ((i >> 16) & 0x1F) | (((i >> 11) & 0x1F) << 5); }

inline u32& GPR(u32 n) { return cpu.gpr[n]; }
inline PairedReg& FPR(u32 n) { return cpu.fpr[n]; }
inline u32 RA0(u32 i) { return RA(i) ? cpu.gpr[RA(i)] : 0; }

inline u32 GetCRField(u32 n) { return (cpu.cr >> (28 - 4 * n)) & 0xF; }
inline void SetCRField(u32 n, u32 v) {
  u32 sh = 28 - 4 * n;
  cpu.cr = (cpu.cr & ~(0xFu << sh)) | ((v & 0xF) << sh);
}
inline u32 GetCRBit(u32 b) { return (cpu.cr >> (31 - b)) & 1; }
inline void SetCRBit(u32 b, u32 v) {
  if (v)
    cpu.cr |= 0x80000000u >> b;
  else
    cpu.cr &= ~(0x80000000u >> b);
}
inline void UpdateCR0(u32 v) {
  u32 f = (s32)v < 0 ? 8 : ((s32)v > 0 ? 4 : 2);
  if (cpu.xer & XER_SO) f |= 1;
  SetCRField(0, f);
}

inline bool NoFault() { return !(cpu.exceptions & EXC_DSI); }

inline bool CheckFP() {
  if (UNLIKELY(!(cpu.msr & MSR_FP))) {
    CPU::RaiseException(EXC_FPU_UNAVAILABLE);
    return false;
  }
  return true;
}

inline double RoundSingle(double v) { return (double)(float)v; }

void ProgramException(u32 reason);

}  // namespace Interpreter
