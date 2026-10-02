// SPDX-License-Identifier: GPL-2.0-or-later
// Integer arithmetic/logic, rotates, compares, branches, CR ops and system instructions.
#include "core/gekko/interp_internal.h"

namespace Interpreter {

namespace {

inline u32 Mask(u32 mb, u32 me) {
  u32 begin = 0xFFFFFFFFu >> mb;
  u32 end = me < 31 ? (0xFFFFFFFFu >> (me + 1)) : 0;
  u32 m = begin ^ end;
  return me < mb ? ~m : m;
}

inline u32 Rotl(u32 v, u32 n) {
  n &= 31;
  return n ? (v << n) | (v >> (32 - n)) : v;
}

inline void SetCA(bool c) {
  if (c)
    cpu.xer |= XER_CA;
  else
    cpu.xer &= ~XER_CA;
}
inline u32 GetCA() { return (cpu.xer >> 29) & 1; }

inline void SetOV(bool ov) {
  if (ov)
    cpu.xer |= XER_OV | XER_SO;
  else
    cpu.xer &= ~XER_OV;
}

inline bool AddOverflow(u32 a, u32 b, u32 r) { return ((a ^ r) & (b ^ r)) >> 31; }

// Shared tail for XO-form arithmetic: carry, overflow and CR0.
inline void FinishXO(u32 inst, u32 a, u32 b, u32 r) {
  if (OE(inst)) SetOV(AddOverflow(a, b, r));
  if (Rc(inst)) UpdateCR0(r);
}

inline void Compare(u32 crf, bool lt, bool gt) {
  u32 f = lt ? 8 : (gt ? 4 : 2);
  if (cpu.xer & XER_SO) f |= 1;
  SetCRField(crf, f);
}

bool TrapCondition(u32 to, u32 a, u32 b) {
  return ((to & 0x10) && (s32)a < (s32)b) || ((to & 0x08) && (s32)a > (s32)b) || ((to & 0x04) && a == b) ||
         ((to & 0x02) && a < b) || ((to & 0x01) && a > b);
}

// ---- Immediate arithmetic ----
void mulli(u32 i) { GPR(RD(i)) = (u32)((s32)GPR(RA(i)) * SIMM(i)); }
void subfic(u32 i) {
  u32 a = GPR(RA(i)), imm = (u32)SIMM(i);
  GPR(RD(i)) = imm - a;
  SetCA(imm >= a || a == 0);
}
void cmpli(u32 i) { Compare(CRFD(i), GPR(RA(i)) < UIMM(i), GPR(RA(i)) > UIMM(i)); }
void cmpi(u32 i) { Compare(CRFD(i), (s32)GPR(RA(i)) < SIMM(i), (s32)GPR(RA(i)) > SIMM(i)); }
void addic(u32 i) {
  u32 a = GPR(RA(i)), r = a + (u32)SIMM(i);
  GPR(RD(i)) = r;
  SetCA(r < a);
}
void addic_rc(u32 i) {
  addic(i);
  UpdateCR0(GPR(RD(i)));
}
void addi(u32 i) { GPR(RD(i)) = RA0(i) + (u32)SIMM(i); }
void addis(u32 i) { GPR(RD(i)) = RA0(i) + (UIMM(i) << 16); }
void ori(u32 i) { GPR(RA(i)) = GPR(RS(i)) | UIMM(i); }
void oris(u32 i) { GPR(RA(i)) = GPR(RS(i)) | (UIMM(i) << 16); }
void xori(u32 i) { GPR(RA(i)) = GPR(RS(i)) ^ UIMM(i); }
void xoris(u32 i) { GPR(RA(i)) = GPR(RS(i)) ^ (UIMM(i) << 16); }
void andi_rc(u32 i) { UpdateCR0(GPR(RA(i)) = GPR(RS(i)) & UIMM(i)); }
void andis_rc(u32 i) { UpdateCR0(GPR(RA(i)) = GPR(RS(i)) & (UIMM(i) << 16)); }
void twi(u32 i) {
  if (TrapCondition(RD(i), GPR(RA(i)), (u32)SIMM(i))) ProgramException(PROGRAM_TRAP);
}

// ---- Rotates ----
void rlwimi(u32 i) {
  u32 m = Mask(MB(i), ME(i));
  u32 r = (GPR(RA(i)) & ~m) | (Rotl(GPR(RS(i)), SH(i)) & m);
  GPR(RA(i)) = r;
  if (Rc(i)) UpdateCR0(r);
}
void rlwinm(u32 i) {
  u32 r = Rotl(GPR(RS(i)), SH(i)) & Mask(MB(i), ME(i));
  GPR(RA(i)) = r;
  if (Rc(i)) UpdateCR0(r);
}
void rlwnm(u32 i) {
  u32 r = Rotl(GPR(RS(i)), GPR(RB(i))) & Mask(MB(i), ME(i));
  GPR(RA(i)) = r;
  if (Rc(i)) UpdateCR0(r);
}

// ---- Branches ----
inline bool BranchCondition(u32 bo, u32 bi) {
  if (!(bo & 4)) CTR--;
  bool ctr_ok = (bo & 4) || ((CTR != 0) ^ ((bo >> 1) & 1));
  bool cond_ok = (bo & 16) || (GetCRBit(bi) == ((bo >> 3) & 1));
  return ctr_ok && cond_ok;
}

void bx(u32 i) {
  u32 li = i & 0x03FFFFFC;
  if (li & 0x02000000) li |= 0xFC000000;
  u32 target = ((i & 2) ? 0 : cpu.pc) + li;
  if (i & 1) LR = cpu.pc + 4;
  cpu.npc = target;
  if (target == cpu.pc && !(i & 1)) CPU::g_idle = true;  // "b ." spins until an interrupt
}
// Detects polling loops "lwz rX,d(rY); cmp(l)wi rX,imm; bc -8": nothing but an
// interrupt or a hardware event can change the polled word, so the CPU can skip
// ahead to the next scheduled event (idle skipping, as Dolphin does).
void CheckIdleLoop(u32 target) {
  if (target != cpu.pc - 8) return;
  u32 load = Mem::ReadInstr(target);
  u32 cmp = Mem::ReadInstr(target + 4);
  u32 op_load = OPCD(load), op_cmp = OPCD(cmp);
  if ((op_load == 32 || op_load == 34 || op_load == 40) && (op_cmp == 10 || op_cmp == 11) &&
      RA(cmp) == RD(load) && RD(load) != RA(load))
    CPU::g_idle = true;
}

void bcx(u32 i) {
  if (BranchCondition(RD(i), RA(i))) {
    u32 bd = (u32)(s32)(s16)(i & 0xFFFC);
    cpu.npc = ((i & 2) ? 0 : cpu.pc) + bd;
    if ((s32)bd == -8) CheckIdleLoop(cpu.npc);
  }
  if (i & 1) LR = cpu.pc + 4;
}
void bclrx(u32 i) {
  u32 target = LR & ~3u;
  if (BranchCondition(RD(i), RA(i))) cpu.npc = target;
  if (i & 1) LR = cpu.pc + 4;
}
void bcctrx(u32 i) {
  u32 bo = RD(i);
  bool cond_ok = (bo & 16) || (GetCRBit(RA(i)) == ((bo >> 3) & 1));
  if (cond_ok) cpu.npc = CTR & ~3u;
  if (i & 1) LR = cpu.pc + 4;
}

// ---- Condition register ----
void mcrf(u32 i) { SetCRField(CRFD(i), GetCRField(CRFS(i))); }
#define CROP(name, expr)                                  \
  void name(u32 i) {                                      \
    u32 a = GetCRBit(RA(i)), b = GetCRBit(RB(i));         \
    SetCRBit(RD(i), (expr) & 1);                          \
  }
CROP(crand, a & b)
CROP(crandc, a & ~b)
CROP(creqv, ~(a ^ b))
CROP(crnand, ~(a & b))
CROP(crnor, ~(a | b))
CROP(cror, a | b)
CROP(crorc, a | ~b)
CROP(crxor, a ^ b)
#undef CROP

// ---- System ----
void rfi(u32) {
  const u32 mask = 0x87C0FFFF;
  cpu.msr = ((cpu.msr & ~mask) | (cpu.spr[SPR_SRR1] & mask)) & ~MSR_POW;
  cpu.npc = cpu.spr[SPR_SRR0] & ~3u;
}
void isync(u32) {}
void sc(u32) { CPU::RaiseException(EXC_SYSCALL); }
void sync(u32) {}
void eieio(u32) {}
void tlbie(u32) {}
void tlbsync(u32) {}

void mfmsr(u32 i) { GPR(RD(i)) = cpu.msr; }
void mtmsr(u32 i) { cpu.msr = GPR(RS(i)); }
void mfsr(u32 i) { GPR(RD(i)) = cpu.sr[RA(i) & 15]; }
void mfsrin(u32 i) { GPR(RD(i)) = cpu.sr[GPR(RB(i)) >> 28]; }
void mtsr(u32 i) { cpu.sr[RA(i) & 15] = GPR(RS(i)); }
void mtsrin(u32 i) { cpu.sr[GPR(RB(i)) >> 28] = GPR(RS(i)); }
void mfspr(u32 i) { GPR(RD(i)) = ReadSPR(SPRN(i)); }
void mtspr(u32 i) { WriteSPR(SPRN(i), GPR(RS(i))); }
void mftb(u32 i) { GPR(RD(i)) = ReadSPR(SPRN(i)); }
void mfcr(u32 i) { GPR(RD(i)) = cpu.cr; }
void mtcrf(u32 i) {
  u32 crm = (i >> 12) & 0xFF, mask = 0;
  for (u32 n = 0; n < 8; n++)
    if (crm & (0x80 >> n)) mask |= 0xF0000000u >> (4 * n);
  cpu.cr = (cpu.cr & ~mask) | (GPR(RS(i)) & mask);
}
void mcrxr(u32 i) {
  SetCRField(CRFD(i), cpu.xer >> 28);
  cpu.xer &= ~0xF0000000u;
}
void tw(u32 i) {
  if (TrapCondition(RD(i), GPR(RA(i)), GPR(RB(i)))) ProgramException(PROGRAM_TRAP);
}

// ---- Register arithmetic (XO-form) ----
void add(u32 i) {
  u32 a = GPR(RA(i)), b = GPR(RB(i)), r = a + b;
  GPR(RD(i)) = r;
  FinishXO(i, a, b, r);
}
void addc(u32 i) {
  u32 a = GPR(RA(i)), b = GPR(RB(i)), r = a + b;
  GPR(RD(i)) = r;
  SetCA(r < a);
  FinishXO(i, a, b, r);
}
void adde(u32 i) {
  u32 a = GPR(RA(i)), b = GPR(RB(i)), ca = GetCA();
  u64 r64 = (u64)a + b + ca;
  u32 r = (u32)r64;
  GPR(RD(i)) = r;
  SetCA(r64 >> 32);
  FinishXO(i, a, b, r);
}
void addme(u32 i) {
  u32 a = GPR(RA(i)), ca = GetCA();
  u64 r64 = (u64)a + 0xFFFFFFFFu + ca;
  u32 r = (u32)r64;
  GPR(RD(i)) = r;
  SetCA(r64 >> 32);
  FinishXO(i, a, 0xFFFFFFFFu, r);
}
void addze(u32 i) {
  u32 a = GPR(RA(i)), ca = GetCA();
  u64 r64 = (u64)a + ca;
  u32 r = (u32)r64;
  GPR(RD(i)) = r;
  SetCA(r64 >> 32);
  FinishXO(i, a, 0, r);
}
void subf(u32 i) {
  u32 a = GPR(RA(i)), b = GPR(RB(i)), r = b - a;
  GPR(RD(i)) = r;
  FinishXO(i, ~a, b, r);
}
void subfc(u32 i) {
  u32 a = GPR(RA(i)), b = GPR(RB(i)), r = b - a;
  GPR(RD(i)) = r;
  SetCA(b >= a);
  FinishXO(i, ~a, b, r);
}
void subfe(u32 i) {
  u32 a = GPR(RA(i)), b = GPR(RB(i)), ca = GetCA();
  u64 r64 = (u64)(~a) + b + ca;
  u32 r = (u32)r64;
  GPR(RD(i)) = r;
  SetCA(r64 >> 32);
  FinishXO(i, ~a, b, r);
}
void subfme(u32 i) {
  u32 a = GPR(RA(i)), ca = GetCA();
  u64 r64 = (u64)(~a) + 0xFFFFFFFFu + ca;
  u32 r = (u32)r64;
  GPR(RD(i)) = r;
  SetCA(r64 >> 32);
  FinishXO(i, ~a, 0xFFFFFFFFu, r);
}
void subfze(u32 i) {
  u32 a = GPR(RA(i)), ca = GetCA();
  u64 r64 = (u64)(~a) + ca;
  u32 r = (u32)r64;
  GPR(RD(i)) = r;
  SetCA(r64 >> 32);
  FinishXO(i, ~a, 0, r);
}
void neg(u32 i) {
  u32 a = GPR(RA(i)), r = (u32)(-(s32)a);
  GPR(RD(i)) = r;
  if (OE(i)) SetOV(a == 0x80000000u);
  if (Rc(i)) UpdateCR0(r);
}
void mullw(u32 i) {
  s64 r = (s64)(s32)GPR(RA(i)) * (s32)GPR(RB(i));
  GPR(RD(i)) = (u32)r;
  if (OE(i)) SetOV(r < INT32_MIN || r > INT32_MAX);
  if (Rc(i)) UpdateCR0((u32)r);
}
void mulhw(u32 i) {
  u32 r = (u32)(((s64)(s32)GPR(RA(i)) * (s32)GPR(RB(i))) >> 32);
  GPR(RD(i)) = r;
  if (Rc(i)) UpdateCR0(r);
}
void mulhwu(u32 i) {
  u32 r = (u32)(((u64)GPR(RA(i)) * GPR(RB(i))) >> 32);
  GPR(RD(i)) = r;
  if (Rc(i)) UpdateCR0(r);
}
void divw(u32 i) {
  s32 a = (s32)GPR(RA(i)), b = (s32)GPR(RB(i));
  u32 r;
  bool ov = b == 0 || ((u32)a == 0x80000000u && b == -1);
  if (ov)
    r = (((u32)a & 0x80000000u) && b == 0) ? 0xFFFFFFFFu : 0;
  else
    r = (u32)(a / b);
  GPR(RD(i)) = r;
  if (OE(i)) SetOV(ov);
  if (Rc(i)) UpdateCR0(r);
}
void divwu(u32 i) {
  u32 a = GPR(RA(i)), b = GPR(RB(i));
  u32 r = b ? a / b : 0;
  GPR(RD(i)) = r;
  if (OE(i)) SetOV(b == 0);
  if (Rc(i)) UpdateCR0(r);
}

// ---- Register logic (X-form) ----
#define LOGIC(name, expr)            \
  void name(u32 i) {                 \
    u32 s = GPR(RS(i)), b = GPR(RB(i)); \
    (void)b;                         \
    u32 r = (expr);                  \
    GPR(RA(i)) = r;                  \
    if (Rc(i)) UpdateCR0(r);         \
  }
LOGIC(and_, s & b)
LOGIC(andc, s & ~b)
LOGIC(or_, s | b)
LOGIC(orc, s | ~b)
LOGIC(xor_, s ^ b)
LOGIC(nand, ~(s & b))
LOGIC(nor, ~(s | b))
LOGIC(eqv, ~(s ^ b))
LOGIC(extsb, (u32)(s32)(s8)s)
LOGIC(extsh, (u32)(s32)(s16)s)
LOGIC(cntlzw, s ? (u32)__builtin_clz(s) : 32u)
LOGIC(slw, (b & 0x20) ? 0 : s << (b & 31))
LOGIC(srw, (b & 0x20) ? 0 : s >> (b & 31))
#undef LOGIC

void sraw(u32 i) {
  u32 s = GPR(RS(i)), n = GPR(RB(i)) & 0x3F, r;
  if (n & 0x20) {
    r = (s & 0x80000000u) ? 0xFFFFFFFFu : 0;
    SetCA(s & 0x80000000u);
  } else {
    r = (u32)((s32)s >> n);
    SetCA((s & 0x80000000u) && n && (s & ((1u << n) - 1)));
  }
  GPR(RA(i)) = r;
  if (Rc(i)) UpdateCR0(r);
}
void srawi(u32 i) {
  u32 s = GPR(RS(i)), n = SH(i);
  u32 r = (u32)((s32)s >> n);
  SetCA((s & 0x80000000u) && n && (s & ((1u << n) - 1)));
  GPR(RA(i)) = r;
  if (Rc(i)) UpdateCR0(r);
}

void cmp(u32 i) {
  s32 a = (s32)GPR(RA(i)), b = (s32)GPR(RB(i));
  Compare(CRFD(i), a < b, a > b);
}
void cmpl(u32 i) {
  u32 a = GPR(RA(i)), b = GPR(RB(i));
  Compare(CRFD(i), a < b, a > b);
}

}  // namespace

void RegisterInteger() {
  Reg(T_PRIMARY, 3, twi, "twi");
  Reg(T_PRIMARY, 7, mulli, "mulli");
  Reg(T_PRIMARY, 8, subfic, "subfic");
  Reg(T_PRIMARY, 10, cmpli, "cmpli");
  Reg(T_PRIMARY, 11, cmpi, "cmpi");
  Reg(T_PRIMARY, 12, addic, "addic");
  Reg(T_PRIMARY, 13, addic_rc, "addic.");
  Reg(T_PRIMARY, 14, addi, "addi");
  Reg(T_PRIMARY, 15, addis, "addis");
  Reg(T_PRIMARY, 16, bcx, "bc");
  Reg(T_PRIMARY, 17, sc, "sc");
  Reg(T_PRIMARY, 18, bx, "b");
  Reg(T_PRIMARY, 20, rlwimi, "rlwimi");
  Reg(T_PRIMARY, 21, rlwinm, "rlwinm");
  Reg(T_PRIMARY, 23, rlwnm, "rlwnm");
  Reg(T_PRIMARY, 24, ori, "ori");
  Reg(T_PRIMARY, 25, oris, "oris");
  Reg(T_PRIMARY, 26, xori, "xori");
  Reg(T_PRIMARY, 27, xoris, "xoris");
  Reg(T_PRIMARY, 28, andi_rc, "andi.");
  Reg(T_PRIMARY, 29, andis_rc, "andis.");

  Reg(T_19, 0, mcrf, "mcrf");
  Reg(T_19, 16, bclrx, "bclr");
  Reg(T_19, 33, crnor, "crnor");
  Reg(T_19, 50, rfi, "rfi");
  Reg(T_19, 129, crandc, "crandc");
  Reg(T_19, 150, isync, "isync");
  Reg(T_19, 193, crxor, "crxor");
  Reg(T_19, 225, crnand, "crnand");
  Reg(T_19, 257, crand, "crand");
  Reg(T_19, 289, creqv, "creqv");
  Reg(T_19, 417, crorc, "crorc");
  Reg(T_19, 449, cror, "cror");
  Reg(T_19, 528, bcctrx, "bcctr");

  Reg(T_31, 0, cmp, "cmp");
  Reg(T_31, 4, tw, "tw");
  RegXO(8, subfc, "subfc");
  RegXO(10, addc, "addc");
  Reg(T_31, 11, mulhwu, "mulhwu");
  Reg(T_31, 19, mfcr, "mfcr");
  Reg(T_31, 24, slw, "slw");
  Reg(T_31, 26, cntlzw, "cntlzw");
  Reg(T_31, 28, and_, "and");
  Reg(T_31, 32, cmpl, "cmpl");
  RegXO(40, subf, "subf");
  Reg(T_31, 60, andc, "andc");
  Reg(T_31, 75, mulhw, "mulhw");
  Reg(T_31, 83, mfmsr, "mfmsr");
  RegXO(104, neg, "neg");
  Reg(T_31, 124, nor, "nor");
  RegXO(136, subfe, "subfe");
  RegXO(138, adde, "adde");
  Reg(T_31, 144, mtcrf, "mtcrf");
  Reg(T_31, 146, mtmsr, "mtmsr");
  RegXO(200, subfze, "subfze");
  RegXO(202, addze, "addze");
  Reg(T_31, 210, mtsr, "mtsr");
  RegXO(232, subfme, "subfme");
  RegXO(234, addme, "addme");
  RegXO(235, mullw, "mullw");
  Reg(T_31, 242, mtsrin, "mtsrin");
  RegXO(266, add, "add");
  Reg(T_31, 284, eqv, "eqv");
  Reg(T_31, 306, tlbie, "tlbie");
  Reg(T_31, 316, xor_, "xor");
  Reg(T_31, 339, mfspr, "mfspr");
  Reg(T_31, 371, mftb, "mftb");
  Reg(T_31, 412, orc, "orc");
  Reg(T_31, 444, or_, "or");
  RegXO(459, divwu, "divwu");
  Reg(T_31, 467, mtspr, "mtspr");
  Reg(T_31, 476, nand, "nand");
  RegXO(491, divw, "divw");
  Reg(T_31, 512, mcrxr, "mcrxr");
  Reg(T_31, 536, srw, "srw");
  Reg(T_31, 566, tlbsync, "tlbsync");
  Reg(T_31, 595, mfsr, "mfsr");
  Reg(T_31, 598, sync, "sync");
  Reg(T_31, 659, mfsrin, "mfsrin");
  Reg(T_31, 792, sraw, "sraw");
  Reg(T_31, 824, srawi, "srawi");
  Reg(T_31, 854, eieio, "eieio");
  Reg(T_31, 922, extsh, "extsh");
  Reg(T_31, 954, extsb, "extsb");
}

}  // namespace Interpreter
