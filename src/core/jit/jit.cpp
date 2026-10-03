// emugcxbox360 - PowerPC -> PowerPC dynamic recompiler (host = Xenon)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Generated block conventions:
//   r31 = &cpu, r28 = guest pc of the current instruction, r27 = instructions
//   executed but not yet added to cpu.cycles, r30 = instructions that may still
//   run before the end of the time slice (budget), r29 = return address of the
//   shared "post" routine.
//
// Instructions are either inlined (simple integer operations: the guest
// instruction is re-emitted on host registers loaded from the CPU state, which
// works because guest and host are both PowerPC) or interpreted (a call to the
// interpreter handler followed by the shared "post" routine, which performs the
// cached interpreter's bookkeeping). Inlined instructions cannot raise
// exceptions, branch or flag idle loops, so they only advance r28/r27 and the
// budget; the emulation stays bit-identical to the interpreter.
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
constexpr u32 R_CPU = 31;
constexpr u32 R_BUDGET = 30;
constexpr u32 R_LINK = 29;
constexpr u32 R_PC = 28;
constexpr u32 R_PENDING = 27;
constexpr int FRAME = 32;

constexpr s32 OFF_GPR = (s32)offsetof(CPUState, gpr);
constexpr s32 OFF_PC = (s32)offsetof(CPUState, pc);
constexpr s32 OFF_NPC = (s32)offsetof(CPUState, npc);
constexpr s32 OFF_CR = (s32)offsetof(CPUState, cr);
constexpr s32 OFF_XER = (s32)offsetof(CPUState, xer);
constexpr s32 OFF_SPR = (s32)offsetof(CPUState, spr);
constexpr s32 OFF_CYC = (s32)offsetof(CPUState, cycles);  // big-endian u64: high word first
constexpr s32 OFF_EXC = (s32)offsetof(CPUState, exceptions);
static_assert(OFF_SPR + 1024 * 4 < 32768, "CPU state offsets must fit a 16-bit displacement");

inline s32 Gpr(u32 n) { return OFF_GPR + (s32)n * 4; }

// Offsets (instructions) of the shared routines in the code buffer
size_t s_post = 0, s_flush = 0, s_budget = 0;

// r30 = ceil((slice_end - cycles) / cpi), saturated to 2^31 - 1. Uses r6-r11.
// The code ends with both paths branching to the returned join points.
void EmitBudgetBody(PPCEmitter& e, std::vector<u32>& joins) {
  const u32 cpi = CPU::g_cycles_per_instruction;
  e.LoadImm(6, Addr(&CoreTiming::g_slice_end));
  e.lwz(7, 0, 6);
  e.lwz(8, 4, 6);
  e.lwz(9, OFF_CYC, R_CPU);
  e.lwz(10, OFF_CYC + 4, R_CPU);
  e.Emit(PPCEmitter::XO(10, 10, 8, 8));  // subfc r10, r10, r8: lo = end.lo - cyc.lo
  e.Emit(PPCEmitter::XO(9, 9, 7, 136));  // subfe r9, r9, r7: hi = end.hi - cyc.hi - borrow
  e.cmpwi(0, 9, 0);
  u32 sat1 = e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ);  // hi != 0
  e.cmpwi(0, 10, 0);
  u32 sat2 = e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_LT);   // >= 2^31
  if (cpi > 1) {
    e.addi(10, 10, (s32)cpi - 1);
    e.li(11, (s32)cpi);
    e.Emit(PPCEmitter::XO(10, 10, 11, 459));  // divwu
  }
  e.mr(R_BUDGET, 10);
  joins.push_back(e.b_forward());
  e.PatchBranchTo(sat1, e.Here());
  e.PatchBranchTo(sat2, e.Here());
  e.LoadImm(R_BUDGET, 0x7FFFFFFF);
}

void EmitBudget(PPCEmitter& e) {
  std::vector<u32> joins;
  EmitBudgetBody(e, joins);
  u32 at = e.Here();
  for (u32 j : joins) e.PatchBranchTo(j, at);
  e.blr();
}

