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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "core/coretiming.h"
#include "core/gekko/cpu.h"
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

void HelperIdle() { CPU::OnIdle(); }

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
constexpr u32 R_MAP = 23;    // block cache map (physical word -> block id)
constexpr u32 R_ENTRY = 22;  // block id -> generated code
constexpr u32 R_IBAT = 21;   // instruction BAT table
constexpr int FRAME = 240;   // r14-r31 saved at 16..84, f14-f31 at 96..232
constexpr int FPR_SAVE = 96;
// Guest GPRs cached in host registers within a block
constexpr u32 CACHE_FIRST = 14, CACHE_COUNT = 6;  // r14-r19
constexpr u32 R_CRC = 20;  // guest CR, cached for the whole block

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
constexpr s32 OFF_FPSCR = (s32)offsetof(CPUState, fpscr);
static_assert(OFF_SPR + 1024 * 4 < 32768, "CPU state offsets must fit a 16-bit displacement");

inline s32 Gpr(u32 n) { return OFF_GPR + (s32)n * 4; }

// Offsets (instructions) of the shared routines in the code buffer
size_t s_post = 0, s_flush = 0, s_budget = 0, s_dispatch = 0, s_exit = 0, s_enter = 0;

// Block linking: exits with a static target jump straight to the target
// block's code. Sites are remembered per target physical address so they can
// be pointed back at the dispatcher when that code changes.
std::map<u32, std::vector<u32*>> s_incoming;

void PatchBranch(u32* site, u32 target) {
  s32 off = (s32)(target - Addr(site));
  *site = 0x48000000 | ((u32)off & 0x03FFFFFC);
  FlushCode(site, 1);
}

constexpr u32 MAX_IDS = 1u << 20;
u32* s_entries = nullptr;    // block id -> code address (0: not compiled)
const u32* s_map = nullptr;  // block cache map

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
// the cached interpreter's bookkeeping. On return:
//   cr0.eq: continue with the next instruction (r28 = pc, budget recomputed)
//   cr0.lt: branch taken, go to the dispatcher (r28 = target, budget valid)
//   cr0.gt: leave the generated code (state published, r28 = cpu.pc)
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
  // continue: r28 = new pc, budget; cr0 = taken branch ? lt : eq
  e.lwz(4, OFF_PC, R_CPU);
  e.addi(5, R_PC, 4);
  e.mr(R_PC, 4);
  e.cmplw(0, 4, 5);
  u32 not_taken = e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ);
  std::vector<u32> joins;
  EmitBudgetBody(e, joins);
  u32 j1 = e.Here();
  for (u32 j : joins) e.PatchBranchTo(j, j1);
  e.li(0, -1);
  e.cmpwi(0, 0, 0);  // lt
  e.mtlr(R_LINK);
  e.blr();
  e.PatchBranchTo(not_taken, e.Here());
  joins.clear();
  EmitBudgetBody(e, joins);
  u32 j2 = e.Here();
  for (u32 j : joins) e.PatchBranchTo(j, j2);
  e.cmpw(0, 0, 0);  // eq
  e.mtlr(R_LINK);
  e.blr();
  // leave: r28 = cpu.pc, cr0 = gt
  u32 at = e.Here();
  for (u32 l : leave) e.PatchBranchTo(l, at);
  e.lwz(R_PC, OFF_PC, R_CPU);
  e.li(0, 1);
  e.cmpwi(0, 0, 0);  // gt
  e.mtlr(R_LINK);
  e.blr();
}

// Finds the block at r28 and jumps to its code; anything unusual (end of the
// time slice, translation off or missing, block not compiled) goes to exit.
void EmitDispatch(PPCEmitter& e, std::vector<u32>& to_exit) {
  e.cmpwi(0, R_BUDGET, 0);
  to_exit.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_GT));
  e.lwz(0, OFF_MSR, R_CPU);
  e.andi_(0, 0, MSR_IR);
  to_exit.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  e.rlwinm(4, R_PC, 17, 15, 29);  // (pc >> 17) * 4 (rotate right 15)
  e.lwzx(5, R_IBAT, 4);
  e.cmpwi(0, 5, 0);
  to_exit.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  e.rlwimi(5, R_PC, 0, 15, 31);  // pa
  e.rlwinm(7, 5, 9, 23, 31);
  e.cmplwi(0, 7, 3);
  to_exit.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT));
  e.andi_(0, 5, 3);
  to_exit.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
  e.lwzx(6, R_MAP, 5);  // map[pa >> 2]: byte offset pa
  e.LoadImm(7, MAX_IDS);
  e.cmplw(0, 6, 7);
  to_exit.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT));
  e.cmpwi(0, 6, 0);
  to_exit.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  e.rlwinm(6, 6, 2, 0, 29);
  e.lwzx(7, R_ENTRY, 6);
  e.cmpwi(0, 7, 0);
  to_exit.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  e.mtctr(7);
  e.bctr();
}

bool AllocCode() {
  if (s_code) return true;
  s_code = (u32*)malloc(CODE_SIZE);
  s_entries = (u32*)calloc(MAX_IDS, sizeof(u32));
  return s_code != nullptr && s_entries != nullptr;
}

