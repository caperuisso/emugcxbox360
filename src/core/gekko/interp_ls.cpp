// SPDX-License-Identifier: GPL-2.0-or-later
// Integer and floating point loads/stores, string/multiple ops, cache control.
#include "core/gekko/block_cache.h"
#include "core/gekko/interp_internal.h"

namespace Interpreter {

namespace {

inline u32 EA_D(u32 i) { return RA0(i) + (u32)SIMM(i); }
inline u32 EA_X(u32 i) { return RA0(i) + GPR(RB(i)); }
inline u32 EA_DU(u32 i) { return GPR(RA(i)) + (u32)SIMM(i); }
inline u32 EA_XU(u32 i) { return GPR(RA(i)) + GPR(RB(i)); }

// Generates the four addressing variants of an integer load/store.
#define LOAD_OPS(name, expr)                                          \
  void name(u32 i) {                                                  \
    u32 v = (expr)(EA_D(i));                                          \
    if (NoFault()) GPR(RD(i)) = v;                                    \
  }                                                                   \
  void name##u(u32 i) {                                               \
    u32 ea = EA_DU(i), v = (expr)(ea);                                \
    if (NoFault()) {                                                  \
      GPR(RD(i)) = v;                                                 \
      GPR(RA(i)) = ea;                                                \
    }                                                                 \
  }                                                                   \
  void name##x(u32 i) {                                               \
    u32 v = (expr)(EA_X(i));                                          \
    if (NoFault()) GPR(RD(i)) = v;                                    \
  }                                                                   \
  void name##ux(u32 i) {                                              \
    u32 ea = EA_XU(i), v = (expr)(ea);                                \
    if (NoFault()) {                                                  \
      GPR(RD(i)) = v;                                                 \
      GPR(RA(i)) = ea;                                                \
    }                                                                 \
  }

inline u32 LoadW(u32 ea) { return Mem::Read32(ea); }
inline u32 LoadB(u32 ea) { return Mem::Read8(ea); }
inline u32 LoadH(u32 ea) { return Mem::Read16(ea); }
inline u32 LoadHA(u32 ea) { return (u32)(s32)(s16)Mem::Read16(ea); }

LOAD_OPS(lwz, LoadW)
LOAD_OPS(lbz, LoadB)
LOAD_OPS(lhz, LoadH)
LOAD_OPS(lha, LoadHA)
#undef LOAD_OPS

#define STORE_OPS(name, fn, type)                       \
  void name(u32 i) { fn(EA_D(i), (type)GPR(RS(i))); }   \
  void name##u(u32 i) {                                 \
    u32 ea = EA_DU(i);                                  \
    fn(ea, (type)GPR(RS(i)));                           \
    if (NoFault()) GPR(RA(i)) = ea;                     \
  }                                                     \
  void name##x(u32 i) { fn(EA_X(i), (type)GPR(RS(i))); } \
  void name##ux(u32 i) {                                \
    u32 ea = EA_XU(i);                                  \
    fn(ea, (type)GPR(RS(i)));                           \
    if (NoFault()) GPR(RA(i)) = ea;                     \
  }

STORE_OPS(stw, Mem::Write32, u32)
STORE_OPS(stb, Mem::Write8, u8)
STORE_OPS(sth, Mem::Write16, u16)
#undef STORE_OPS

void lwbrx(u32 i) {
  u32 v = Swap32(Mem::Read32(EA_X(i)));
  if (NoFault()) GPR(RD(i)) = v;
}
void lhbrx(u32 i) {
  u32 v = Swap16(Mem::Read16(EA_X(i)));
  if (NoFault()) GPR(RD(i)) = v;
}
void stwbrx(u32 i) { Mem::Write32(EA_X(i), Swap32(GPR(RS(i)))); }
void sthbrx(u32 i) { Mem::Write16(EA_X(i), Swap16((u16)GPR(RS(i)))); }

void lmw(u32 i) {
  u32 ea = EA_D(i);
  for (u32 r = RD(i); r < 32; r++, ea += 4) {
    u32 v = Mem::Read32(ea);
    if (!NoFault()) return;
    GPR(r) = v;
  }
}
void stmw(u32 i) {
  u32 ea = EA_D(i);
  for (u32 r = RS(i); r < 32; r++, ea += 4) {
    Mem::Write32(ea, GPR(r));
    if (!NoFault()) return;
  }
}

