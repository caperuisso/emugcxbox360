// emugcxbox360 - Gekko interpreter unit tests
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "core/coretiming.h"
#include "core/gekko/cpu.h"
#include "core/hw/hw.h"
#include "core/memory.h"
#include "core/system.h"

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK_EQ(actual, expected)                                                              \
  do {                                                                                          \
    g_checks++;                                                                                 \
    auto a_ = (actual);                                                                         \
    auto e_ = (expected);                                                                       \
    if (!(a_ == e_)) {                                                                          \
      g_failures++;                                                                             \
      printf("  FAIL %s:%d: %s == 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #actual,       \
             (unsigned long long)a_, (unsigned long long)e_);                                   \
    }                                                                                           \
  } while (0)

#define CHECK_NEAR(actual, expected)                                                            \
  do {                                                                                          \
    g_checks++;                                                                                 \
    double a_ = (actual), e_ = (expected);                                                      \
    if (std::fabs(a_ - e_) > 1e-6) {                                                            \
      g_failures++;                                                                             \
      printf("  FAIL %s:%d: %s == %f, expected %f\n", __FILE__, __LINE__, #actual, a_, e_);     \
    }                                                                                           \
  } while (0)

class NullHost : public Host {
 public:
  void Log(const char* msg) override { fputs(msg, stdout); }
  void PresentFrame(const u32*, int, int) override {}
  void PollPad(int, PadState& p) override { p.connected = true; }
};

// ---- Tiny PowerPC encoder ----
u32 D(u32 op, u32 rt, u32 ra, s32 imm) { return (op << 26) | (rt << 21) | (ra << 16) | ((u32)imm & 0xFFFF); }
u32 X(u32 rt, u32 ra, u32 rb, u32 xo, u32 rc = 0) {
  return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}
u32 XO(u32 rt, u32 ra, u32 rb, u32 xo, u32 oe = 0, u32 rc = 0) {
  return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (oe << 10) | (xo << 1) | rc;
}
u32 M(u32 op, u32 rs, u32 ra, u32 sh, u32 mb, u32 me, u32 rc = 0) {
  return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc;
}
u32 A(u32 op, u32 frt, u32 fra, u32 frb, u32 frc, u32 xo, u32 rc = 0) {
  return (op << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1) | rc;
}
u32 FX(u32 op, u32 frt, u32 fra, u32 frb, u32 xo) { return (op << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (xo << 1); }
u32 PSQ(u32 op, u32 frt, u32 ra, u32 w, u32 i, s32 d) {
  return (op << 26) | (frt << 21) | (ra << 16) | (w << 15) | (i << 12) | ((u32)d & 0xFFF);
}
u32 SPR(u32 xo, u32 r, u32 spr) { return (31u << 26) | (r << 21) | ((spr & 0x1F) << 16) | ((spr >> 5) << 11) | (xo << 1); }
u32 li(u32 r, s32 v) { return D(14, r, 0, v); }
u32 lis(u32 r, s32 v) { return D(15, r, 0, v); }
u32 ori(u32 r, u32 s, u32 v) { return D(24, s, r, (s32)v); }
u32 mtspr(u32 spr, u32 r) { return SPR(467, r, spr); }
u32 mfspr(u32 r, u32 spr) { return SPR(339, r, spr); }
u32 bc(u32 bo, u32 bi, s32 off) { return (16u << 26) | (bo << 21) | (bi << 16) | ((u32)off & 0xFFFC); }
u32 b(s32 off) { return (18u << 26) | ((u32)off & 0x03FFFFFC); }
constexpr u32 SC = 0x44000002;
constexpr u32 CODE = 0x80003100;
constexpr u32 DATA = 0x80100000;

void ResetMachine() {
  CoreTiming::Init();
  Mem::Clear();
  CPU::Reset();
  HW::Reset();
  cpu.spr[SPR_IBAT0U] = 0x80001FFF;
  cpu.spr[SPR_IBAT0U + 1] = 0x00000002;
  cpu.spr[SPR_DBAT0U] = 0x80001FFF;
  cpu.spr[SPR_DBAT0U + 1] = 0x00000002;
  cpu.spr[SPR_DBAT0U + 2] = 0xC0001FFF;
  cpu.spr[SPR_DBAT0U + 3] = 0x0000002A;
  Mem::UpdateBATs();
  cpu.msr = MSR_FP | MSR_IR | MSR_DR | MSR_ME;
}

// Runs a program at CODE until it reaches the trailing "b ." or the step limit.
u32 Run(std::initializer_list<u32> program, int max_steps = 100000) {
  u32 addr = CODE;
  for (u32 inst : program) {
    Mem::Write32(addr, inst);
    addr += 4;
  }
  Mem::Write32(addr, b(0));
  cpu.pc = CODE;
  for (int i = 0; i < max_steps && cpu.pc != addr; i++) CPU::Step();
  return addr;
}

void TestIntegerArithmetic() {
  puts("integer arithmetic");
  ResetMachine();
  Run({li(3, -1), D(12, 4, 3, 1) /* addic r4,r3,1 */, li(5, 7), li(6, 0),
       XO(7, 5, 6, 138) /* adde r7,r5,r6 */, li(8, 100), li(9, 58), XO(10, 9, 8, 40) /* subf r10,r9,r8 */,
       XO(11, 8, 9, 235) /* mullw */, lis(12, 0x7FFF), ori(12, 12, 0xFFFF), li(13, 1),
       XO(14, 12, 13, 266, 1, 1) /* addo. r14,r12,r13 */});
  CHECK_EQ(cpu.gpr[4], 0u);
  CHECK_EQ(cpu.gpr[7], 8u);  // 7 + 0 + carry
  CHECK_EQ(cpu.gpr[10], 42u);
  CHECK_EQ(cpu.gpr[11], 5800u);
  CHECK_EQ(cpu.gpr[14], 0x80000000u);
  CHECK_EQ(cpu.xer & (XER_OV | XER_SO), (u32)(XER_OV | XER_SO));
  CHECK_EQ(cpu.cr >> 28, 0x9u);  // LT | SO

  ResetMachine();
  Run({li(3, -7), li(4, 2), XO(5, 3, 4, 491) /* divw */, XO(6, 3, 4, 459) /* divwu */, li(7, 0),
       XO(8, 3, 7, 491) /* divw by 0 */, lis(9, 0x1234), ori(9, 9, 0x5678), X(9, 10, 0, 26) /* cntlzw */,
       li(11, -5), (31u << 26) | (11 << 21) | (12 << 16) | (1 << 11) | (824 << 1) /* srawi r12,r11,1 */});
  CHECK_EQ(cpu.gpr[5], (u32)-3);
  CHECK_EQ(cpu.gpr[6], 0x7FFFFFFCu);
  CHECK_EQ(cpu.gpr[8], 0xFFFFFFFFu);
  CHECK_EQ(cpu.gpr[10], 3u);
  CHECK_EQ(cpu.gpr[12], (u32)-3);
  CHECK_EQ((cpu.xer & XER_CA) != 0, true);
}

void TestRotates() {
  puts("rotates and logic");
  ResetMachine();
  Run({lis(3, 0x1234), ori(3, 3, 0x5678), M(21, 3, 4, 8, 24, 31) /* rlwinm r4,r3,8,24,31 */,
       M(21, 3, 5, 0, 16, 31) /* clrlwi r5,r3,16 */, lis(6, 0xFFFF), ori(6, 6, 0xFFFF),
       M(20, 3, 6, 16, 0, 15) /* rlwimi r6,r3,16,0,15 */, X(3, 7, 3, 316) /* xor r7,r3,r3 */,
       M(21, 3, 8, 4, 28, 3) /* wrap-around mask */});
  CHECK_EQ(cpu.gpr[4], 0x12u);
  CHECK_EQ(cpu.gpr[5], 0x5678u);
  CHECK_EQ(cpu.gpr[6], 0x5678FFFFu);
  CHECK_EQ(cpu.gpr[7], 0u);
  CHECK_EQ(cpu.gpr[8], 0x20000001u);  // rotl(0x12345678,4)=0x23456781 & 0xF000000F
}

void TestBranches() {
  puts("branches and loops");
  ResetMachine();
  // sum = 0; for (ctr = 10; ctr; ctr--) sum += ctr
  Run({li(3, 0), li(4, 10), mtspr(SPR_CTR, 4), mfspr(5, SPR_CTR), XO(3, 3, 5, 266), bc(16, 0, -8) /* bdnz */,
       li(6, 5), D(11, 0, 6, 5) /* cmpwi r6,5 */, bc(12, 2, 8) /* beq +8 */, li(7, 1) /* skipped */,
       li(8, 2)});
  CHECK_EQ(cpu.gpr[3], 55u);
  CHECK_EQ(cpu.gpr[7], 0u);
  CHECK_EQ(cpu.gpr[8], 2u);

  // bl / blr
  ResetMachine();
  u32 end = Run({(18u << 26) | 12 | 1 /* bl +12 */, li(3, 1), b(12), li(4, 2), 0x4E800020 /* blr */});
  CHECK_EQ(cpu.gpr[3], 1u);
  CHECK_EQ(cpu.gpr[4], 2u);
  CHECK_EQ(cpu.pc, end);
}

void TestLoadStore() {
  puts("loads and stores");
  ResetMachine();
  Run({lis(3, (s32)(DATA >> 16)), lis(4, 0x8899), ori(4, 4, 0xAABB), D(36, 4, 3, 0) /* stw */,
       D(34, 5, 3, 1) /* lbz */, D(40, 6, 3, 2) /* lhz */, D(42, 7, 3, 0) /* lha */,
       D(37, 4, 3, 16) /* stwu r4,16(r3) */, li(28, 28), li(29, 29), li(30, 30), li(31, 31),
       D(47, 28, 3, 32) /* stmw r28 */, li(28, 0), li(29, 0), D(46, 28, 3, 32) /* lmw */,
       X(4, 0, 3, 662) /* stwbrx r4,0,r3 */, D(32, 8, 3, 0) /* lwz */});
  CHECK_EQ(cpu.gpr[5], 0x99u);
  CHECK_EQ(cpu.gpr[6], 0xAABBu);
  CHECK_EQ(cpu.gpr[7], 0xFFFF8899u);
  CHECK_EQ(cpu.gpr[3], DATA + 16);
  CHECK_EQ(cpu.gpr[28], 28u);
  CHECK_EQ(cpu.gpr[29], 29u);
  CHECK_EQ(cpu.gpr[8], 0xBBAA9988u);
  CHECK_EQ(Mem::Read32(DATA + 16 + 32 + 12), 31u);
}

void TestFloat() {
  puts("floating point");
  ResetMachine();
  Mem::Write64(DATA, BitCast<u64>(1.5));
  Mem::Write64(DATA + 8, BitCast<u64>(-2.25));
  Run({lis(3, (s32)(DATA >> 16)), D(50, 1, 3, 0) /* lfd f1 */, D(50, 2, 3, 8) /* lfd f2 */,
       A(63, 3, 1, 2, 0, 21) /* fadd f3=f1+f2 */, A(63, 4, 1, 0, 2, 25) /* fmul f4=f1*f2 */,
       FX(63, 5, 0, 4, 15) /* fctiwz f5,f4 */, X(5, 3, 0, 983) /* stfiwx f5,0,r3 */,
       D(32, 6, 3, 0) /* lwz r6 */, A(59, 7, 1, 2, 0, 18) /* fdivs */, FX(63, 0, 1, 2, 0) /* fcmpu cr0 */});
  CHECK_NEAR(cpu.fpr[3].d0(), -0.75);
  CHECK_NEAR(cpu.fpr[4].d0(), -3.375);
  CHECK_EQ(cpu.gpr[6], (u32)-3);
  CHECK_EQ(cpu.fpr[7].d0(), (double)(float)(1.5 / -2.25));
  CHECK_EQ(cpu.fpr[7].d1(), cpu.fpr[7].d0());
  CHECK_EQ(cpu.cr >> 28, 0x4u);  // f1 > f2
}

void TestPairedSingles() {
  puts("paired singles");
  ResetMachine();
  Mem::Write8(DATA, 10);
  Mem::Write8(DATA + 1, 20);
  cpu.spr[SPR_GQR0 + 2] = (4u << 16) | 7u | (4u << 8);  // load u8 scale 0; store s16 scale +4
  Run({lis(3, (s32)(DATA >> 16)), PSQ(56, 1, 3, 0, 2, 0) /* psq_l f1,0(r3),0,qr2 */,
       A(4, 2, 1, 1, 0, 21) /* ps_add f2=f1+f1 */, (4u << 26) | (3 << 21) | (1 << 16) | (2 << 11) | (592 << 1) /* ps_merge10 */,
       A(4, 4, 1, 2, 1, 10) /* ps_sum0 f4 = f1.ps0+f2.ps1, f1.ps1 */, PSQ(60, 2, 3, 0, 2, 16) /* psq_st f2,16(r3) */});
  CHECK_NEAR(cpu.fpr[1].d0(), 10.0);
  CHECK_NEAR(cpu.fpr[1].d1(), 20.0);
  CHECK_NEAR(cpu.fpr[2].d0(), 20.0);
  CHECK_NEAR(cpu.fpr[2].d1(), 40.0);
  CHECK_NEAR(cpu.fpr[3].d0(), 20.0);  // f1.ps1
  CHECK_NEAR(cpu.fpr[3].d1(), 20.0);  // f2.ps0
  CHECK_NEAR(cpu.fpr[4].d0(), 50.0);
  CHECK_NEAR(cpu.fpr[4].d1(), 20.0);
  CHECK_EQ(Mem::Read16(DATA + 16), (u16)(20 * 16));
  CHECK_EQ(Mem::Read16(DATA + 18), (u16)(40 * 16));
}

void TestExceptions() {
  puts("exceptions");
  ResetMachine();
  Run({li(3, 1), SC}, 2);
  CHECK_EQ(cpu.pc, 0xC00u);
  CHECK_EQ(cpu.spr[SPR_SRR0], CODE + 8);
  CHECK_EQ(cpu.msr & (MSR_IR | MSR_DR | MSR_EE), 0u);

  ResetMachine();
  Run({li(3, 1), (31u << 26) | (31 << 21) | (3 << 16) | (3 << 11) | (4 << 1) /* tw 31,r3,r3: always */}, 2);
  CHECK_EQ(cpu.pc, 0x700u);
  CHECK_EQ(cpu.spr[SPR_SRR0], CODE + 4);
  CHECK_EQ((cpu.spr[SPR_SRR1] & PROGRAM_TRAP) != 0, true);

  // rfi returns to SRR0 with SRR1's MSR
  ResetMachine();
  cpu.spr[SPR_SRR0] = CODE + 8;
  cpu.spr[SPR_SRR1] = MSR_FP | MSR_IR | MSR_DR | MSR_EE;
  u32 end = Run({0x4C000064 /* rfi */, li(3, 1) /* skipped */, li(4, 2)});
  CHECK_EQ(cpu.pc, end);
  CHECK_EQ(cpu.gpr[3], 0u);
  CHECK_EQ(cpu.gpr[4], 2u);
  CHECK_EQ(cpu.msr & MSR_EE, (u32)MSR_EE);
}

void TestDecrementer() {
  puts("decrementer interrupt");
  ResetMachine();
  Mem::PhysWrite32(0x900, b(0));  // handler: spin
  Mem::Write32(CODE, b(0));
  cpu.pc = CODE;
  cpu.msr |= MSR_EE;
  CPU::SetDecrementer(10);
  for (int i = 0; i < 4 && cpu.pc != 0x900; i++) CoreTiming::Advance();
  CHECK_EQ(cpu.pc, 0x900u);
  CHECK_EQ(cpu.spr[SPR_SRR0], CODE);
  CHECK_EQ((s32)CPU::GetDecrementer() < 0, true);
}

void TestDataFault() {
  puts("unmapped access raises DSI");
  ResetMachine();
  Run({lis(3, 0x5000), D(32, 4, 3, 0) /* lwz from unmapped 0x50000000 */}, 2);
  CHECK_EQ(cpu.pc, 0x300u);
  CHECK_EQ(cpu.spr[SPR_DAR], 0x50000000u);
  CHECK_EQ(cpu.spr[SPR_SRR0], CODE + 4);
}

}  // namespace

int main() {
  static NullHost host;
  if (!System::Init(&host)) return 1;
  TestIntegerArithmetic();
  TestRotates();
  TestBranches();
  TestLoadStore();
  TestFloat();
  TestPairedSingles();
  TestExceptions();
  TestDecrementer();
  TestDataFault();
  printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures ? 1 : 0;
}