void Reset() {
  PPCEmitter e;
  s_budget = e.Here();
  EmitBudget(e);
  s_flush = e.Here();
  EmitFlush(e);
  s_post = e.Here();
  EmitPost(e);
  // dispatcher, then the exit (publish the state, epilogue)
  std::vector<u32> to_exit;
  s_dispatch = e.Here();
  EmitDispatch(e, to_exit);
  s_exit = e.Here();
  for (u32 at : to_exit) e.PatchBranchTo(at, s_exit);
  {
    s32 off = ((s32)s_flush - (s32)e.Here()) * 4;
    e.Emit(0x48000001 | ((u32)off & 0x03FFFFFC));  // bl flush
  }
  e.lwz(0, FRAME + 4, 1);
  e.mtlr(0);
  for (u32 r = 14; r <= 31; r++) e.lwz(r, 16 + (s32)(r - 14) * 4, 1);
  for (u32 f = 14; f <= 31; f++) e.lfd(f, FPR_SAVE + (s32)(f - 14) * 8, 1);
  e.addi(1, 1, FRAME);
  e.blr();
  // enter(): C entry point. Saves registers, loads the constants, then dispatches.
  s_enter = e.Here();
  e.stwu(1, -FRAME, 1);
  e.mflr(0);
  e.stw(0, FRAME + 4, 1);
  for (u32 r = 14; r <= 31; r++) e.stw(r, 16 + (s32)(r - 14) * 4, 1);
  for (u32 f = 14; f <= 31; f++) e.stfd(f, FPR_SAVE + (s32)(f - 14) * 8, 1);
  e.LoadImm(R_CPU, Addr(&cpu));
  e.LoadImm(R_MEM1, Addr(Mem::g_mem1));
  e.LoadImm(R_DBAT, Addr(Mem::DataBatTable()));
  e.LoadImm(R_STAMP, Addr(Mem::g_page_stamp));
  e.LoadImm(R_MAP, Addr(s_map));
  e.LoadImm(R_ENTRY, Addr(s_entries));
  e.LoadImm(R_IBAT, Addr(Mem::InstrBatTable()));
  e.lwz(R_PC, OFF_PC, R_CPU);
  e.li(R_PENDING, 0);
  e.mr(20, 3);  // keep the entry across the budget call (r20 is unused otherwise... saved below)
  {
    s32 off = ((s32)s_budget - (s32)e.Here()) * 4;
    e.Emit(0x48000001 | ((u32)off & 0x03FFFFFC));  // bl budget
  }
  e.mr(3, 20);
  // jump to the first block (r3, found by the C loop): it always runs, even
  // where the dispatcher could not translate the pc (real mode)
  e.mtctr(3);
  e.bctr();
  memcpy(s_code, e.code.data(), e.code.size() * 4);
  FlushCode(s_code, e.code.size());
  s_used = e.code.size();
  memset(s_entries, 0, MAX_IDS * sizeof(u32));
  s_incoming.clear();
}

// ---- Inlined instructions ----
// Operands go to host r3 (first source), r4 (second source), result in r6.
// Register cache of the block being compiled: host register per guest GPR
// (0 = in memory). Cached registers are loaded at block entry, written back
// before anything that reads the CPU state (interpreter calls, exits) and
// reloaded after interpreter calls.
u32 s_cache[32];
u32 s_cache_written;  // guest registers written in the block (bit mask)
bool s_cr_cached = false, s_cr_written = false;

inline void LoadCR(PPCEmitter& e, u32 h) {
  if (s_cr_cached)
    e.mr(h, R_CRC);
  else
    e.lwz(h, OFF_CR, R_CPU);
}
inline void StoreCR(PPCEmitter& e, u32 h) {
  if (s_cr_cached) {
    e.mr(R_CRC, h);
    s_cr_written = true;
  } else {
    e.stw(h, OFF_CR, R_CPU);
  }
}

inline void LoadGpr(PPCEmitter& e, u32 h, u32 g) {
  if (s_cache[g])
    e.mr(h, s_cache[g]);
  else
    e.lwz(h, Gpr(g), R_CPU);
}
inline void StoreGpr(PPCEmitter& e, u32 h, u32 g) {
  if (s_cache[g]) {
    e.mr(s_cache[g], h);
    s_cache_written |= 1u << g;
  } else {
    e.stw(h, Gpr(g), R_CPU);
  }
}
// Guest FPR halves cached in host f14-f31 (ps0, ps1 pairs)
u32 s_fcache[32];  // host FPR of ps0 (ps1 = +1), 0 = in memory
u64 s_fcache_written;  // bit 2*g + half
constexpr u32 FCACHE_FIRST = 14, FCACHE_PAIRS = 9;
const double kOneD = 1.0;

inline s32 FprOff(u32 g, int half) { return OFF_FPR + (s32)g * 16 + half * 8; }
// Host FPR holding guest g's half (loading it into `tmp` when not cached).
inline u32 FprRead(PPCEmitter& e, u32 tmp, u32 g, int half) {
  if (s_fcache[g]) return s_fcache[g] + half;
  e.lfd(tmp, FprOff(g, half), R_CPU);
  return tmp;
}
inline void FprWrite(PPCEmitter& e, u32 src, u32 g, int half) {
  if (s_fcache[g]) {
    if (s_fcache[g] + half != src) e.Emit(PPCEmitter::X(63, s_fcache[g] + half, 0, src, 72));  // fmr
    s_fcache_written |= 1ull << (2 * g + half);
  } else {
    e.stfd(src, FprOff(g, half), R_CPU);
  }
}

void CacheWriteBack(PPCEmitter& e) {
  for (u32 g = 0; g < 32; g++)
    if (s_cache[g] && (s_cache_written & (1u << g))) e.stw(s_cache[g], Gpr(g), R_CPU);
  if (s_cr_cached && s_cr_written) e.stw(R_CRC, OFF_CR, R_CPU);
  for (u32 g = 0; g < 32; g++)
    for (int h = 0; h < 2; h++)
      if (s_fcache[g] && (s_fcache_written & (1ull << (2 * g + h)))) e.stfd(s_fcache[g] + h, FprOff(g, h), R_CPU);
}
void CacheReload(PPCEmitter& e) {
  for (u32 g = 0; g < 32; g++)
    if (s_cache[g]) e.lwz(s_cache[g], Gpr(g), R_CPU);
  if (s_cr_cached) e.lwz(R_CRC, OFF_CR, R_CPU);
  for (u32 g = 0; g < 32; g++)
    if (s_fcache[g]) {
      e.lfd(s_fcache[g], FprOff(g, 0), R_CPU);
      e.lfd(s_fcache[g] + 1, FprOff(g, 1), R_CPU);
    }
}

