// emugcxbox360 - PowerPC machine code emitter for the JIT (host = Xenon)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Only 32-bit PowerPC user-mode instructions are emitted, so the same code
// runs natively on the Xenon and in the verification simulator (ppc_sim).
#pragma once

#include <vector>

#include "core/common.h"

class PPCEmitter {
 public:
  std::vector<u32> code;

  u32 Here() const { return (u32)code.size(); }  // in instructions
  void Emit(u32 inst) { code.push_back(inst); }

  // ---- Encodings ----
  static u32 D(u32 op, u32 rt, u32 ra, s32 imm) { return (op << 26) | (rt << 21) | (ra << 16) | ((u32)imm & 0xFFFF); }
  static u32 X(u32 op, u32 rt, u32 ra, u32 rb, u32 xo, u32 rc = 0) {
    return (op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
  }
  static u32 XO(u32 rt, u32 ra, u32 rb, u32 xo, u32 rc = 0) { return X(31, rt, ra, rb, xo, rc); }
  static u32 A(u32 op, u32 frt, u32 fra, u32 frb, u32 frc, u32 xo) {
    return (op << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1);
  }

  // ---- Integer ----
  void addi(u32 rt, u32 ra, s32 imm) { Emit(D(14, rt, ra, imm)); }
  void addis(u32 rt, u32 ra, s32 imm) { Emit(D(15, rt, ra, imm)); }
  void li(u32 rt, s32 imm) { addi(rt, 0, imm); }
  void lis(u32 rt, s32 imm) { addis(rt, 0, imm); }
  void ori(u32 ra, u32 rs, u32 imm) { Emit(D(24, rs, ra, (s32)imm)); }
  void oris(u32 ra, u32 rs, u32 imm) { Emit(D(25, rs, ra, (s32)imm)); }
  void xoris(u32 ra, u32 rs, u32 imm) { Emit(D(27, rs, ra, (s32)imm)); }
  void andi_(u32 ra, u32 rs, u32 imm) { Emit(D(28, rs, ra, (s32)imm)); }
  void andis_(u32 ra, u32 rs, u32 imm) { Emit(D(29, rs, ra, (s32)imm)); }
  void cmpwi(u32 crf, u32 ra, s32 imm) { Emit(D(11, crf << 2, ra, imm)); }
  void cmplwi(u32 crf, u32 ra, u32 imm) { Emit(D(10, crf << 2, ra, (s32)imm)); }
  void cmpw(u32 crf, u32 ra, u32 rb) { Emit(X(31, crf << 2, ra, rb, 0)); }
  void cmplw(u32 crf, u32 ra, u32 rb) { Emit(X(31, crf << 2, ra, rb, 32)); }
  void addic(u32 rt, u32 ra, s32 imm) { Emit(D(12, rt, ra, imm)); }
  void addze(u32 rt, u32 ra) { Emit(XO(rt, ra, 0, 202)); }
  void add(u32 rt, u32 ra, u32 rb) { Emit(XO(rt, ra, rb, 266)); }
  void subf(u32 rt, u32 ra, u32 rb) { Emit(XO(rt, ra, rb, 40)); }
  void mr(u32 ra, u32 rs) { Emit(X(31, rs, ra, rs, 444)); }
  // Loads a 32-bit constant in one or two instructions.
  void LoadImm(u32 rt, u32 value) {
    if ((s32)value >= -0x8000 && (s32)value < 0x8000) {
      li(rt, (s32)value);
    } else {
      lis(rt, (s32)(value >> 16));
      if (value & 0xFFFF) ori(rt, rt, value & 0xFFFF);
    }
  }

  void rlwinm(u32 ra, u32 rs, u32 sh, u32 mb, u32 me, u32 rc = 0) {
    Emit((21u << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc);
  }
  void rlwimi(u32 ra, u32 rs, u32 sh, u32 mb, u32 me) {
    Emit((20u << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1));
  }

  // ---- Memory ----
  void lwz(u32 rt, s32 d, u32 ra) { Emit(D(32, rt, ra, d)); }
  void lbz(u32 rt, s32 d, u32 ra) { Emit(D(34, rt, ra, d)); }
  void stb(u32 rs, s32 d, u32 ra) { Emit(D(38, rs, ra, d)); }
  void stw(u32 rs, s32 d, u32 ra) { Emit(D(36, rs, ra, d)); }
  void stwu(u32 rs, s32 d, u32 ra) { Emit(D(37, rs, ra, d)); }
  void lfd(u32 frt, s32 d, u32 ra) { Emit(D(50, frt, ra, d)); }
  void stfd(u32 frs, s32 d, u32 ra) { Emit(D(54, frs, ra, d)); }
  void lwzx(u32 rt, u32 ra, u32 rb) { Emit(X(31, rt, ra, rb, 23)); }
  void lbzx(u32 rt, u32 ra, u32 rb) { Emit(X(31, rt, ra, rb, 87)); }
  void lhzx(u32 rt, u32 ra, u32 rb) { Emit(X(31, rt, ra, rb, 279)); }
  void lhax(u32 rt, u32 ra, u32 rb) { Emit(X(31, rt, ra, rb, 343)); }
  void stwx(u32 rs, u32 ra, u32 rb) { Emit(X(31, rs, ra, rb, 151)); }
  void stbx(u32 rs, u32 ra, u32 rb) { Emit(X(31, rs, ra, rb, 215)); }
  void sthx(u32 rs, u32 ra, u32 rb) { Emit(X(31, rs, ra, rb, 407)); }
  void lfsx(u32 frt, u32 ra, u32 rb) { Emit(X(31, frt, ra, rb, 535)); }
  void lfdx(u32 frt, u32 ra, u32 rb) { Emit(X(31, frt, ra, rb, 599)); }
  void stfsx(u32 frs, u32 ra, u32 rb) { Emit(X(31, frs, ra, rb, 663)); }
  void stfdx(u32 frs, u32 ra, u32 rb) { Emit(X(31, frs, ra, rb, 727)); }

  // ---- Special registers ----
  void mfcr(u32 rt) { Emit(X(31, rt, 0, 0, 19)); }
  // mfocrf rt, cr0: only field 0 is defined (mfcr is microcoded on the Xenon)
  void mfocrf_cr0(u32 rt) { Emit(X(31, rt, 0, 0, 19) | 0x00100000 | (0x80u << 12)); }
  void mtspr(u32 spr, u32 rs) { Emit((31u << 26) | (rs << 21) | ((spr & 0x1F) << 16) | ((spr >> 5) << 11) | (467 << 1)); }
  void mfspr(u32 rt, u32 spr) { Emit((31u << 26) | (rt << 21) | ((spr & 0x1F) << 16) | ((spr >> 5) << 11) | (339 << 1)); }
  void mtxer(u32 rs) { mtspr(1, rs); }
  void mfxer(u32 rt) { mfspr(rt, 1); }
  void mtlr(u32 rs) { mtspr(8, rs); }
  void mflr(u32 rt) { mfspr(rt, 8); }
  void mtctr(u32 rs) { mtspr(9, rs); }

  // ---- Branches (offsets in instructions, patched later when needed) ----
  void b(s32 offset_insts) { Emit((18u << 26) | (((u32)offset_insts << 2) & 0x03FFFFFC)); }
  void bc(u32 bo, u32 bi, s32 offset_insts) { Emit((16u << 26) | (bo << 21) | (bi << 16) | (((u32)offset_insts << 2) & 0xFFFC)); }
  void blr() { Emit(0x4E800020); }
  void bctr() { Emit(0x4E800420); }
  // Calls an absolute address through CTR (clobbers r12).
  void CallAbs(u32 addr) {
    LoadImm(12, addr);
    mtctr(12);
    bctrl();
  }
  void bctrl() { Emit(0x4E800421); }
  // Emits a placeholder conditional branch; returns its index for PatchBranch.
  u32 bc_forward(u32 bo, u32 bi) {
    bc(bo, bi, 0);
    return Here() - 1;
  }
  u32 b_forward() {
    b(0);
    return Here() - 1;
  }
  void PatchBranchTo(u32 at, u32 target) {
    s32 off = ((s32)target - (s32)at) * 4;
    u32 inst = code[at];
    if ((inst >> 26) == 18)
      code[at] = (inst & ~0x03FFFFFCu) | ((u32)off & 0x03FFFFFC);
    else
      code[at] = (inst & ~0xFFFCu) | ((u32)off & 0xFFFC);
  }

  // Branch-if helpers on CR0 (bo: 12 = branch if true, 4 = branch if false)
  static constexpr u32 BO_TRUE = 12, BO_FALSE = 4;
  static constexpr u32 CR0_LT = 0, CR0_GT = 1, CR0_EQ = 2;
};
