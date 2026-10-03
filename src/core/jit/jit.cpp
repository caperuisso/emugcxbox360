// emugcxbox360 - PowerPC -> PowerPC dynamic recompiler (host = Xenon)
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/jit/jit.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "core/coretiming.h"
#include "core/jit/ppc_emitter.h"

namespace Jit {

#if defined(__powerpc__) || defined(__PPC__)
constexpr bool kHostPPC = true;
#else
constexpr bool kHostPPC = false;
#endif

namespace {

bool s_enabled = kHostPPC;
constexpr size_t CODE_SIZE = 16u << 20;
u32* s_code = nullptr;  // code buffer (instructions)
size_t s_used = 0;      // in instructions

// ---- Runtime helpers called by generated code ----
void HelperException() {
  if (!(cpu.exceptions & EXC_SYNC_MASK)) cpu.pc = cpu.npc;
  CPU::CheckExceptions();
}

void HelperIdle() {
  CPU::g_idle = false;
  if (CPU::g_idle_skipping && cpu.cycles < CoreTiming::g_slice_end) cpu.cycles = CoreTiming::g_slice_end;
}

inline u32 Addr(const void* p) { return (u32)(uintptr_t)p; }

void FlushCode(const u32* start, size_t words) {
#if defined(__powerpc__) || defined(__PPC__)
  const char* p = (const char*)start;
  const char* end = p + words * 4;
  for (const char* q = (const char*)((uintptr_t)p & ~31u); q < end; q += 32) asm volatile("dcbst 0,%0" ::"r"(q));
  asm volatile("sync");
  for (const char* q = (const char*)((uintptr_t)p & ~31u); q < end; q += 32) asm volatile("icbi 0,%0" ::"r"(q));
  asm volatile("sync; isync");
#else
  (void)start;
  (void)words;
#endif
}

// Register usage in generated code
constexpr u32 R_CPU = 31;      // &cpu
constexpr u32 R_NEXT = 28;     // pc + 4 of the current instruction
constexpr int FRAME = 32;

constexpr s32 OFF_PC = (s32)offsetof(CPUState, pc);
constexpr s32 OFF_NPC = (s32)offsetof(CPUState, npc);
constexpr s32 OFF_CYC = (s32)offsetof(CPUState, cycles);  // big-endian u64: high word first
constexpr s32 OFF_EXC = (s32)offsetof(CPUState, exceptions);


// Shared per-instruction bookkeeping, emitted once at the start of the code
// buffer and called with `bl` after each interpreted instruction. It mirrors
// the cached interpreter loop exactly. On return, cr0.eq = continue with the
// next instruction (npc already set up), cr0.ne = leave the block.
// Uses r28 (pc + 4 of the instruction just executed) and r29 (saved LR).
size_t s_post = 0;  // offset (instructions) of the routine

void EmitPost(PPCEmitter& e) {
  const u32 cpi = CPU::g_cycles_per_instruction;
  std::vector<u32> leave;
  e.mflr(29);
  // cycles += cpi
  e.lwz(4, OFF_CYC + 4, R_CPU);
  e.addic(4, 4, (s32)cpi);
  e.stw(4, OFF_CYC + 4, R_CPU);
  e.lwz(5, OFF_CYC, R_CPU);
  e.addze(5, 5);
  e.stw(5, OFF_CYC, R_CPU);
  // pending exception: handle it and leave
  e.lwz(0, OFF_EXC, R_CPU);
  e.cmpwi(0, 0, 0);
  u32 no_exc = e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ);
  e.CallAbs(Addr((const void*)&HelperException));
  leave.push_back(e.b_forward());
  e.PatchBranchTo(no_exc, e.Here());
  // pc = npc
  e.lwz(4, OFF_NPC, R_CPU);
  e.stw(4, OFF_PC, R_CPU);
  // idle loop detected by the handler
  e.LoadImm(6, Addr(&CPU::g_idle));
  e.lbz(0, 0, 6);
  e.cmpwi(0, 0, 0);
  u32 not_idle = e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ);
  e.CallAbs(Addr((const void*)&HelperIdle));
  e.PatchBranchTo(not_idle, e.Here());
  // taken branch: leave
  e.lwz(4, OFF_NPC, R_CPU);
  e.cmplw(0, 4, R_NEXT);
  leave.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
  // end of the time slice: leave
  e.LoadImm(6, Addr(&CoreTiming::g_slice_end));
  e.lwz(7, 0, 6);
  e.lwz(8, 4, 6);
  e.lwz(9, OFF_CYC, R_CPU);
  e.lwz(10, OFF_CYC + 4, R_CPU);
  e.cmplw(0, 9, 7);
  leave.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_GT));
  u32 hi_less = e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_LT);
  e.cmplw(0, 10, 8);
  leave.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT));
  e.PatchBranchTo(hi_less, e.Here());
  // continue: npc = pc + 4 for the next instruction
  e.addi(R_NEXT, 4, 4);
  e.stw(R_NEXT, OFF_NPC, R_CPU);
  e.cmpw(0, 0, 0);  // eq
  e.mtlr(29);
  e.blr();
  // leave: cr0 = ne
  u32 at = e.Here();
  for (u32 l : leave) e.PatchBranchTo(l, at);
  e.li(0, 1);
  e.cmpwi(0, 0, 0);  // ne
  e.mtlr(29);
  e.blr();
}