void LoadString(u32 ea, u32 rd, u32 n) {
  u32 r = rd, shift = 24;
  if (n) GPR(r) = 0;
  for (u32 k = 0; k < n; k++) {
    u8 b = Mem::Read8(ea + k);
    if (!NoFault()) return;
    GPR(r) |= (u32)b << shift;
    if (shift == 0) {
      shift = 24;
      r = (r + 1) & 31;
      if (k + 1 < n) GPR(r) = 0;
    } else {
      shift -= 8;
    }
  }
}
void StoreString(u32 ea, u32 rs, u32 n) {
  u32 r = rs, shift = 24;
  for (u32 k = 0; k < n; k++) {
    Mem::Write8(ea + k, (u8)(GPR(r) >> shift));
    if (!NoFault()) return;
    if (shift == 0) {
      shift = 24;
      r = (r + 1) & 31;
    } else {
      shift -= 8;
    }
  }
}
void lswi(u32 i) { LoadString(RA0(i), RD(i), RB(i) ? RB(i) : 32); }
void stswi(u32 i) { StoreString(RA0(i), RS(i), RB(i) ? RB(i) : 32); }
void lswx(u32 i) { LoadString(EA_X(i), RD(i), cpu.xer & 0x7F); }
void stswx(u32 i) { StoreString(EA_X(i), RS(i), cpu.xer & 0x7F); }

void lwarx(u32 i) {
  u32 ea = EA_X(i), v = Mem::Read32(ea);
  if (!NoFault()) return;
  GPR(RD(i)) = v;
  cpu.reserve = true;
  cpu.reserve_addr = ea;
}
void stwcx(u32 i) {
  u32 ea = EA_X(i);
  u32 f = (cpu.xer & XER_SO) ? 1 : 0;
  if (cpu.reserve && cpu.reserve_addr == ea) {
    Mem::Write32(ea, GPR(RS(i)));
    if (!NoFault()) return;
    f |= 2;
  }
  cpu.reserve = false;
  SetCRField(0, f);
}

// ---- Floating point loads/stores ----
inline void LoadSingle(u32 rd, u32 ea) {
  u32 v = Mem::Read32(ea);
  if (NoFault()) FPR(rd).fill((double)BitCast<float>(v));
}
inline void LoadDouble(u32 rd, u32 ea) {
  u64 v = Mem::Read64(ea);
  if (NoFault()) FPR(rd).ps0 = v;
}

void lfs(u32 i) { if (CheckFP()) LoadSingle(RD(i), EA_D(i)); }
void lfsx(u32 i) { if (CheckFP()) LoadSingle(RD(i), EA_X(i)); }
void lfsu(u32 i) {
  if (!CheckFP()) return;
  u32 ea = EA_DU(i);
  LoadSingle(RD(i), ea);
  if (NoFault()) GPR(RA(i)) = ea;
}
void lfsux(u32 i) {
  if (!CheckFP()) return;
  u32 ea = EA_XU(i);
  LoadSingle(RD(i), ea);
  if (NoFault()) GPR(RA(i)) = ea;
}
void lfd(u32 i) { if (CheckFP()) LoadDouble(RD(i), EA_D(i)); }
void lfdx(u32 i) { if (CheckFP()) LoadDouble(RD(i), EA_X(i)); }
void lfdu(u32 i) {
  if (!CheckFP()) return;
  u32 ea = EA_DU(i);
  LoadDouble(RD(i), ea);
  if (NoFault()) GPR(RA(i)) = ea;
}
void lfdux(u32 i) {
  if (!CheckFP()) return;
  u32 ea = EA_XU(i);
  LoadDouble(RD(i), ea);
  if (NoFault()) GPR(RA(i)) = ea;
}

inline void StoreSingle(u32 rs, u32 ea) { Mem::Write32(ea, BitCast<u32>((float)FPR(rs).d0())); }
inline void StoreDouble(u32 rs, u32 ea) { Mem::Write64(ea, FPR(rs).ps0); }

void stfs(u32 i) { if (CheckFP()) StoreSingle(RS(i), EA_D(i)); }
void stfsx(u32 i) { if (CheckFP()) StoreSingle(RS(i), EA_X(i)); }
void stfsu(u32 i) {
  if (!CheckFP()) return;
  u32 ea = EA_DU(i);
  StoreSingle(RS(i), ea);
  if (NoFault()) GPR(RA(i)) = ea;
}
void stfsux(u32 i) {
  if (!CheckFP()) return;
  u32 ea = EA_XU(i);
  StoreSingle(RS(i), ea);
  if (NoFault()) GPR(RA(i)) = ea;
}
void stfd(u32 i) { if (CheckFP()) StoreDouble(RS(i), EA_D(i)); }
void stfdx(u32 i) { if (CheckFP()) StoreDouble(RS(i), EA_X(i)); }
void stfdu(u32 i) {
  if (!CheckFP()) return;
  u32 ea = EA_DU(i);
  StoreDouble(RS(i), ea);
  if (NoFault()) GPR(RA(i)) = ea;
}
void stfdux(u32 i) {
  if (!CheckFP()) return;
  u32 ea = EA_XU(i);
  StoreDouble(RS(i), ea);
  if (NoFault()) GPR(RA(i)) = ea;
}
void stfiwx(u32 i) {
  if (CheckFP()) Mem::Write32(EA_X(i), (u32)FPR(RS(i)).ps0);
}