// cycles += pending * cpi; pending = 0; cpu.pc = r28; cpu.npc = r28 + 4. Leaf.
void EmitFlush(PPCEmitter& e) {
  const u32 cpi = CPU::g_cycles_per_instruction;
  if (cpi != 1) {
    e.li(6, (s32)cpi);
    e.Emit(PPCEmitter::XO(R_PENDING, R_PENDING, 6, 235));  // mullw
  }
  e.lwz(4, OFF_CYC + 4, R_CPU);
  e.lwz(5, OFF_CYC, R_CPU);
  e.Emit(PPCEmitter::XO(4, 4, R_PENDING, 10));  // addc r4, r4, pending
  e.addze(5, 5);
  e.stw(4, OFF_CYC + 4, R_CPU);
  e.stw(5, OFF_CYC, R_CPU);
  e.li(R_PENDING, 0);
  e.stw(R_PC, OFF_PC, R_CPU);
  e.addi(4, R_PC, 4);
  e.stw(4, OFF_NPC, R_CPU);
  e.blr();
}

// After an interpreted instruction (cpu.pc = its address = r28, cpu.npc = next):
// the cached interpreter's bookkeeping. On return cr0.eq = continue (r28 = new
// pc, budget recomputed), cr0.ne = leave the block.
void EmitPost(PPCEmitter& e) {
  const u32 cpi = CPU::g_cycles_per_instruction;
  std::vector<u32> leave;
  e.mflr(R_LINK);
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
  e.addi(5, R_PC, 4);
  e.cmplw(0, 4, 5);
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
  // continue: r28 = pc (npc, still in r4), new budget
  e.mr(R_PC, 4);
  std::vector<u32> joins;
  EmitBudgetBody(e, joins);
  u32 join = e.Here();
  for (u32 j : joins) e.PatchBranchTo(j, join);
  e.cmpw(0, 0, 0);  // eq
  e.mtlr(R_LINK);
  e.blr();
  // leave: cr0 = ne
  u32 at = e.Here();
  for (u32 l : leave) e.PatchBranchTo(l, at);
  e.li(0, 1);
  e.cmpwi(0, 0, 0);  // ne
  e.mtlr(R_LINK);
  e.blr();
}

bool AllocCode() {
  if (s_code) return true;
  s_code = (u32*)malloc(CODE_SIZE);
  return s_code != nullptr;
}

void Reset() {
  PPCEmitter e;
  s_budget = e.Here();
  EmitBudget(e);
  s_flush = e.Here();
  EmitFlush(e);
  s_post = e.Here();
  EmitPost(e);
  memcpy(s_code, e.code.data(), e.code.size() * 4);
  FlushCode(s_code, e.code.size());
  s_used = e.code.size();
}

// ---- Inlined instructions ----
// Operands go to host r3 (first source), r4 (second source), result in r6.
inline void LoadGpr(PPCEmitter& e, u32 h, u32 g) { e.lwz(h, Gpr(g), R_CPU); }
inline void StoreGpr(PPCEmitter& e, u32 h, u32 g) { e.stw(h, Gpr(g), R_CPU); }

// Copies host cr0 (LT GT EQ) plus the guest XER.SO into guest CR field `f`.
void UpdateGuestCR(PPCEmitter& e, u32 f) {
  e.mfcr(0);
  e.rlwinm(7, 0, 4, 28, 30);  // LT GT EQ -> bits 3..1 of a nibble
  e.lwz(8, OFF_XER, R_CPU);
  e.rlwinm(8, 8, 1, 31, 31);  // SO -> bit 0
  e.Emit(PPCEmitter::X(31, 7, 7, 8, 444));  // or r7, r7, r8
  e.lwz(9, OFF_CR, R_CPU);
  e.rlwimi(9, 7, (28 - 4 * f) & 31, 4 * f, 4 * f + 3);
  e.stw(9, OFF_CR, R_CPU);
}

inline u32 Field(u32 inst, u32 shift) { return (inst >> shift) & 31; }
inline u32 WithField(u32 inst, u32 shift, u32 value) { return (inst & ~(31u << shift)) | ((value & 31) << shift); }

