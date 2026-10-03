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
#include "core/memory.h"

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
constexpr u32 R_MEM1 = 26;   // Mem::g_mem1
constexpr u32 R_DBAT = 25;   // data BAT table
constexpr u32 R_STAMP = 24;  // Mem::g_page_stamp
constexpr int FRAME = 48;    // r24-r31 saved at 16..44

constexpr s32 OFF_GPR = (s32)offsetof(CPUState, gpr);
constexpr s32 OFF_PC = (s32)offsetof(CPUState, pc);
constexpr s32 OFF_NPC = (s32)offsetof(CPUState, npc);
constexpr s32 OFF_CR = (s32)offsetof(CPUState, cr);
constexpr s32 OFF_XER = (s32)offsetof(CPUState, xer);
constexpr s32 OFF_SPR = (s32)offsetof(CPUState, spr);
constexpr s32 OFF_CYC = (s32)offsetof(CPUState, cycles);  // big-endian u64: high word first
constexpr s32 OFF_EXC = (s32)offsetof(CPUState, exceptions);
constexpr s32 OFF_MSR = (s32)offsetof(CPUState, msr);
constexpr s32 OFF_FPR = (s32)offsetof(CPUState, fpr);
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

// The interpreter's FP expressions such as A * C + B are contracted into
// fused multiply-adds by the compiler unless built with -ffp-contract=off
// (the bit-exact qemu test build defines EMUGC_NO_FMA): emit the same.
#ifdef EMUGC_NO_FMA
constexpr bool kFused = false;
#else
constexpr bool kFused = true;
#endif

enum FpKind { FK_ADD, FK_SUB, FK_MUL, FK_DIV, FK_MADD, FK_MSUB, FK_NMADD, FK_NMSUB };
struct FpTerm {
  FpKind kind;
  int a, b, c;  // operand halves (0 = ps0, 1 = ps1), -1 = unused
};
struct FpOp {
  int dest;      // 0: ps0 only (double), 1: fill both (single), 2: pair
  bool round;    // round to single
  FpTerm t0, t1; // terms for ps0 and ps1 (t1 used when dest == 2)
};

bool DecodeFpOp(u32 inst, FpOp& f) {
  u32 op = inst >> 26, xo5 = (inst >> 1) & 31;
  auto kind_of = [](u32 x, FpKind& k) {
    switch (x) {
      case 21: k = FK_ADD; return true;
      case 20: k = FK_SUB; return true;
      case 25: k = FK_MUL; return true;
      case 18: k = FK_DIV; return true;
      case 29: k = FK_MADD; return true;
      case 28: k = FK_MSUB; return true;
      case 31: k = FK_NMADD; return true;
      case 30: k = FK_NMSUB; return true;
      default: return false;
    }
  };
  FpKind k;
  if (op == 63 && kind_of(xo5, k)) {  // double precision
    f = {0, false, {k, 0, 0, 0}, {k, 0, 0, 0}};
    return true;
  }
  if (op == 59 && kind_of(xo5, k)) {  // single precision
    f = {1, true, {k, 0, 0, 0}, {k, 0, 0, 0}};
    return true;
  }
  if (op == 4) {
    if (kind_of(xo5, k)) {  // ps_add .. ps_nmsub
      f = {2, true, {k, 0, 0, 0}, {k, 1, 1, 1}};
      return true;
    }
    switch (xo5) {
      case 12: f = {2, true, {FK_MUL, 0, -1, 0}, {FK_MUL, 1, -1, 0}}; return true;      // ps_muls0
      case 13: f = {2, true, {FK_MUL, 0, -1, 1}, {FK_MUL, 1, -1, 1}}; return true;      // ps_muls1
      case 14: f = {2, true, {FK_MADD, 0, 0, 0}, {FK_MADD, 1, 1, 0}}; return true;      // ps_madds0
      case 15: f = {2, true, {FK_MADD, 0, 0, 1}, {FK_MADD, 1, 1, 1}}; return true;      // ps_madds1
      default: return false;
    }
  }
  return false;
}