// ---- Cache control ----
void dcbz(u32 i) {
  u32 ea = EA_X(i) & ~31u;
  u32 pa;
  if (Mem::TranslateData(ea, pa)) {
    if (u8* p = Mem::PhysPtr(pa, 32)) {
      memset(p, 0, 32);
      return;
    }
  }
  for (u32 k = 0; k < 32; k += 8) Mem::Write64(ea + k, 0);
}
void cache_nop(u32) {}

void icbi(u32 i) {
  u32 pa;
  if (Mem::TranslateData(EA_X(i) & ~31u, pa)) BlockCache::Invalidate(pa, 32);
}

}  // namespace

// dcbz_l (paired-single group) shares the dcbz implementation.
void DcbzL(u32 inst) { dcbz(inst); }

void RegisterLoadStore() {
  Reg(T_PRIMARY, 32, lwz, "lwz");
  Reg(T_PRIMARY, 33, lwzu, "lwzu");
  Reg(T_PRIMARY, 34, lbz, "lbz");
  Reg(T_PRIMARY, 35, lbzu, "lbzu");
  Reg(T_PRIMARY, 36, stw, "stw");
  Reg(T_PRIMARY, 37, stwu, "stwu");
  Reg(T_PRIMARY, 38, stb, "stb");
  Reg(T_PRIMARY, 39, stbu, "stbu");
  Reg(T_PRIMARY, 40, lhz, "lhz");
  Reg(T_PRIMARY, 41, lhzu, "lhzu");
  Reg(T_PRIMARY, 42, lha, "lha");
  Reg(T_PRIMARY, 43, lhau, "lhau");
  Reg(T_PRIMARY, 44, sth, "sth");
  Reg(T_PRIMARY, 45, sthu, "sthu");
  Reg(T_PRIMARY, 46, lmw, "lmw");
  Reg(T_PRIMARY, 47, stmw, "stmw");
  Reg(T_PRIMARY, 48, lfs, "lfs");
  Reg(T_PRIMARY, 49, lfsu, "lfsu");
  Reg(T_PRIMARY, 50, lfd, "lfd");
  Reg(T_PRIMARY, 51, lfdu, "lfdu");
  Reg(T_PRIMARY, 52, stfs, "stfs");
  Reg(T_PRIMARY, 53, stfsu, "stfsu");
  Reg(T_PRIMARY, 54, stfd, "stfd");
  Reg(T_PRIMARY, 55, stfdu, "stfdu");

  Reg(T_31, 20, lwarx, "lwarx");
  Reg(T_31, 23, lwzx, "lwzx");
  Reg(T_31, 54, cache_nop, "dcbst");
  Reg(T_31, 55, lwzux, "lwzux");
  Reg(T_31, 86, cache_nop, "dcbf");
  Reg(T_31, 87, lbzx, "lbzx");
  Reg(T_31, 119, lbzux, "lbzux");
  Reg(T_31, 150, stwcx, "stwcx.");
  Reg(T_31, 151, stwx, "stwx");
  Reg(T_31, 183, stwux, "stwux");
  Reg(T_31, 215, stbx, "stbx");
  Reg(T_31, 246, cache_nop, "dcbtst");
  Reg(T_31, 247, stbux, "stbux");
  Reg(T_31, 278, cache_nop, "dcbt");
  Reg(T_31, 279, lhzx, "lhzx");
  Reg(T_31, 310, cache_nop, "eciwx");
  Reg(T_31, 311, lhzux, "lhzux");
  Reg(T_31, 343, lhax, "lhax");
  Reg(T_31, 375, lhaux, "lhaux");
  Reg(T_31, 407, sthx, "sthx");
  Reg(T_31, 438, cache_nop, "ecowx");
  Reg(T_31, 439, sthux, "sthux");
  Reg(T_31, 470, cache_nop, "dcbi");
  Reg(T_31, 533, lswx, "lswx");
  Reg(T_31, 534, lwbrx, "lwbrx");
  Reg(T_31, 535, lfsx, "lfsx");
  Reg(T_31, 567, lfsux, "lfsux");
  Reg(T_31, 597, lswi, "lswi");
  Reg(T_31, 599, lfdx, "lfdx");
  Reg(T_31, 631, lfdux, "lfdux");
  Reg(T_31, 661, stswx, "stswx");
  Reg(T_31, 662, stwbrx, "stwbrx");
  Reg(T_31, 663, stfsx, "stfsx");
  Reg(T_31, 695, stfsux, "stfsux");
  Reg(T_31, 725, stswi, "stswi");
  Reg(T_31, 727, stfdx, "stfdx");
  Reg(T_31, 759, stfdux, "stfdux");
  Reg(T_31, 790, lhbrx, "lhbrx");
  Reg(T_31, 918, sthbrx, "sthbrx");
  Reg(T_31, 982, icbi, "icbi");
  Reg(T_31, 983, stfiwx, "stfiwx");
  Reg(T_31, 1014, dcbz, "dcbz");
}

}  // namespace Interpreter