// Chooses the guest registers to cache: the most used ones (at least twice).
void PlanCache(const u32* insts, u32 count) {
  u32 uses[32] = {};
  for (u32 k = 0; k < count; k++) {
    u32 in = insts[k], op = in >> 26;
    if (op == 16 || op == 18 || op == 17 || op == 19 || op == 1) continue;  // branches, sc, HLE
    uses[(in >> 21) & 31]++;
    uses[(in >> 16) & 31]++;
    if (op == 31 || op == 4) uses[(in >> 11) & 31]++;
  }
  // FPR uses: FP loads/stores, psq, FP arithmetic and paired singles
  u32 fuses[32] = {};
  for (u32 k = 0; k < count; k++) {
    u32 in = insts[k], op = in >> 26;
    if (op >= 48 && op <= 61) {
      fuses[(in >> 21) & 31]++;
    } else if (op == 59 || op == 63 || op == 4) {
      fuses[(in >> 21) & 31]++;
      fuses[(in >> 16) & 31]++;
      fuses[(in >> 11) & 31]++;
      fuses[(in >> 6) & 31]++;
    } else if (op == 31) {
      u32 xo = (in >> 1) & 0x3FF;
      if (xo == 535 || xo == 567 || xo == 599 || xo == 631 || xo == 663 || xo == 695 || xo == 727 || xo == 759)
        fuses[(in >> 21) & 31]++;
    }
  }
  memset(s_fcache, 0, sizeof(s_fcache));
  s_fcache_written = 0;
  for (u32 n = 0; n < FCACHE_PAIRS; n++) {
    u32 best = 32, best_uses = 1;
    for (u32 g = 0; g < 32; g++)
      if (!s_fcache[g] && fuses[g] > best_uses) best = g, best_uses = fuses[g];
    if (best == 32) break;
    s_fcache[best] = FCACHE_FIRST + 2 * n;
  }
  memset(s_cache, 0, sizeof(s_cache));
  s_cache_written = 0;
  for (u32 n = 0; n < CACHE_COUNT; n++) {
    u32 best = 32, best_uses = 1;
    for (u32 g = 0; g < 32; g++)
      if (!s_cache[g] && uses[g] > best_uses) best = g, best_uses = uses[g];
    if (best == 32) break;
    s_cache[best] = CACHE_FIRST + n;
  }
}

