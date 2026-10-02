// SPDX-License-Identifier: GPL-2.0-or-later
// Scalar FPU, FPSCR handling, Gekko paired singles and quantized loads/stores.
#include <cmath>

#include "core/gekko/interp_internal.h"

namespace Interpreter {

void DcbzL(u32 inst);

namespace {

constexpr u64 FCTIW_HIGH = 0xFFF8000000000000ull;

inline double A(u32 i) { return FPR(RA(i)).d0(); }
inline double B(u32 i) { return FPR(RB(i)).d0(); }
inline double C(u32 i) { return FPR(RC(i)).d0(); }

inline void UpdateCR1(u32 i) {
  if (Rc(i)) SetCRField(1, cpu.fpscr >> 28);
}

inline void SetFPCC(u32 c) { cpu.fpscr = (cpu.fpscr & ~0x1F000u) | (c << 12); }

u32 CompareCode(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return 1;
  if (a < b) return 8;
  if (a > b) return 4;
  return 2;
}

// ---- Double precision (op 63) ----
#define FP_D(name, expr)                         \
  void name(u32 i) {                             \
    if (!CheckFP()) return;                      \
    FPR(RD(i)).set0(expr);                       \
    UpdateCR1(i);                                \
  }
FP_D(fadd, A(i) + B(i))
FP_D(fsub, A(i) - B(i))
FP_D(fmul, A(i) * C(i))
FP_D(fdiv, A(i) / B(i))
FP_D(fmadd, A(i) * C(i) + B(i))
FP_D(fmsub, A(i) * C(i) - B(i))
FP_D(fnmadd, -(A(i) * C(i) + B(i)))
FP_D(fnmsub, -(A(i) * C(i) - B(i)))
FP_D(fsel, A(i) >= 0.0 ? C(i) : B(i))
FP_D(frsqrte, 1.0 / std::sqrt(B(i)))
#undef FP_D

// ---- Single precision (op 59): result rounded and written to both halves ----
#define FP_S(name, expr)                         \
  void name(u32 i) {                             \
    if (!CheckFP()) return;                      \
    FPR(RD(i)).fill(RoundSingle(expr));          \
    UpdateCR1(i);                                \
  }
FP_S(fadds, A(i) + B(i))
FP_S(fsubs, A(i) - B(i))
FP_S(fmuls, A(i) * C(i))
FP_S(fdivs, A(i) / B(i))
FP_S(fmadds, A(i) * C(i) + B(i))
FP_S(fmsubs, A(i) * C(i) - B(i))
FP_S(fnmadds, -(A(i) * C(i) + B(i)))
FP_S(fnmsubs, -(A(i) * C(i) - B(i)))
FP_S(fres, 1.0 / B(i))
#undef FP_S

void frsp(u32 i) {
  if (!CheckFP()) return;
  FPR(RD(i)).fill(RoundSingle(B(i)));
  UpdateCR1(i);
}

// Bitwise moves operate on ps0 only.
void fmr(u32 i) {
  if (!CheckFP()) return;
  FPR(RD(i)).ps0 = FPR(RB(i)).ps0;
  UpdateCR1(i);
}
void fneg(u32 i) {
  if (!CheckFP()) return;
  FPR(RD(i)).ps0 = FPR(RB(i)).ps0 ^ (1ull << 63);
  UpdateCR1(i);
}
void fabs_(u32 i) {
  if (!CheckFP()) return;
  FPR(RD(i)).ps0 = FPR(RB(i)).ps0 & ~(1ull << 63);
  UpdateCR1(i);
}
void fnabs(u32 i) {
  if (!CheckFP()) return;
  FPR(RD(i)).ps0 = FPR(RB(i)).ps0 | (1ull << 63);
  UpdateCR1(i);
}

u32 ConvertToInt(double b, u32 rounding) {
  if (std::isnan(b)) return 0x80000000u;
  double r;
  switch (rounding) {
    case 0: r = std::nearbyint(b); break;  // host default is round-to-nearest-even
    case 1: r = std::trunc(b); break;
    case 2: r = std::ceil(b); break;
    default: r = std::floor(b); break;
  }
  if (r >= 2147483647.0) return 0x7FFFFFFFu;
  if (r <= -2147483648.0) return 0x80000000u;
  return (u32)(s32)r;
}
void fctiw(u32 i) {
  if (!CheckFP()) return;
  FPR(RD(i)).ps0 = FCTIW_HIGH | ConvertToInt(B(i), cpu.fpscr & 3);
  UpdateCR1(i);
}
void fctiwz(u32 i) {
  if (!CheckFP()) return;
  FPR(RD(i)).ps0 = FCTIW_HIGH | ConvertToInt(B(i), 1);
  UpdateCR1(i);
}

void fcmpu(u32 i) {
  if (!CheckFP()) return;
  u32 c = CompareCode(A(i), B(i));
  SetCRField(CRFD(i), c);
  SetFPCC(c);
}

// ---- FPSCR ----
void mffs(u32 i) {
  if (!CheckFP()) return;
  FPR(RD(i)).ps0 = FCTIW_HIGH | cpu.fpscr;
  UpdateCR1(i);
}
void mtfsf(u32 i) {
  if (!CheckFP()) return;
  u32 fm = (i >> 17) & 0xFF, mask = 0;
  for (u32 n = 0; n < 8; n++)
    if (fm & (0x80 >> n)) mask |= 0xF0000000u >> (4 * n);
  cpu.fpscr = (cpu.fpscr & ~mask) | ((u32)FPR(RB(i)).ps0 & mask);
  UpdateCR1(i);
}
void mtfsfi(u32 i) {
  if (!CheckFP()) return;
  u32 sh = 28 - 4 * CRFD(i);
  cpu.fpscr = (cpu.fpscr & ~(0xFu << sh)) | (((i >> 12) & 0xF) << sh);
  UpdateCR1(i);
}
void mtfsb0(u32 i) {
  if (!CheckFP()) return;
  cpu.fpscr &= ~(0x80000000u >> RD(i));
  UpdateCR1(i);
}
void mtfsb1(u32 i) {
  if (!CheckFP()) return;
  cpu.fpscr |= 0x80000000u >> RD(i);
  UpdateCR1(i);
}
void mcrfs(u32 i) {
  if (!CheckFP()) return;
  u32 sh = 28 - 4 * CRFS(i);
  SetCRField(CRFD(i), (cpu.fpscr >> sh) & 0xF);
  // Exception bits copied out are cleared (FX, OX..VXCVI subset).
  cpu.fpscr &= ~((0x9FF80700u >> 0) & (0xFu << sh));
}

// ---- Paired singles ----
inline double A1(u32 i) { return FPR(RA(i)).d1(); }
inline double B1(u32 i) { return FPR(RB(i)).d1(); }
inline double C1(u32 i) { return FPR(RC(i)).d1(); }

inline void SetPair(u32 rd, double p0, double p1) {
  FPR(rd).set0(RoundSingle(p0));
  FPR(rd).set1(RoundSingle(p1));
}

#define PS_OP(name, e0, e1)               \
  void name(u32 i) {                      \
    if (!CheckFP()) return;               \
    double r0 = (e0), r1 = (e1);          \
    SetPair(RD(i), r0, r1);               \
    UpdateCR1(i);                         \
  }
PS_OP(ps_add, A(i) + B(i), A1(i) + B1(i))
PS_OP(ps_sub, A(i) - B(i), A1(i) - B1(i))
PS_OP(ps_mul, A(i) * C(i), A1(i) * C1(i))
PS_OP(ps_div, A(i) / B(i), A1(i) / B1(i))
PS_OP(ps_madd, A(i) * C(i) + B(i), A1(i) * C1(i) + B1(i))
PS_OP(ps_msub, A(i) * C(i) - B(i), A1(i) * C1(i) - B1(i))
PS_OP(ps_nmadd, -(A(i) * C(i) + B(i)), -(A1(i) * C1(i) + B1(i)))
PS_OP(ps_nmsub, -(A(i) * C(i) - B(i)), -(A1(i) * C1(i) - B1(i)))
PS_OP(ps_sel, A(i) >= 0.0 ? C(i) : B(i), A1(i) >= 0.0 ? C1(i) : B1(i))
PS_OP(ps_res, 1.0 / B(i), 1.0 / B1(i))
PS_OP(ps_rsqrte, 1.0 / std::sqrt(B(i)), 1.0 / std::sqrt(B1(i)))
PS_OP(ps_sum0, A(i) + B1(i), C1(i))
PS_OP(ps_sum1, C(i), A(i) + B1(i))
PS_OP(ps_muls0, A(i) * C(i), A1(i) * C(i))
PS_OP(ps_muls1, A(i) * C1(i), A1(i) * C1(i))
PS_OP(ps_madds0, A(i) * C(i) + B(i), A1(i) * C(i) + B1(i))
PS_OP(ps_madds1, A(i) * C1(i) + B(i), A1(i) * C1(i) + B1(i))
#undef PS_OP

#define PS_BITS(name, op)                                   \
  void name(u32 i) {                                        \
    if (!CheckFP()) return;                                 \
    FPR(RD(i)).ps0 = FPR(RB(i)).ps0 op;                     \
    FPR(RD(i)).ps1 = FPR(RB(i)).ps1 op;                     \
    UpdateCR1(i);                                           \
  }
PS_BITS(ps_mr, | 0)
PS_BITS(ps_neg, ^(1ull << 63))
PS_BITS(ps_abs, &~(1ull << 63))
PS_BITS(ps_nabs, | (1ull << 63))
#undef PS_BITS

#define PS_MERGE(name, src0, src1)              \
  void name(u32 i) {                            \
    if (!CheckFP()) return;                     \
    u64 p0 = FPR(RA(i)).src0, p1 = FPR(RB(i)).src1; \
    FPR(RD(i)).ps0 = p0;                        \
    FPR(RD(i)).ps1 = p1;                        \
    UpdateCR1(i);                               \
  }
PS_MERGE(ps_merge00, ps0, ps0)
PS_MERGE(ps_merge01, ps0, ps1)
PS_MERGE(ps_merge10, ps1, ps0)
PS_MERGE(ps_merge11, ps1, ps1)
#undef PS_MERGE

void ps_cmpu0(u32 i) {
  if (!CheckFP()) return;
  u32 c = CompareCode(A(i), B(i));
  SetCRField(CRFD(i), c);
  SetFPCC(c);
}
void ps_cmpu1(u32 i) {
  if (!CheckFP()) return;
  u32 c = CompareCode(A1(i), B1(i));
  SetCRField(CRFD(i), c);
  SetFPCC(c);
}

// ---- Quantized loads/stores (psq_l / psq_st) ----
float s_dequant[64];
float s_quant[64];

inline u32 QuantSize(u32 type) {
  switch (type) {
    case 4: case 6: return 1;  // u8 / s8
    case 5: case 7: return 2;  // u16 / s16
    default: return 4;         // float
  }
}

double LoadQuantized(u32 ea, u32 type, u32 scale) {
  switch (type) {
    case 4: return (double)((float)Mem::Read8(ea) * s_dequant[scale]);
    case 5: return (double)((float)Mem::Read16(ea) * s_dequant[scale]);
    case 6: return (double)((float)(s8)Mem::Read8(ea) * s_dequant[scale]);
    case 7: return (double)((float)(s16)Mem::Read16(ea) * s_dequant[scale]);
    default: return (double)BitCast<float>(Mem::Read32(ea));
  }
}

template <typename T>
T Clamp(float v, float lo, float hi) {
  if (!(v > lo)) return (T)lo;  // also catches NaN
  if (v > hi) return (T)hi;
  return (T)v;
}

void StoreQuantized(u32 ea, u32 type, u32 scale, double value) {
  float v = (float)value;
  float q = v * s_quant[scale];
  switch (type) {
    case 4: Mem::Write8(ea, Clamp<u8>(q, 0.0f, 255.0f)); break;
    case 5: Mem::Write16(ea, Clamp<u16>(q, 0.0f, 65535.0f)); break;
    case 6: Mem::Write8(ea, (u8)Clamp<s8>(q, -128.0f, 127.0f)); break;
    case 7: Mem::Write16(ea, (u16)Clamp<s16>(q, -32768.0f, 32767.0f)); break;
    default: Mem::Write32(ea, BitCast<u32>(v)); break;
  }
}

void QuantLoad(u32 rd, u32 ea, u32 w, u32 qr) {
  u32 gqr = cpu.spr[SPR_GQR0 + qr];
  u32 type = (gqr >> 16) & 7, scale = (gqr >> 24) & 0x3F;
  double p0 = LoadQuantized(ea, type, scale);
  double p1 = w ? 1.0 : LoadQuantized(ea + QuantSize(type), type, scale);
  if (!NoFault()) return;
  FPR(rd).set0(p0);
  FPR(rd).set1(p1);
}

void QuantStore(u32 rs, u32 ea, u32 w, u32 qr) {
  u32 gqr = cpu.spr[SPR_GQR0 + qr];
  u32 type = gqr & 7, scale = (gqr >> 8) & 0x3F;
  StoreQuantized(ea, type, scale, FPR(rs).d0());
  if (!w && NoFault()) StoreQuantized(ea + QuantSize(type), type, scale, FPR(rs).d1());
}

inline s32 PsqOffset(u32 i) { return ((s32)(i << 20)) >> 20; }

void psq_l(u32 i) {
  if (CheckFP()) QuantLoad(RD(i), RA0(i) + PsqOffset(i), (i >> 15) & 1, (i >> 12) & 7);
}
void psq_lu(u32 i) {
  if (!CheckFP()) return;
  u32 ea = GPR(RA(i)) + PsqOffset(i);
  QuantLoad(RD(i), ea, (i >> 15) & 1, (i >> 12) & 7);
  if (NoFault()) GPR(RA(i)) = ea;
}
void psq_st(u32 i) {
  if (CheckFP()) QuantStore(RS(i), RA0(i) + PsqOffset(i), (i >> 15) & 1, (i >> 12) & 7);
}
void psq_stu(u32 i) {
  if (!CheckFP()) return;
  u32 ea = GPR(RA(i)) + PsqOffset(i);
  QuantStore(RS(i), ea, (i >> 15) & 1, (i >> 12) & 7);
  if (NoFault()) GPR(RA(i)) = ea;
}
void psq_lx(u32 i) {
  if (CheckFP()) QuantLoad(RD(i), RA0(i) + GPR(RB(i)), (i >> 10) & 1, (i >> 7) & 7);
}
void psq_lux(u32 i) {
  if (!CheckFP()) return;
  u32 ea = GPR(RA(i)) + GPR(RB(i));
  QuantLoad(RD(i), ea, (i >> 10) & 1, (i >> 7) & 7);
  if (NoFault()) GPR(RA(i)) = ea;
}
void psq_stx(u32 i) {
  if (CheckFP()) QuantStore(RS(i), RA0(i) + GPR(RB(i)), (i >> 10) & 1, (i >> 7) & 7);
}
void psq_stux(u32 i) {
  if (!CheckFP()) return;
  u32 ea = GPR(RA(i)) + GPR(RB(i));
  QuantStore(RS(i), ea, (i >> 10) & 1, (i >> 7) & 7);
  if (NoFault()) GPR(RA(i)) = ea;
}

}  // namespace

void RegisterFloat() {
  RegA(T_59, 18, fdivs, "fdivs");
  RegA(T_59, 20, fsubs, "fsubs");
  RegA(T_59, 21, fadds, "fadds");
  RegA(T_59, 24, fres, "fres");
  RegA(T_59, 25, fmuls, "fmuls");
  RegA(T_59, 28, fmsubs, "fmsubs");
  RegA(T_59, 29, fmadds, "fmadds");
  RegA(T_59, 30, fnmsubs, "fnmsubs");
  RegA(T_59, 31, fnmadds, "fnmadds");

  RegA(T_63, 18, fdiv, "fdiv");
  RegA(T_63, 20, fsub, "fsub");
  RegA(T_63, 21, fadd, "fadd");
  RegA(T_63, 23, fsel, "fsel");
  RegA(T_63, 25, fmul, "fmul");
  RegA(T_63, 26, frsqrte, "frsqrte");
  RegA(T_63, 28, fmsub, "fmsub");
  RegA(T_63, 29, fmadd, "fmadd");
  RegA(T_63, 30, fnmsub, "fnmsub");
  RegA(T_63, 31, fnmadd, "fnmadd");
  Reg(T_63, 0, fcmpu, "fcmpu");
  Reg(T_63, 12, frsp, "frsp");
  Reg(T_63, 14, fctiw, "fctiw");
  Reg(T_63, 15, fctiwz, "fctiwz");
  Reg(T_63, 32, fcmpu, "fcmpo");
  Reg(T_63, 38, mtfsb1, "mtfsb1");
  Reg(T_63, 40, fneg, "fneg");
  Reg(T_63, 64, mcrfs, "mcrfs");
  Reg(T_63, 70, mtfsb0, "mtfsb0");
  Reg(T_63, 72, fmr, "fmr");
  Reg(T_63, 134, mtfsfi, "mtfsfi");
  Reg(T_63, 136, fnabs, "fnabs");
  Reg(T_63, 264, fabs_, "fabs");
  Reg(T_63, 583, mffs, "mffs");
  Reg(T_63, 711, mtfsf, "mtfsf");
}

void RegisterPaired() {
  for (int s = 0; s < 64; s++) {
    int scale = (s & 0x20) ? s - 64 : s;  // 6-bit signed
    s_dequant[s] = std::ldexp(1.0f, -scale);
    s_quant[s] = std::ldexp(1.0f, scale);
  }

  Reg(T_PRIMARY, 56, psq_l, "psq_l");
  Reg(T_PRIMARY, 57, psq_lu, "psq_lu");
  Reg(T_PRIMARY, 60, psq_st, "psq_st");
  Reg(T_PRIMARY, 61, psq_stu, "psq_stu");

  RegA(T_4, 10, ps_sum0, "ps_sum0");
  RegA(T_4, 11, ps_sum1, "ps_sum1");
  RegA(T_4, 12, ps_muls0, "ps_muls0");
  RegA(T_4, 13, ps_muls1, "ps_muls1");
  RegA(T_4, 14, ps_madds0, "ps_madds0");
  RegA(T_4, 15, ps_madds1, "ps_madds1");
  RegA(T_4, 18, ps_div, "ps_div");
  RegA(T_4, 20, ps_sub, "ps_sub");
  RegA(T_4, 21, ps_add, "ps_add");
  RegA(T_4, 23, ps_sel, "ps_sel");
  RegA(T_4, 24, ps_res, "ps_res");
  RegA(T_4, 25, ps_mul, "ps_mul");
  RegA(T_4, 26, ps_rsqrte, "ps_rsqrte");
  RegA(T_4, 28, ps_msub, "ps_msub");
  RegA(T_4, 29, ps_madd, "ps_madd");
  RegA(T_4, 30, ps_nmsub, "ps_nmsub");
  RegA(T_4, 31, ps_nmadd, "ps_nmadd");

  // 6-bit extended opcodes: the index register field (bits 21..24) is free.
  for (u32 k = 0; k < 16; k++) {
    Reg(T_4, (k << 6) | 6, psq_lx, "psq_lx");
    Reg(T_4, (k << 6) | 7, psq_stx, "psq_stx");
    Reg(T_4, (k << 6) | 38, psq_lux, "psq_lux");
    Reg(T_4, (k << 6) | 39, psq_stux, "psq_stux");
  }

  Reg(T_4, 0, ps_cmpu0, "ps_cmpu0");
  Reg(T_4, 32, ps_cmpu0, "ps_cmpo0");
  Reg(T_4, 40, ps_neg, "ps_neg");
  Reg(T_4, 64, ps_cmpu1, "ps_cmpu1");
  Reg(T_4, 72, ps_mr, "ps_mr");
  Reg(T_4, 96, ps_cmpu1, "ps_cmpo1");
  Reg(T_4, 136, ps_nabs, "ps_nabs");
  Reg(T_4, 264, ps_abs, "ps_abs");
  Reg(T_4, 528, ps_merge00, "ps_merge00");
  Reg(T_4, 560, ps_merge01, "ps_merge01");
  Reg(T_4, 592, ps_merge10, "ps_merge10");
  Reg(T_4, 624, ps_merge11, "ps_merge11");
  Reg(T_4, 1014, DcbzL, "dcbz_l");
}

}  // namespace Interpreter