// Emits `inst` inline when supported; returns false otherwise.
bool EmitInline(PPCEmitter& e, u32 inst) {
  u32 op = inst >> 26;
  u32 rd = Field(inst, 21), ra = Field(inst, 16), rb = Field(inst, 11);
  bool rc = inst & 1;
  switch (op) {
    case 7:   // mulli
    case 14:  // addi
    case 15:  // addis
    {
      bool ra_literal = (op != 7) && ra == 0;
      if (!ra_literal) LoadGpr(e, 3, ra);
      e.Emit(WithField(WithField(inst, 21, 6), 16, ra_literal ? 0 : 3));
      StoreGpr(e, 6, rd);
      return true;
    }
    case 24: case 25: case 26: case 27:  // ori, oris, xori, xoris: rS -> rA
    case 28: case 29:                    // andi., andis.
      LoadGpr(e, 3, rd);
      e.Emit(WithField(WithField(inst, 21, 3), 16, 6));
      StoreGpr(e, 6, ra);
      if (op >= 28) UpdateGuestCR(e, 0);
      return true;
    case 20:  // rlwimi: rA is also an input
      LoadGpr(e, 3, rd);
      LoadGpr(e, 6, ra);
      e.Emit(WithField(WithField(inst, 21, 3), 16, 6));
      StoreGpr(e, 6, ra);
      if (rc) UpdateGuestCR(e, 0);
      return true;
    case 21:  // rlwinm
      LoadGpr(e, 3, rd);
      e.Emit(WithField(WithField(inst, 21, 3), 16, 6));
      StoreGpr(e, 6, ra);
      if (rc) UpdateGuestCR(e, 0);
      return true;
    case 23:  // rlwnm
      LoadGpr(e, 3, rd);
      LoadGpr(e, 4, rb);
      e.Emit(WithField(WithField(WithField(inst, 21, 3), 16, 6), 11, 4));
      StoreGpr(e, 6, ra);
      if (rc) UpdateGuestCR(e, 0);
      return true;
    case 10:  // cmpli
    case 11:  // cmpi
    {
      if (inst & (1u << 21)) return false;  // L = 1 is not valid on the Gekko
      u32 crf = (inst >> 23) & 7;
      LoadGpr(e, 3, ra);
      e.Emit((inst & ~((7u << 23) | (31u << 16))) | (3u << 16));  // host crf 0, rA = r3
      UpdateGuestCR(e, crf);
      return true;
    }
    case 31: {
      u32 xo = (inst >> 1) & 0x3FF;
      switch (xo) {
        // XO-form arithmetic with OE = 0 (the OE = 1 encodings have other xo values)
        case 266: case 40: case 235: case 75: case 11: {  // add, subf, mullw, mulhw, mulhwu
          LoadGpr(e, 3, ra);
          LoadGpr(e, 4, rb);
          e.Emit(WithField(WithField(WithField(inst, 21, 6), 16, 3), 11, 4));
          StoreGpr(e, 6, rd);
          if (rc) UpdateGuestCR(e, 0);
          return true;
        }
        case 104: {  // neg
          LoadGpr(e, 3, ra);
          e.Emit(WithField(WithField(inst, 21, 6), 16, 3));
          StoreGpr(e, 6, rd);
          if (rc) UpdateGuestCR(e, 0);
          return true;
        }
        // X-form logical and shifts: rA = f(rS, rB)
        case 28: case 444: case 316: case 476: case 124: case 284: case 60: case 412:  // and or xor nand nor eqv andc orc
        case 24: case 536:                                                             // slw srw
          LoadGpr(e, 3, rd);
          LoadGpr(e, 4, rb);
          e.Emit(WithField(WithField(WithField(inst, 21, 3), 16, 6), 11, 4));
          StoreGpr(e, 6, ra);
          if (rc) UpdateGuestCR(e, 0);
          return true;
        case 954: case 922: case 26:  // extsb, extsh, cntlzw: rA = f(rS)
          LoadGpr(e, 3, rd);
          e.Emit(WithField(WithField(inst, 21, 3), 16, 6));
          StoreGpr(e, 6, ra);
          if (rc) UpdateGuestCR(e, 0);
          return true;
        case 0: case 32: {  // cmp, cmpl
          if (inst & (1u << 21)) return false;
          u32 crf = (inst >> 23) & 7;
          LoadGpr(e, 3, ra);
          LoadGpr(e, 4, rb);
          e.Emit((inst & ~((7u << 23) | (31u << 16) | (31u << 11))) | (3u << 16) | (4u << 11));
          UpdateGuestCR(e, crf);
          return true;
        }
        case 339: {  // mfspr LR / CTR
          u32 spr = ((inst >> 16) & 31) | (((inst >> 11) & 31) << 5);
          if (spr != SPR_LR && spr != SPR_CTR) return false;
          e.lwz(6, OFF_SPR + (s32)spr * 4, R_CPU);
          StoreGpr(e, 6, rd);
          return true;
        }
        default:
          return false;
      }
    }
    default:
      return false;
  }
}