// Copies host cr0 (LT GT EQ) plus the guest XER.SO into guest CR field `f`.
void UpdateGuestCR(PPCEmitter& e, u32 f) {
  e.mfocrf_cr0(0);
  e.rlwinm(7, 0, 4, 28, 30);  // LT GT EQ -> bits 3..1 of a nibble
  e.lwz(8, OFF_XER, R_CPU);
  e.rlwinm(8, 8, 1, 31, 31);  // SO -> bit 0
  e.Emit(PPCEmitter::X(31, 7, 7, 8, 444));  // or r7, r7, r8
  LoadCR(e, 9);
  e.rlwimi(9, 7, (28 - 4 * f) & 31, 4 * f, 4 * f + 3);
  StoreCR(e, 9);
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
    case 8:   // subfic
    case 12:  // addic
    case 13:  // addic.
      // XER.CA: guest XER -> host XER, run, host XER -> guest
      LoadGpr(e, 3, ra);
      e.lwz(0, OFF_XER, R_CPU);
      e.mtxer(0);
      e.Emit(WithField(WithField(inst, 21, 6), 16, 3));
      e.mfxer(0);
      e.stw(0, OFF_XER, R_CPU);
      StoreGpr(e, 6, rd);
      if (op == 13) UpdateGuestCR(e, 0);
      return true;
    case 19: {  // CR logical operations on the cached CR
      u32 xo = (inst >> 1) & 0x3FF, host_xo;
      switch (xo) {
        case 257: host_xo = 28; break;   // crand  -> and
        case 129: host_xo = 60; break;   // crandc -> andc
        case 289: host_xo = 284; break;  // creqv  -> eqv
        case 225: host_xo = 476; break;  // crnand -> nand
        case 33: host_xo = 124; break;   // crnor  -> nor
        case 449: host_xo = 444; break;  // cror   -> or
        case 417: host_xo = 412; break;  // crorc  -> orc
        case 193: host_xo = 316; break;  // crxor  -> xor
        default: return false;
      }
      u32 bd = rd, ba = ra, bb = rb;
      LoadCR(e, 5);
      e.rlwinm(3, 5, (ba + 1) & 31, 31, 31);
      e.rlwinm(4, 5, (bb + 1) & 31, 31, 31);
      e.Emit(PPCEmitter::X(31, 3, 6, 4, host_xo));  // r6 = r3 op r4 (bit 0 matters)
      e.rlwimi(5, 6, (31 - bd) & 31, bd, bd);
      StoreCR(e, 5);
      return true;
    }
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
        // carrying arithmetic (OE = 0): rD = f(rA, rB, CA)
        case 10: case 138: case 8: case 136:     // addc, adde, subfc, subfe
        case 202: case 234: case 200: case 232:  // addze, addme, subfze, subfme
        {
          LoadGpr(e, 3, ra);
          LoadGpr(e, 4, rb);
          e.lwz(0, OFF_XER, R_CPU);
          e.mtxer(0);
          e.Emit(WithField(WithField(WithField(inst, 21, 6), 16, 3), 11, 4));
          e.mfxer(0);
          e.stw(0, OFF_XER, R_CPU);
          StoreGpr(e, 6, rd);
          if (rc) UpdateGuestCR(e, 0);
          return true;
        }
        case 792: case 824: {  // sraw, srawi: rA = f(rS, rB / sh), sets CA
          LoadGpr(e, 3, rd);
          if (xo == 792) LoadGpr(e, 4, rb);
          e.lwz(0, OFF_XER, R_CPU);
          e.mtxer(0);
          u32 h = WithField(WithField(inst, 21, 3), 16, 6);
          if (xo == 792) h = WithField(h, 11, 4);
          e.Emit(h);
          e.mfxer(0);
          e.stw(0, OFF_XER, R_CPU);
          StoreGpr(e, 6, ra);
          if (rc) UpdateGuestCR(e, 0);
          return true;
        }
        case 19:  // mfcr
          LoadCR(e, 6);
          StoreGpr(e, 6, rd);
          return true;
        case 144: {  // mtcrf
          u32 crm = (inst >> 12) & 0xFF, mask = 0;
          for (int f = 0; f < 8; f++)
            if (crm & (0x80 >> f)) mask |= 0xF0000000u >> (4 * f);
          LoadGpr(e, 3, rd);
          LoadCR(e, 4);
          e.LoadImm(5, mask);
          e.Emit(PPCEmitter::X(31, 3, 3, 5, 28));   // and r3, r3, r5
          e.Emit(PPCEmitter::X(31, 4, 4, 5, 60));   // andc r4, r4, r5
          e.Emit(PPCEmitter::X(31, 3, 3, 4, 444));  // or r3, r3, r4
          StoreCR(e, 3);
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
        case 467: {  // mtspr without side effects (see Interpreter::WriteSPR)
          u32 spr = ((inst >> 16) & 31) | (((inst >> 11) & 31) << 5);
          if (spr == SPR_DEC || spr == SPR_TBL_W || spr == SPR_TBU_W || spr == SPR_PVR || spr == SPR_WPAR ||
              spr == SPR_DMAL || (spr >= SPR_IBAT0U && spr < SPR_IBAT0U + 16) || spr >= 1024)
            return false;
          LoadGpr(e, 3, rd);
          e.stw(3, spr == SPR_XER ? OFF_XER : OFF_SPR + (s32)spr * 4, R_CPU);
          return true;
        }
        case 83:  // mfmsr (mtmsr stays interpreted: it may unmask a pending interrupt)
          e.lwz(6, OFF_MSR, R_CPU);
          StoreGpr(e, 6, rd);
          return true;
        case 54: case 86: case 246: case 278: case 470: case 310: case 438:  // cache hints: no-ops
          return true;
        case 339: {  // mfspr without side effects (see Interpreter::ReadSPR)
          u32 spr = ((inst >> 16) & 31) | (((inst >> 11) & 31) << 5);
          if (spr == SPR_DEC || spr == SPR_TBL_R || spr == SPR_TBU_R || spr == SPR_WPAR || spr >= 1024) return false;
          e.lwz(6, spr == SPR_XER ? OFF_XER : OFF_SPR + (s32)spr * 4, R_CPU);
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

enum FpKind { FK_ADD, FK_SUB, FK_MUL, FK_DIV, FK_MADD, FK_MSUB, FK_NMADD, FK_NMSUB, FK_COPYC };
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
      case 10: f = {2, true, {FK_ADD, 0, 1, -1}, {FK_COPYC, -1, -1, 1}}; return true;   // ps_sum0
      case 11: f = {2, true, {FK_COPYC, -1, -1, 0}, {FK_ADD, 0, 1, -1}}; return true;   // ps_sum1
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
  (void)fpr;
  if (t.kind == FK_COPYC) {
    u32 c = FprRead(e, 3, rc, t.c);
    if (round)
      e.Emit(PPCEmitter::X(63, 0, 0, c, 12));  // frsp f0, c
    else
      e.Emit(PPCEmitter::X(63, 0, 0, c, 72));  // fmr
    return;
  }
  bool uses_c = t.kind == FK_MUL || (t.kind >= FK_MADD && t.kind <= FK_NMSUB);
  bool uses_b = t.kind != FK_MUL;
  u32 fa = FprRead(e, 1, ra, t.a);
  u32 fb = uses_b ? FprRead(e, 2, rb, t.b) : 0;
  u32 fc = uses_c ? FprRead(e, 3, rc, t.c) : 0;
  auto A = [&](u32 xo, u32 frt, u32 fra, u32 frb, u32 frc) { e.Emit(PPCEmitter::A(63, frt, fra, frb, frc, xo)); };
  switch (t.kind) {
    case FK_ADD: A(21, 0, fa, fb, 0); break;
    case FK_SUB: A(20, 0, fa, fb, 0); break;
    case FK_MUL: A(25, 0, fa, 0, fc); break;
    case FK_DIV: A(18, 0, fa, fb, 0); break;
    default:
      if (kFused) {
        u32 xo = t.kind == FK_MADD ? 29 : t.kind == FK_MSUB ? 28 : t.kind == FK_NMADD ? 31 : 30;
        A(xo, 0, fa, fb, fc);
      } else {
        A(25, 0, fa, 0, fc);  // f0 = a * c
        if (t.kind == FK_MADD || t.kind == FK_NMADD)
          A(21, 0, 0, fb, 0);  // + b
        else
          A(20, 0, 0, fb, 0);  // - b
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
  e.rlwinm(4, 3, 17, 15, 29);
  e.lwzx(5, R_DBAT, 4);
  e.cmpwi(0, 5, 0);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  e.rlwimi(5, 3, 0, 15, 31);
  e.addi(7, 5, 8);
  e.rlwinm(7, 7, 9, 23, 31);  // (pa + 8) >> 23 < 3: both elements in MEM1
  e.cmplwi(0, 7, 3);
  slow.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT));
  if (!store) {
    e.lfsx(0, R_MEM1, 5);
    if (w) {
      e.LoadImm(6, Addr(&kOneD));
      e.lfd(1, 0, 6);
    } else {
      e.addi(6, 5, 4);
      e.lfsx(1, R_MEM1, 6);
    }
    FprWrite(e, 0, rd, 0);
    FprWrite(e, 1, rd, 1);
  } else {
    u32 f0 = FprRead(e, 0, rd, 0);
    e.Emit(PPCEmitter::X(63, 0, 0, f0, 12));  // frsp f0, ps0
    e.stfsx(0, R_MEM1, 5);
    if (!w) {
      u32 f1 = FprRead(e, 1, rd, 1);
      e.Emit(PPCEmitter::X(63, 1, 0, f1, 12));  // frsp f1, ps1
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

// Register moves and sign manipulation (raw bits), merges and frsp.
bool EmitFpMove(PPCEmitter& e, u32 inst, std::vector<u32>& slow) {
  if (inst & 1) return false;
  u32 op = inst >> 26, xo = (inst >> 1) & 0x3FF;
  u32 rd = Field(inst, 21), ra = Field(inst, 16), rb = Field(inst, 11);
  auto fpr = [](u32 r, int half) { return OFF_FPR + (s32)r * 16 + half * 8; };
  int kind;  // 0 mr, 1 neg, 2 abs, 3 nabs
  bool pair;
  if ((op == 63 || op == 4) && (xo == 72 || xo == 40 || xo == 264 || xo == 136)) {
    kind = xo == 72 ? 0 : xo == 40 ? 1 : xo == 264 ? 2 : 3;
    pair = op == 4;
  } else if (op == 4 && (xo == 528 || xo == 560 || xo == 592 || xo == 624)) {
    kind = -1;
    pair = true;
  } else if (op == 63 && xo == 12) {
    kind = -2;  // frsp
    pair = false;
  } else {
    return false;
  }
  e.lwz(0, OFF_MSR, R_CPU);
  e.andi_(0, 0, MSR_FP);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  if (kind == -2) {  // frsp: both halves = RoundSingle(B)
    u32 fb = FprRead(e, 1, rb, 0);
    e.Emit(PPCEmitter::X(63, 0, 0, fb, 12));
    FprWrite(e, 0, rd, 0);
    FprWrite(e, 0, rd, 1);
    return true;
  }
  if (kind == -1) {  // ps_merge: ps0 from A (half a), ps1 from B (half b), read both first
    int ha = (xo == 592 || xo == 624) ? 1 : 0, hb = (xo == 560 || xo == 624) ? 1 : 0;
    u32 fa = FprRead(e, 1, ra, ha), fb = FprRead(e, 2, rb, hb);
    e.Emit(PPCEmitter::X(63, 3, 0, fa, 72));  // fmr f3, a
    e.Emit(PPCEmitter::X(63, 4, 0, fb, 72));  // fmr f4, b
    FprWrite(e, 3, rd, 0);
    FprWrite(e, 4, rd, 1);
    return true;
  }
  // fmr / fneg / fabs / fnabs are exact bit operations on the host too
  static const u32 kXo[4] = {72, 40, 264, 136};
  u32 t[2];
  for (int h = 0; h < (pair ? 2 : 1); h++) {
    u32 fb = FprRead(e, 1 + h, rb, h);
    t[h] = 3 + h;
    e.Emit(PPCEmitter::X(63, t[h], 0, fb, kXo[kind]));
  }
  for (int h = 0; h < (pair ? 2 : 1); h++) FprWrite(e, t[h], rd, h);
  return true;
}

// fctiwz: the host conversion saturates and handles NaN like the interpreter;
// the high word is the interpreter's constant.
bool EmitFctiwz(PPCEmitter& e, u32 inst, std::vector<u32>& slow) {
  if ((inst >> 26) != 63 || ((inst >> 1) & 0x3FF) != 15 || (inst & 1)) return false;
  u32 rd = Field(inst, 21), rb = Field(inst, 11);
  e.lwz(0, OFF_MSR, R_CPU);
  e.andi_(0, 0, MSR_FP);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  u32 fb = FprRead(e, 1, rb, 0);
  e.Emit(PPCEmitter::X(63, 0, 0, fb, 15));  // fctiwz f0, fb
  e.stfd(0, 8, 1);                          // scratch slot in our stack frame: keep the low word,
  e.LoadImm(4, 0xFFF80000);                 // replace the high word with the interpreter's constant
  e.stw(4, 8, 1);
  e.lfd(0, 8, 1);
  FprWrite(e, 0, rd, 0);
  return true;
}

// fcmpu / fcmpo: the host comparison yields the same LT GT EQ UN code
bool EmitFpCompare(PPCEmitter& e, u32 inst, std::vector<u32>& slow) {
  u32 op = inst >> 26, xo = (inst >> 1) & 0x3FF;
  if (op != 63 || (xo != 0 && xo != 32)) return false;
  u32 crf = (inst >> 23) & 7, ra = Field(inst, 16), rb = Field(inst, 11);
  e.lwz(0, OFF_MSR, R_CPU);
  e.andi_(0, 0, MSR_FP);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  u32 fa = FprRead(e, 1, ra, 0), fb = FprRead(e, 2, rb, 0);
  e.Emit(PPCEmitter::X(63, 0, fa, fb, 0));  // fcmpu cr0, fa, fb
  e.mfocrf_cr0(0);
  e.rlwinm(7, 0, 4, 28, 31);  // code in the low nibble
  LoadCR(e, 9);
  e.rlwimi(9, 7, (28 - 4 * crf) & 31, 4 * crf, 4 * crf + 3);
  StoreCR(e, 9);
  // FPSCR: clear FPRF's C bit (0x10000), FPCC = code
  e.lwz(9, OFF_FPSCR, R_CPU);
  e.rlwinm(9, 9, 0, 16, 14);
  e.rlwimi(9, 7, 12, 16, 19);
  e.stw(9, OFF_FPSCR, R_CPU);
  return true;
}

bool EmitFpFast(PPCEmitter& e, u32 inst, std::vector<u32>& slow) {
  if (EmitFpMove(e, inst, slow)) return true;
  if (EmitFpCompare(e, inst, slow)) return true;
  if (EmitFctiwz(e, inst, slow)) return true;
  FpOp f;
  if ((inst & 1) || !DecodeFpOp(inst, f)) return false;  // Rc forms are left to the interpreter
  u32 rd = Field(inst, 21);
  s32 dst = OFF_FPR + (s32)rd * 16;
  e.lwz(0, OFF_MSR, R_CPU);
  e.andi_(0, 0, MSR_FP);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  (void)dst;
  if (f.dest == 2) {
    // both halves read the sources before writing (rd may be a source)
    EmitFpTerm(e, inst, f.t0, f.round);
    e.Emit(PPCEmitter::X(63, 4, 0, 0, 72));  // fmr f4, f0
    EmitFpTerm(e, inst, f.t1, f.round);
    FprWrite(e, 4, rd, 0);
    FprWrite(e, 0, rd, 1);
  } else {
    EmitFpTerm(e, inst, f.t0, f.round);
    FprWrite(e, 0, rd, 0);
    if (f.dest == 1) FprWrite(e, 0, rd, 1);
  }
  return true;
}

// Branches (always the last instruction of a block). On return r28 holds the
// next pc. The forms that the interpreter uses for idle-loop detection
// ("b ." and bc -8) are left to the interpreter.
// Static exit emitter supplied by Compile: leaves the block towards
// r28 + offset (bookkeeping, budget check, linkable jump).
struct StaticExits {
  bool enabled = false;  // false: CanInline statistics only
  u32 inst_pa = 0;       // physical address of the branch instruction
  std::function<void(s32 offset, u32 target_pa)> emit;
};
StaticExits* s_static = nullptr;

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
    if (!aa && s_static && s_static->enabled) {
      s_static->emit(li, s_static->inst_pa + (u32)li);
      return true;
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
    LoadCR(e, 6);
    e.rlwinm(6, 6, (bi + 1) & 31, 31, 31);
    e.cmpwi(0, 6, (s32)((bo >> 3) & 1));
    fail.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_EQ));
  }
  // relative bc with static exits: one linkable exit per outcome
  if (is_bc && !aa && s_static && s_static->enabled) {
    if (lk) {
      e.addi(4, R_PC, 4);
      e.stw(4, OFF_LR, R_CPU);
    }
    s_static->emit(bd, s_static->inst_pa + (u32)bd);
    u32 fail_at = e.Here();
    for (u32 f : fail) e.PatchBranchTo(f, fail_at);
    if (lk) {
      e.addi(4, R_PC, 4);
      e.stw(4, OFF_LR, R_CPU);
    }
    s_static->emit(4, s_static->inst_pa + 4);
    return true;
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
  e.rlwinm(4, 3, 17, 15, 29);  // (ea >> 17) * 4
  e.lwzx(5, R_DBAT, 4);
  e.cmpwi(0, 5, 0);
  slow.push_back(e.bc_forward(PPCEmitter::BO_TRUE, PPCEmitter::CR0_EQ));
  // pa = block base | (ea & 0x1FFFF); MEM1 only (pa < 24 MiB: pa >> 23 < 3)
  e.rlwimi(5, 3, 0, 15, 31);
  e.rlwinm(7, 5, 9, 23, 31);  // pa >> 23
  e.cmplwi(0, 7, 3);
  slow.push_back(e.bc_forward(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT));
  if (m.store && m.fp == 2) {
    u32 fs = FprRead(e, 0, rd, 0);
    e.stfdx(fs, R_MEM1, 5);  // 8-byte aligned (checked above)
  } else if (m.store && m.fp == 1) {
    u32 fs = FprRead(e, 0, rd, 0);
    e.Emit(PPCEmitter::X(63, 0, 0, fs, 12));  // frsp f0, fs
    e.stfsx(0, R_MEM1, 5);
  }
  if (m.store && m.fp) {
    e.LoadImm(8, Addr(&Mem::g_write_stamp));
    e.lwz(8, 0, 8);
    e.rlwinm(9, 5, 22, 10, 29);
    e.stwx(8, R_STAMP, 9);
  } else if (!m.store && m.fp == 2) {
    e.lfdx(0, R_MEM1, 5);
    FprWrite(e, 0, rd, 0);
  } else if (!m.store && m.fp == 1) {
    e.lfsx(0, R_MEM1, 5);
    FprWrite(e, 0, rd, 0);
    FprWrite(e, 0, rd, 1);
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
void SetBlockMap(const u32* map) {
  if (map != s_map) {
    s_map = map;
    if (s_code) Reset();
  }
}

void Clear() {
  if (s_code) Reset();
}
size_t CodeBytes() { return s_used * 4; }

bool CanInline(u32 inst, bool last) {
  PPCEmitter e;
  std::vector<u32> slow;
  memset(s_cache, 0, sizeof(s_cache));
  memset(s_fcache, 0, sizeof(s_fcache));
  s_cr_cached = false;
  MemOp m;
  if (last && EmitBranch(e, inst)) return true;
  if (DecodeMemOp(inst, m)) return true;
  return EmitFpFast(e, inst, slow) || EmitPsqFast(e, inst, slow) || EmitInline(e, inst);
}

bool HasBlock(u32 id) { return s_entries && id < MAX_IDS && s_entries[id]; }

void Run(u32 id) {
  using EnterFn = void (*)(u32 entry);
  ((EnterFn)(void*)(s_code + s_enter))(s_entries[id]);
}

void Unlink(u32 pa, u32 len) {
  if (!s_code) return;
  u32 dispatch = Addr(s_code + s_dispatch);
  for (auto it = s_incoming.lower_bound(pa); it != s_incoming.end() && it->first < pa + len; ++it)
    for (u32* site : it->second) PatchBranch(site, dispatch);
}

void UnlinkAll() {
  if (!s_code) return;
  u32 dispatch = Addr(s_code + s_dispatch);
  for (auto& kv : s_incoming)
    for (u32* site : kv.second) PatchBranch(site, dispatch);
}

// EMUGC_JIT_PROF=1: counts the instructions still run by the interpreter
// (by name; "+slow" when the inline fast path bailed out), printed at exit.
std::map<std::string, u64>* s_prof;
void PrintProfile() {
  std::vector<std::pair<u64, std::string>> v;
  u64 total = 0;
  for (auto& [name, n] : *s_prof) v.push_back({n, name}), total += n;
  std::sort(v.rbegin(), v.rend());
  printf("[jit] interpreted instructions: %llu\n", (unsigned long long)total);
  for (size_t i = 0; i < v.size() && i < 30; i++) printf("[jit] %12llu %s\n", (unsigned long long)v[i].first, v[i].second.c_str());
}
void ProfiledCall(u32 inst, Interpreter::OpFn fn, u32 slow) {
  std::string name = Interpreter::GetOpName(inst);
  if (slow) {  // memory access: also the 1 MiB region of the address
    u32 op = inst >> 26, ra = (inst >> 16) & 31;
    u32 ea = op == 31 ? (ra ? cpu.gpr[ra] : 0) + cpu.gpr[(inst >> 11) & 31]
                      : (ra ? cpu.gpr[ra] : 0) + (u32)(s32)(s16)inst;
    if (op == 56 || op == 57 || op == 60 || op == 61) ea = (ra ? cpu.gpr[ra] : 0) + (u32)((s32)(inst << 20) >> 20);
    char buf[32];
    snprintf(buf, sizeof(buf), "+slow@%03x dr%d bat%d al%d", ea >> 20, (cpu.msr & MSR_DR) != 0,
             Mem::DataBatTable()[ea >> 17] != 0, (int)(ea & 7));
    name += buf;
  }
  (*s_prof)[name]++;
  fn(inst);
}
bool ProfilingEnabled() {
  static int on = -1;
  if (on < 0) {
    on = getenv("EMUGC_JIT_PROF") != nullptr;
    if (on) {
      s_prof = new std::map<std::string, u64>;
      atexit(PrintProfile);
    }
  }
  return on;
}

bool Compile(u32 id, u32 block_pa, const u32* insts, const Interpreter::OpFn* fns, u32 count) {
  if (!s_enabled || !s_map || id >= MAX_IDS) return true;  // stays interpreted
  if (!s_code) {
    if (!AllocCode()) {
      s_enabled = false;
      return true;
    }
    Reset();
  }
  PPCEmitter e;
  e.code.reserve(32 + count * 16);
  struct Jump {
    u32 at;
    size_t target;
    bool link;
  };
  std::vector<Jump> jumps;  // to shared routines, patched once placed
  auto call = [&](size_t target) {
    jumps.push_back({e.Here(), target, true});
    e.Emit(0);
  };
  auto jump = [&](size_t target) {
    jumps.push_back({e.Here(), target, false});
    e.Emit(0);
  };
  // conditional jump to a far routine: skip over an unconditional branch
  auto jump_unless = [&](u32 bo, u32 bi, size_t target) {
    u32 skip = e.bc_forward(bo, bi);
    jump(target);
    e.PatchBranchTo(skip, e.Here());
  };
  struct LinkSite {
    u32 at;
    u32 target_pa;
  };
  std::vector<LinkSite> links;
  // Leaves the block for a statically known target (same 128 KiB BAT block
  // as this one, so the translation is linear): linkable jump.
  auto linkable_jump = [&](u32 target_pa) {
    if ((target_pa >> 17) == (block_pa >> 17) && target_pa < Mem::MEM1_SIZE && !(target_pa & 3))
      links.push_back({e.Here(), target_pa});
    jump(s_dispatch);
  };
  StaticExits sx;
  sx.enabled = s_inline_enabled;
  sx.emit = [&](s32 offset, u32 target_pa) {
    if (offset >= -32768 && offset < 32768) {
      e.addi(R_PC, R_PC, offset);
    } else {
      e.LoadImm(4, (u32)offset);
      e.add(R_PC, R_PC, 4);
    }
    e.addi(R_PENDING, R_PENDING, 1);
    e.Emit(PPCEmitter::D(13, R_BUDGET, R_BUDGET, -1));  // addic. r30, r30, -1
    CacheWriteBack(e);
    jump_unless(PPCEmitter::BO_TRUE, PPCEmitter::CR0_GT, s_exit);  // end of the slice
    linkable_jump(target_pa);
  };
  s_static = &sx;

  PlanCache(insts, count);
  if (!s_inline_enabled) {
    memset(s_cache, 0, sizeof(s_cache));
    memset(s_fcache, 0, sizeof(s_fcache));
  }
  s_cr_cached = s_inline_enabled;
  s_cr_written = false;
  // Block entry: the whole block must fit in the time slice, so that no
  // instruction needs its own budget check (the C loop interprets blocks the
  // slice ends inside, exactly like the interpreter). Memory holds the state.
  e.cmpwi(0, R_BUDGET, (s32)count);
  jump_unless(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT, s_exit);
  CacheReload(e);
  // pc, pending cycles and budget are brought up to date lazily: `unsynced`
  // inlined instructions are not counted in them yet (r28 + 4 * unsynced is
  // the current instruction).
  u32 unsynced = 0;
  auto sync_by = [&](u32 n) {
    if (!n) return;
    e.addi(R_PC, R_PC, (s32)(4 * n));
    e.addi(R_PENDING, R_PENDING, (s32)n);
    e.addi(R_BUDGET, R_BUDGET, -(s32)n);
  };
  auto sync = [&]() {
    sync_by(unsynced);
    unsynced = 0;
  };

  bool ended = false;
  for (u32 k = 0; k < count && !ended; k++) {
    bool last = k + 1 == count;
    MemOp m;
    u32 ra = Field(insts[k], 16), rd = Field(insts[k], 21);
    std::vector<u32> slow;
    bool fast = false;
    if (s_inline_enabled) {
      sx.inst_pa = block_pa + 4 * k;
      if (last) sync();  // branches use r28 as their own address
      u32 before = e.Here();
      if (last && EmitBranch(e, insts[k])) {
        // static exits emitted their own tails; dynamic targets (bclr, bcctr,
        // absolute) go through the dispatcher
        bool static_tail = !links.empty() && links.back().at >= before;
        bool any_static_jump = false;
        for (const Jump& j : jumps)
          if (j.at >= before && j.target == s_dispatch) any_static_jump = true;
        if (!static_tail && !any_static_jump) {
          e.addi(R_PENDING, R_PENDING, 1);
          e.Emit(PPCEmitter::D(13, R_BUDGET, R_BUDGET, -1));  // addic. r30, r30, -1
          CacheWriteBack(e);
          jump(s_dispatch);
        }
        ended = true;
        continue;
      }
      if (DecodeMemOp(insts[k], m) && !(m.update && (ra == 0 || (!m.store && !m.fp && ra == rd)))) {
        EmitMemFast(e, insts[k], m, slow);
        fast = true;
      } else if (EmitFpFast(e, insts[k], slow)) {
        fast = true;
      } else if (EmitPsqFast(e, insts[k], slow)) {
        fast = true;
      } else if (EmitInline(e, insts[k])) {
        fast = true;
      }
    }
    u32 to_next = 0;
    bool has_fast_path = fast;
    u32 before_this = unsynced;  // not counting this instruction
    if (fast) {
      unsynced++;
      if (slow.empty()) continue;  // no fallback needed
      to_next = e.b_forward();
    }
    // Interpreted (or the slow path of a fast instruction)
    u32 slow_at = e.Here();
    for (u32 at : slow) e.PatchBranchTo(at, slow_at);
    sync_by(before_this);
    CacheWriteBack(e);  // the handler works on the CPU state in memory
    call(s_flush);
    e.LoadImm(3, insts[k]);
    if (ProfilingEnabled()) {
      e.LoadImm(4, Addr((const void*)fns[k]));
      e.LoadImm(5, has_fast_path);
      e.CallAbs(Addr((const void*)ProfiledCall));
    } else {
      e.CallAbs(Addr((const void*)fns[k]));
    }
    call(s_post);
    jump_unless(PPCEmitter::BO_FALSE, PPCEmitter::CR0_GT, s_exit);  // gt: leave
    if (last) {
      jump(s_dispatch);  // eq or lt: the dispatcher continues at r28
      ended = true;
    } else {
      jump_unless(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT, s_dispatch);  // lt: branch taken
      // the handler may have moved the end of the slice (an event scheduled
      // by a hardware register write): the rest of the block must still fit
      e.cmpwi(0, R_BUDGET, (s32)(count - k - 1));
      jump_unless(PPCEmitter::BO_FALSE, PPCEmitter::CR0_LT, s_exit);
      CacheReload(e);  // the handler may have changed any register
      // r28 and the counters are now exact (past this instruction); the fast
      // path joins below with `unsynced` instructions not counted: match it
      if (has_fast_path) sync_by((u32)-(s32)unsynced);
    }
    if (has_fast_path) {
      e.PatchBranchTo(to_next, e.Here());
      if (last) {  // the fast path of the last instruction
        sync();
        CacheWriteBack(e);
        jump(s_dispatch);
      }
    } else {
      unsynced = 0;
    }
  }
  if (!ended) {  // end of the block after an inlined instruction: falls into the next one
    sync();
    CacheWriteBack(e);
    linkable_jump(block_pa + 4 * count);
  }

  s_static = nullptr;
  if (s_used + e.code.size() > CODE_SIZE / 4) return false;
  u32* dst = s_code + s_used;
  for (const Jump& j : jumps) {
    s32 off = ((s32)j.target - (s32)(s_used + j.at)) * 4;
    e.code[j.at] = 0x48000000 | ((u32)off & 0x03FFFFFC) | (j.link ? 1u : 0u);
  }
  memcpy(dst, e.code.data(), e.code.size() * 4);
  FlushCode(dst, e.code.size());
  s_used += e.code.size();
  s_entries[id] = Addr(dst);
  s_static = nullptr;
  // Link this block's static exits to compiled targets, and the exits waiting
  // for this block to it.
  for (const LinkSite& l : links) {
    u32* site = dst + l.at;
    s_incoming[l.target_pa].push_back(site);
    u32 tid = s_map[l.target_pa >> 2];
    if (tid && tid < MAX_IDS && s_entries[tid]) PatchBranch(site, s_entries[tid]);
  }
  auto inc = s_incoming.find(block_pa);
  if (inc != s_incoming.end())
    for (u32* site : inc->second) PatchBranch(site, Addr(dst));
  // Debugging: EMUGC_JIT_DUMP=N writes block N (guest, then host code) to jit_block.bin
  static int dump_at = getenv("EMUGC_JIT_DUMP") ? atoi(getenv("EMUGC_JIT_DUMP")) : -1;
  if ((int)id == dump_at) {
    if (FILE* f = fopen("jit_block.bin", "wb")) {
      u32 hdr[2] = {count, (u32)e.code.size()};
      fwrite(hdr, 4, 2, f);
      fwrite(insts, 4, count, f);
      fwrite(e.code.data(), 4, e.code.size(), f);
      fclose(f);
    }
  }
  return true;
}

}  // namespace Jit
