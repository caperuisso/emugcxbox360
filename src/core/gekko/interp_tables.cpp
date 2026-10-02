// SPDX-License-Identifier: GPL-2.0-or-later
// Opcode dispatch tables, SPR access and the unknown-instruction handler.
#include "core/coretiming.h"
#include "core/gekko/interp_internal.h"
#include "core/hw/hw.h"

namespace Interpreter {

namespace {
OpFn s_tables[T_COUNT][1024];
const char* s_names[T_COUNT][1024];
int s_sub_of_primary[64];  // primary opcode -> sub-table, or -1

void Unknown(u32 inst) {
  static u32 reported[64];
  u32 op = OPCD(inst);
  if (reported[op]++ < 4)
    LOG("Interpreter: unknown instruction %08x (op %u ext %u) at %08x\n", inst, op, (inst >> 1) & 0x3FF,
        cpu.pc);
  ProgramException(PROGRAM_ILLEGAL);
}

void DispatchSub(u32 inst) {
  s_tables[s_sub_of_primary[OPCD(inst)]][(inst >> 1) & 0x3FF](inst);
}

void LockedCacheDMA(u32 dmal) {
  u32 dmau = cpu.spr[SPR_DMAU];
  u32 mem_addr = dmau & ~0x1Fu;
  u32 cache_addr = dmal & ~0x1Fu;
  u32 blocks = ((dmau & 0x1F) << 2) | ((dmal >> 2) & 3);
  if (blocks == 0) blocks = 128;
  u32 len = blocks * 32;
  u8* cache = Mem::PhysPtr(cache_addr, len);
  u8* ram = Mem::PhysPtr(mem_addr, len);
  if (!cache || !ram) {
    LOG("LC DMA: bad addresses mem=%08x lc=%08x len=%u\n", mem_addr, cache_addr, len);
    return;
  }
  if (dmal & 0x10)
    memcpy(cache, ram, len);  // load into locked cache
  else
    memcpy(ram, cache, len);  // store from locked cache
}
}  // namespace

void ProgramException(u32 reason) {
  cpu.program_reason = reason;
  CPU::RaiseException(EXC_PROGRAM);
}

void Reg(int table, u32 ext, OpFn fn, const char* name) {
  s_tables[table][ext] = fn;
  s_names[table][ext] = name;
}

void RegA(int table, u32 ext5, OpFn fn, const char* name) {
  for (u32 i = 0; i < 32; i++) Reg(table, (i << 5) | ext5, fn, name);
}

void RegXO(u32 ext9, OpFn fn, const char* name) {
  Reg(T_31, ext9, fn, name);
  Reg(T_31, ext9 | 0x200, fn, name);
}

void Init() {
  for (int t = 0; t < T_COUNT; t++)
    for (int e = 0; e < 1024; e++) {
      s_tables[t][e] = Unknown;
      s_names[t][e] = "unknown";
    }
  for (int i = 0; i < 64; i++) s_sub_of_primary[i] = -1;
  const int subs[][2] = {{4, T_4}, {19, T_19}, {31, T_31}, {59, T_59}, {63, T_63}};
  for (auto& s : subs) {
    s_sub_of_primary[s[0]] = s[1];
    Reg(T_PRIMARY, s[0], DispatchSub, "(group)");
  }
  RegisterInteger();
  RegisterLoadStore();
  RegisterFloat();
  RegisterPaired();
}

void Execute(u32 inst) { s_tables[T_PRIMARY][OPCD(inst)](inst); }

const char* GetOpName(u32 inst) {
  int sub = s_sub_of_primary[OPCD(inst)];
  if (sub < 0) return s_names[T_PRIMARY][OPCD(inst)];
  return s_names[sub][(inst >> 1) & 0x3FF];
}

u32 ReadSPR(u32 n) {
  switch (n) {
    case SPR_XER: return cpu.xer;
    case SPR_DEC: return CPU::GetDecrementer();
    case SPR_TBL_R: return (u32)CPU::GetTimeBase();
    case SPR_TBU_R: return (u32)(CPU::GetTimeBase() >> 32);
    case SPR_WPAR: return (cpu.spr[SPR_WPAR] & ~1u) | (HW::GatherPipeBytes() ? 1 : 0);
    default: return cpu.spr[n];
  }
}

void WriteSPR(u32 n, u32 v) {
  switch (n) {
    case SPR_XER: cpu.xer = v; return;
    case SPR_DEC: CPU::SetDecrementer(v); return;
    case SPR_TBL_W: CPU::SetTimeBase((CPU::GetTimeBase() & 0xFFFFFFFF00000000ull) | v); return;
    case SPR_TBU_W: CPU::SetTimeBase((CPU::GetTimeBase() & 0xFFFFFFFFull) | ((u64)v << 32)); return;
    case SPR_PVR: return;  // read-only
    case SPR_WPAR:
      cpu.spr[n] = v & ~0x1Fu;
      HW::ResetGatherPipe();
      return;
    case SPR_DMAL:
      cpu.spr[n] = v;
      if (v & 2) {  // trigger
        LockedCacheDMA(v);
        cpu.spr[n] &= ~2u;
      }
      return;
    default: break;
  }
  cpu.spr[n] = v;
  if (n >= SPR_IBAT0U && n < SPR_IBAT0U + 16) Mem::UpdateBATs();
}

}  // namespace Interpreter