// f1 = A, f2 = B, f3 = C (as needed) -> f0 = result
void EmitFpTerm(PPCEmitter& e, u32 inst, const FpTerm& t, bool round) {
  u32 ra = Field(inst, 16), rb = Field(inst, 11), rc = Field(inst, 6);
  auto fpr = [](u32 r, int half) { return OFF_FPR + (s32)r * 16 + half * 8; };
  bool uses_c = t.kind == FK_MUL || t.kind >= FK_MADD;
  bool uses_b = t.kind != FK_MUL;
  e.lfd(1, fpr(ra, t.a), R_CPU);
  if (uses_b) e.lfd(2, fpr(rb, t.b), R_CPU);
  if (uses_c) e.lfd(3, fpr(rc, t.c), R_CPU);
  auto A = [&](u32 xo, u32 frt, u32 fra, u32 frb, u32 frc) { e.Emit(PPCEmitter::A(63, frt, fra, frb, frc, xo)); };
  switch (t.kind) {
    case FK_ADD: A(21, 0, 1, 2, 0); break;
    case FK_SUB: A(20, 0, 1, 2, 0); break;
    case FK_MUL: A(25, 0, 1, 0, 3); break;
    case FK_DIV: A(18, 0, 1, 2, 0); break;
    default:
      if (kFused) {
        u32 xo = t.kind == FK_MADD ? 29 : t.kind == FK_MSUB ? 28 : t.kind == FK_NMADD ? 31 : 30;
        A(xo, 0, 1, 2, 3);
      } else {
        A(25, 0, 1, 0, 3);  // f0 = a * c
        if (t.kind == FK_MADD || t.kind == FK_NMADD)
          A(21, 0, 0, 2, 0);  // + b
        else
          A(20, 0, 0, 2, 0);  // - b
        if (t.kind == FK_NMADD || t.kind == FK_NMSUB) e.Emit(PPCEmitter::X(63, 0, 0, 0, 40));  // fneg
      }
      break;
  }
  if (round) e.Emit(PPCEmitter::X(63, 0, 0, 0, 12));  // frsp
}

// Quantized paired-single loads/stores with a float GQR type (the common
// case): two singles (or one and 1.0) straight from/to guest RAM.
bool EmitPsqFast(PPCEmitter& e, u32 inst, std::vector<u32>& slow) {
  u32 op = inst >> 26;
  bool indexed = false, store, update;
  u32 w, qr;
  if (op == 56 || op == 57 || op == 60 || op == 61) {
    store = op >= 60;
    update = op & 1;
    w = (inst >> 15) & 1;
    qr = (inst >> 12) & 7;
  } else if (op == 4) {
    u32 xo = (inst >> 1) & 0x3F;
    if (xo != 6 && xo != 7 && xo != 38 && xo != 39) return false;
    indexed = true;
    store = xo & 1;
    update = xo >= 38;
    w = (inst >> 10) & 1;
    qr = (inst >> 7) & 7;
  } else {
    return false;
  }
  u32 rd = Field(inst, 21), ra = Field(inst, 16), rb = Field(inst, 11);
  if (update && ra == 0) return false;
  // ea -> r3
  if (indexed) {
    LoadGpr(e, 4, rb);
    if (ra) {
      LoadGpr(e, 3, ra);
      e.add(3, 3, 4);
    } else {
      e.mr(3, 4);
    }
  } else {
    s32 d = ((s32)(inst << 20)) >> 20;
    if (ra) {
      LoadGpr(e, 3, ra);
      e.addi(3, 3, d);
    } else {
      e.li(3, d);
    }
  }
  // MSR.DR and MSR.FP, float GQR type, 4-byte alignment
  e.lwz(0, OFF_MSR, R_CPU);
  e.andi_(0, 0, MSR_DR | MSR_FP);
  e.cmplwi(0, 0, MSR_DR | MSR_FP);
  slow.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
  e.lwz(8, OFF_SPR + (s32)(SPR_GQR0 + qr) * 4, R_CPU);
  e.rlwinm(9, 8, store ? 0 : 16, 29, 31);  // type
  e.cmplwi(0, 9, 4);
  slow.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT));
  e.andi_(0, 3, 3);
  slow.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
  // both elements in the same 128 KiB BAT block: (ea & 0x1FFFF) <= 0x1FFF8
  e.rlwinm(0, 3, 0, 15, 31);
  e.LoadImm(10, 0x1FFF8);
  e.cmplw(0, 0, 10);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_GT));
  // translation
  e.rlwinm(4, 3, 15, 15, 29);
  e.lwzx(5, R_DBAT, 4);
  e.cmpwi(0, 5, 0);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  e.rlwimi(5, 3, 0, 15, 31);
  e.addi(7, 5, 8);
  e.rlwinm(7, 7, 9, 23, 31);  // (pa + 8) >> 23 < 3: both elements in MEM1
  e.cmplwi(0, 7, 3);
  slow.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT));
  s32 fpr = OFF_FPR + (s32)rd * 16;
  if (!store) {
    e.lfsx(0, R_MEM1, 5);
    if (w) {
      e.LoadImm(6, 0x3FF00000);  // 1.0
      e.li(7, 0);
      e.stw(6, fpr + 8, R_CPU);
      e.stw(7, fpr + 12, R_CPU);
    } else {
      e.addi(6, 5, 4);
      e.lfsx(1, R_MEM1, 6);
      e.stfd(1, fpr + 8, R_CPU);
    }
    e.stfd(0, fpr, R_CPU);
  } else {
    e.lfd(0, fpr, R_CPU);
    e.Emit(PPCEmitter::X(63, 0, 0, 0, 12));  // frsp
    e.stfsx(0, R_MEM1, 5);
    if (!w) {
      e.lfd(1, fpr + 8, R_CPU);
      e.Emit(PPCEmitter::X(63, 1, 0, 1, 12));  // frsp f1, f1
      e.addi(6, 5, 4);
      e.stfsx(1, R_MEM1, 6);
    }
    e.LoadImm(8, Addr(&Mem::g_write_stamp));
    e.lwz(8, 0, 8);
    e.rlwinm(9, 5, 22, 10, 29);
    e.stwx(8, R_STAMP, 9);
    if (!w) {
      e.addi(6, 5, 4);
      e.rlwinm(9, 6, 22, 10, 29);
      e.stwx(8, R_STAMP, 9);
    }
  }
  if (update) StoreGpr(e, 3, ra);
  return true;
}