bool s_inline_enabled = true;

}  // namespace

bool Enabled() { return s_enabled; }
void SetEnabled(bool on) { s_enabled = on && kHostPPC; }
void SetInlining(bool on) { s_inline_enabled = on; }

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
  e.code.reserve(32 + count * 16);
  std::vector<u32> leave;    // to the epilogue (state already published)
  std::vector<u32> exhaust;  // budget exhausted after an inlined instruction
  struct Call {
    u32 at;
    size_t target;
  };
  std::vector<Call> calls;  // `bl` to shared routines, patched once placed
  auto bl = [&](size_t target) {
    calls.push_back({e.Here(), target});
    e.Emit(0x48000001);
  };

  // Prologue: save LR and r27-r31; r28 = pc, nothing pending, budget
  e.stwu(1, -FRAME, 1);
  e.mflr(0);
  e.stw(0, FRAME + 4, 1);
  e.stw(27, 12, 1);
  e.stw(28, 16, 1);
  e.stw(29, 20, 1);
  e.stw(30, 24, 1);
  e.stw(31, 28, 1);
  e.LoadImm(R_CPU, Addr(&cpu));
  e.lwz(R_PC, OFF_PC, R_CPU);
  e.li(R_PENDING, 0);
  bl(s_budget);

  bool last_inline = false;
  for (u32 k = 0; k < count; k++) {
    if (s_inline_enabled && EmitInline(e, insts[k])) {
      e.addi(R_PC, R_PC, 4);
      e.addi(R_PENDING, R_PENDING, 1);
      e.Emit(PPCEmitter::D(13, R_BUDGET, R_BUDGET, -1));  // addic. r30, r30, -1
      if (k + 1 < count) exhaust.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
      last_inline = true;
      continue;
    }
    // Interpreted: publish pc/npc/cycles, call the handler, bookkeeping
    bl(s_flush);
    e.LoadImm(3, insts[k]);
    e.CallAbs(Addr((const void*)fns[k]));
    bl(s_post);
    if (k + 1 < count) leave.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
    last_inline = false;
  }
  // After an inlined last instruction, or when the budget ran out: publish
  // the state (pc = r28, cycles).
  if (!last_inline) leave.push_back(e.b_forward());
  u32 flush_exit = e.Here();
  for (u32 at : exhaust) e.PatchBranchTo(at, flush_exit);
  bl(s_flush);

  // Epilogue
  u32 epilogue = e.Here();
  for (u32 at : leave) e.PatchBranchTo(at, epilogue);
  e.lwz(0, FRAME + 4, 1);
  e.mtlr(0);
  e.lwz(27, 12, 1);
  e.lwz(28, 16, 1);
  e.lwz(29, 20, 1);
  e.lwz(30, 24, 1);
  e.lwz(31, 28, 1);
  e.addi(1, 1, FRAME);
  e.blr();

  if (s_used + e.code.size() > CODE_SIZE / 4) return nullptr;
  u32* dst = s_code + s_used;
  for (const Call& c : calls) {
    s32 off = ((s32)c.target - (s32)(s_used + c.at)) * 4;
    e.code[c.at] = 0x48000001 | ((u32)off & 0x03FFFFFC);
  }
  memcpy(dst, e.code.data(), e.code.size() * 4);
  FlushCode(dst, e.code.size());
  s_used += e.code.size();
  return (BlockFn)(void*)dst;
}

}  // namespace Jit