bool AllocCode() {
  if (s_code) return true;
  s_code = (u32*)malloc(CODE_SIZE);
  if (!s_code) return false;
  return true;
}

void Reset() {
  PPCEmitter e;
  EmitPost(e);
  memcpy(s_code, e.code.data(), e.code.size() * 4);
  FlushCode(s_code, e.code.size());
  s_post = 0;
  s_used = e.code.size();
}

}  // namespace

bool Enabled() { return s_enabled; }
void SetEnabled(bool on) { s_enabled = on && kHostPPC; }

void Clear() {
  if (s_code) Reset();
}
size_t CodeBytes() { return s_used * 4; }

BlockFn Compile(const u32* insts, const Interpreter::OpFn* fns, u32 count) {
  if (!s_enabled) return nullptr;
  if (!s_code) {
    if (!AllocCode()) {
      s_enabled = false;
      return nullptr;
    }
    Reset();
  }
  PPCEmitter e;
  e.code.reserve(32 + count * 10);
  std::vector<u32> exits;  // branches to the epilogue
  std::vector<u32> posts;  // `bl post` to patch

  // Prologue: save LR and r28-r31, set up npc for the first instruction
  e.stwu(1, -FRAME, 1);
  e.mflr(0);
  e.stw(0, FRAME + 4, 1);
  e.stw(28, 16, 1);
  e.stw(29, 20, 1);
  e.stw(30, 24, 1);
  e.stw(31, 28, 1);
  e.LoadImm(R_CPU, Addr(&cpu));
  e.lwz(R_NEXT, OFF_PC, R_CPU);
  e.addi(R_NEXT, R_NEXT, 4);
  e.stw(R_NEXT, OFF_NPC, R_CPU);

  for (u32 k = 0; k < count; k++) {
    e.LoadImm(3, insts[k]);
    e.CallAbs(Addr((const void*)fns[k]));
    posts.push_back(e.Here());
    e.Emit(0x48000001);  // bl post (patched below)
    if (k + 1 < count) exits.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
  }

  // Epilogue
  u32 epilogue = e.Here();
  for (u32 at : exits) e.PatchBranchTo(at, epilogue);
  e.lwz(0, FRAME + 4, 1);
  e.mtlr(0);
  e.lwz(28, 16, 1);
  e.lwz(29, 20, 1);
  e.lwz(30, 24, 1);
  e.lwz(31, 28, 1);
  e.addi(1, 1, FRAME);
  e.blr();

  if (s_used + e.code.size() > CODE_SIZE / 4) return nullptr;
  u32* dst = s_code + s_used;
  for (u32 at : posts) {
    s32 off = ((s32)s_post - (s32)(s_used + at)) * 4;
    e.code[at] = 0x48000001 | ((u32)off & 0x03FFFFFC);
  }
  memcpy(dst, e.code.data(), e.code.size() * 4);
  FlushCode(dst, e.code.size());
  s_used += e.code.size();
  return (BlockFn)(void*)dst;
}

}  // namespace Jit