bool EmitFpFast(PPCEmitter& e, u32 inst, std::vector<u32>& slow) {
  FpOp f;
  if ((inst & 1) || !DecodeFpOp(inst, f)) return false;  // Rc forms are left to the interpreter
  u32 rd = Field(inst, 21);
  s32 dst = OFF_FPR + (s32)rd * 16;
  e.lwz(0, OFF_MSR, R_CPU);
  e.andi_(0, 0, MSR_FP);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  if (f.dest == 2) {
    // both halves read the sources before writing (rd may be a source)
    EmitFpTerm(e, inst, f.t0, f.round);
    e.Emit(PPCEmitter::X(63, 4, 0, 0, 72));  // fmr f4, f0
    EmitFpTerm(e, inst, f.t1, f.round);
    e.stfd(4, dst, R_CPU);
    e.stfd(0, dst + 8, R_CPU);
  } else {
    EmitFpTerm(e, inst, f.t0, f.round);
    e.stfd(0, dst, R_CPU);
    if (f.dest == 1) e.stfd(0, dst + 8, R_CPU);
  }
  return true;
}

// Branches (always the last instruction of a block). On return r28 holds the
// next pc. The forms that the interpreter uses for idle-loop detection
// ("b ." and bc -8) are left to the interpreter.
bool EmitBranch(PPCEmitter& e, u32 inst) {
  u32 op = inst >> 26;
  bool lk = inst & 1, aa = inst & 2;
  constexpr s32 OFF_LR = OFF_SPR + SPR_LR * 4, OFF_CTR = OFF_SPR + SPR_CTR * 4;
  if (op == 18) {
    s32 li = (s32)((inst & 0x03FFFFFC) << 6) >> 6;
    if (!aa && li == 0 && !lk) return false;
    if (lk) {
      e.addi(4, R_PC, 4);
      e.stw(4, OFF_LR, R_CPU);
    }
    if (aa) {
      e.LoadImm(R_PC, (u32)li);
    } else {
      e.LoadImm(4, (u32)li);
      e.add(R_PC, R_PC, 4);
    }
    return true;
  }
  u32 xo = (inst >> 1) & 0x3FF;
  bool is_bc = op == 16, is_lr = op == 19 && xo == 16, is_ctr = op == 19 && xo == 528;
  if (!is_bc && !is_lr && !is_ctr) return false;
  u32 bo = Field(inst, 21), bi = Field(inst, 16);
  s32 bd = (s16)(inst & 0xFFFC);
  if (is_bc && bd == -8) return false;
  std::vector<u32> fail;
  // target -> r7 (read LR / CTR before they are updated)
  if (is_lr) {
    e.lwz(7, OFF_LR, R_CPU);
    e.rlwinm(7, 7, 0, 0, 29);
  }
  if (!is_ctr && !(bo & 4)) {
    e.lwz(6, OFF_CTR, R_CPU);
    e.addi(6, 6, -1);
    e.stw(6, OFF_CTR, R_CPU);
    e.cmpwi(0, 6, 0);
    // ctr_ok = (CTR != 0) ^ bo[1]
    fail.push_back(e.bc_forward((bo & 2) ? PPCEmitter::BO_FALSE : PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  }
  if (!(bo & 16)) {
    e.lwz(6, OFF_CR, R_CPU);
    e.rlwinm(6, 6, (bi + 1) & 31, 31, 31);
    e.cmpwi(0, 6, (s32)((bo >> 3) & 1));
    fail.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
  }
  // taken
  if (is_bc) {
    if (aa) {
      e.LoadImm(7, (u32)bd);
    } else {
      e.addi(7, R_PC, bd);
    }
  } else if (is_ctr) {
    e.lwz(7, OFF_CTR, R_CPU);
    e.rlwinm(7, 7, 0, 0, 29);
  }
  u32 done = e.b_forward();
  u32 fail_at = e.Here();
  for (u32 f : fail) e.PatchBranchTo(f, fail_at);
  e.addi(7, R_PC, 4);
  e.PatchBranchTo(done, e.Here());
  if (lk) {
    e.addi(4, R_PC, 4);
    e.stw(4, OFF_LR, R_CPU);
  }
  e.mr(R_PC, 7);
  return true;
}

// Load/store with a fast path for guest RAM. Returns false when the
// instruction is not a supported load/store. Emits:
//   fast path (translated address in MEM1)  -> inline bookkeeping, b next
//   slow path -> the interpreter handler (falls through to the caller's code)
struct MemOp {
  bool store;
  u32 size;     // 1, 2, 4 (8 for lfd/stfd)
  bool sign;    // lha
  bool update;  // xxxu forms
  bool indexed;
  u32 fp = 0;   // 0 integer, 1 single (lfs/stfs), 2 double (lfd/stfd)
};

bool DecodeMemOp(u32 inst, MemOp& m) {
  u32 op = inst >> 26;
  m.indexed = false;
  switch (op) {
    case 32: m = {false, 4, false, false, false}; return true;  // lwz
    case 33: m = {false, 4, false, true, false}; return true;   // lwzu
    case 34: m = {false, 1, false, false, false}; return true;  // lbz
    case 35: m = {false, 1, false, true, false}; return true;   // lbzu
    case 36: m = {true, 4, false, false, false}; return true;   // stw
    case 37: m = {true, 4, false, true, false}; return true;    // stwu
    case 38: m = {true, 1, false, false, false}; return true;   // stb
    case 39: m = {true, 1, false, true, false}; return true;    // stbu
    case 40: m = {false, 2, false, false, false}; return true;  // lhz
    case 41: m = {false, 2, false, true, false}; return true;   // lhzu
    case 42: m = {false, 2, true, false, false}; return true;   // lha
    case 43: m = {false, 2, true, true, false}; return true;    // lhau
    case 44: m = {true, 2, false, false, false}; return true;   // sth
    case 45: m = {true, 2, false, true, false}; return true;    // sthu
    case 48: m = {false, 4, false, false, false, 1}; return true;  // lfs
    case 49: m = {false, 4, false, true, false, 1}; return true;   // lfsu
    case 50: m = {false, 8, false, false, false, 2}; return true;  // lfd
    case 51: m = {false, 8, false, true, false, 2}; return true;   // lfdu
    case 52: m = {true, 4, false, false, false, 1}; return true;   // stfs
    case 53: m = {true, 4, false, true, false, 1}; return true;    // stfsu
    case 54: m = {true, 8, false, false, false, 2}; return true;   // stfd
    case 55: m = {true, 8, false, true, false, 2}; return true;    // stfdu
    case 31:
      switch ((inst >> 1) & 0x3FF) {
        case 23: m = {false, 4, false, false, true}; return true;   // lwzx
        case 55: m = {false, 4, false, true, true}; return true;    // lwzux
        case 87: m = {false, 1, false, false, true}; return true;   // lbzx
        case 119: m = {false, 1, false, true, true}; return true;   // lbzux
        case 151: m = {true, 4, false, false, true}; return true;   // stwx
        case 183: m = {true, 4, false, true, true}; return true;    // stwux
        case 215: m = {true, 1, false, false, true}; return true;   // stbx
        case 247: m = {true, 1, false, true, true}; return true;    // stbux
        case 279: m = {false, 2, false, false, true}; return true;  // lhzx
        case 311: m = {false, 2, false, true, true}; return true;   // lhzux
        case 343: m = {false, 2, true, false, true}; return true;   // lhax
        case 407: m = {true, 2, false, false, true}; return true;   // sthx
        case 439: m = {true, 2, false, true, true}; return true;    // sthux
        case 535: m = {false, 4, false, false, true, 1}; return true;  // lfsx
        case 567: m = {false, 4, false, true, true, 1}; return true;   // lfsux
        case 599: m = {false, 8, false, false, true, 2}; return true;  // lfdx
        case 631: m = {false, 8, false, true, true, 2}; return true;   // lfdux
        case 663: m = {true, 4, false, false, true, 1}; return true;   // stfsx
        case 695: m = {true, 4, false, true, true, 1}; return true;    // stfsux
        case 727: m = {true, 8, false, false, true, 2}; return true;   // stfdx
        case 759: m = {true, 8, false, true, true, 2}; return true;    // stfdux
        default: return false;
      }
    default:
      return false;
  }
}

// Emits the fast path; branches that must take the slow path are appended to
// `slow`. On success, falls through with the access done.
void EmitMemFast(PPCEmitter& e, u32 inst, const MemOp& m, std::vector<u32>& slow) {
  u32 rd = Field(inst, 21), ra = Field(inst, 16), rb = Field(inst, 11);
  // ea -> r3
  if (m.indexed) {
    LoadGpr(e, 4, rb);
    if (ra) {
      LoadGpr(e, 3, ra);
      e.add(3, 3, 4);
    } else {
      e.mr(3, 4);
    }
  } else {
    s32 d = (s16)(inst & 0xFFFF);
    if (ra) {
      LoadGpr(e, 3, ra);
      e.addi(3, 3, d);
    } else {
      e.li(3, d);
    }
  }
  // Data translation must be on (MSR.DR); FP accesses also need MSR.FP
  e.lwz(0, OFF_MSR, R_CPU);
  if (m.fp) {
    e.andi_(0, 0, MSR_DR | MSR_FP);
    e.cmplwi(0, 0, MSR_DR | MSR_FP);
    slow.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
    // natural alignment (the Xenon traps on unaligned FP accesses)
    e.andi_(0, 3, m.size - 1);
    slow.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
  } else {
    e.andi_(0, 0, MSR_DR);
    slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  }
  // entry = dbat[ea >> 17]
  e.rlwinm(4, 3, 15, 15, 29);  // (ea >> 17) * 4
  e.lwzx(5, R_DBAT, 4);
  e.cmpwi(0, 5, 0);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  // pa = block base | (ea & 0x1FFFF); MEM1 only (pa < 24 MiB: pa >> 23 < 3)
  e.rlwimi(5, 3, 0, 15, 31);
  e.rlwinm(7, 5, 9, 23, 31);  // pa >> 23
  e.cmplwi(0, 7, 3);
  slow.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT));
  const s32 fpr = OFF_FPR + (s32)rd * 16;  // ps0, then ps1 at +8
  if (m.store && m.fp == 2) {
    e.lwz(6, fpr, R_CPU);
    e.lwz(7, fpr + 4, R_CPU);
    e.add(5, 5, R_MEM1);  // host address
    e.stw(6, 0, 5);
    e.stw(7, 4, 5);
    e.Emit(PPCEmitter::XO(5, R_MEM1, 5, 40));  // subf r5, r_mem1, r5: back to pa
  } else if (m.store && m.fp == 1) {
    e.lfd(0, fpr, R_CPU);
    e.Emit(PPCEmitter::X(63, 0, 0, 0, 12));  // frsp f0, f0
    e.stfsx(0, R_MEM1, 5);
  }
  if (m.store && m.fp) {
    e.LoadImm(8, Addr(&Mem::g_write_stamp));
    e.lwz(8, 0, 8);
    e.rlwinm(9, 5, 22, 10, 29);
    e.stwx(8, R_STAMP, 9);
  } else if (!m.store && m.fp == 2) {
    e.add(5, 5, R_MEM1);
    e.lwz(6, 0, 5);
    e.lwz(7, 4, 5);
    e.stw(6, fpr, R_CPU);
    e.stw(7, fpr + 4, R_CPU);
  } else if (!m.store && m.fp == 1) {
    e.lfsx(0, R_MEM1, 5);
    e.stfd(0, fpr, R_CPU);
    e.stfd(0, fpr + 8, R_CPU);
  } else if (m.store) {
    LoadGpr(e, 6, rd);
    u32 xo = m.size == 4 ? 151 : (m.size == 2 ? 407 : 215);  // stwx, sthx, stbx
    e.Emit(PPCEmitter::X(31, 6, R_MEM1, 5, xo));
    // write tracking: page_stamp[pa >> 12] = g_write_stamp
    e.LoadImm(8, Addr(&Mem::g_write_stamp));
    e.lwz(8, 0, 8);
    e.rlwinm(9, 5, 22, 10, 29);  // (pa >> 12) * 4
    e.stwx(8, R_STAMP, 9);
  } else {
    u32 xo = m.size == 4 ? 23 : (m.size == 2 ? (m.sign ? 343 : 279) : 87);  // lwzx, lhax/lhzx, lbzx
    e.Emit(PPCEmitter::X(31, 6, R_MEM1, 5, xo));
    StoreGpr(e, 6, rd);
  }
  if (m.update) StoreGpr(e, 3, ra);
}

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
  for (u32 r = 24; r <= 31; r++) e.stw(r, 16 + (s32)(r - 24) * 4, 1);
  e.LoadImm(R_CPU, Addr(&cpu));
  e.LoadImm(R_MEM1, Addr(Mem::g_mem1));
  e.LoadImm(R_DBAT, Addr(Mem::DataBatTable()));
  e.LoadImm(R_STAMP, Addr(Mem::g_page_stamp));
  e.lwz(R_PC, OFF_PC, R_CPU);
  e.li(R_PENDING, 0);
  bl(s_budget);

  bool last_inline = false;
  for (u32 k = 0; k < count; k++) {
    MemOp m;
    u32 ra = Field(insts[k], 16), rd = Field(insts[k], 21);
    std::vector<u32> slow;
    bool fast = false;
    if (s_inline_enabled) {
      if (DecodeMemOp(insts[k], m) && !(m.update && (ra == 0 || (!m.store && !m.fp && ra == rd)))) {
        EmitMemFast(e, insts[k], m, slow);
        fast = true;
      } else if (EmitFpFast(e, insts[k], slow)) {
        fast = true;
      } else if (EmitPsqFast(e, insts[k], slow)) {
        fast = true;
      }
    }
    if (fast) {
      // fast path done: inline bookkeeping
      e.addi(R_PC, R_PC, 4);
      e.addi(R_PENDING, R_PENDING, 1);
      e.Emit(PPCEmitter::D(13, R_BUDGET, R_BUDGET, -1));  // addic. r30, r30, -1
      if (k + 1 < count) exhaust.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
      u32 to_next = e.b_forward();
      // slow path: the interpreter
      u32 slow_at = e.Here();
      for (u32 at : slow) e.PatchBranchTo(at, slow_at);
      bl(s_flush);
      e.LoadImm(3, insts[k]);
      e.CallAbs(Addr((const void*)fns[k]));
      bl(s_post);
      if (k + 1 < count) {
        leave.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
        e.PatchBranchTo(to_next, e.Here());
      } else {
        // last instruction: the slow path leaves with the state published,
        // the fast path joins the flushing exit
        leave.push_back(e.b_forward());
        e.PatchBranchTo(to_next, e.Here());
      }
      last_inline = true;  // the fast path needs the final flush
      continue;
    }
    if (s_inline_enabled && k + 1 == count && EmitBranch(e, insts[k])) {
      e.addi(R_PENDING, R_PENDING, 1);
      last_inline = true;  // flush (pc = r28) and leave
      break;
    }
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
  for (u32 r = 24; r <= 31; r++) e.lwz(r, 16 + (s32)(r - 24) * 4, 1);
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
