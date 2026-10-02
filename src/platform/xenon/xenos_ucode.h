// emugcxbox360 - Xenos (Xbox 360 GPU) shader microcode builder
// SPDX-License-Identifier: GPL-2.0-or-later
//
// libxenon only loads shaders precompiled by Microsoft's tools, so the GPU
// backend assembles its own microcode. Instruction layouts follow Xenia's
// src/xenia/gpu/ucode.h and were checked against the XDK-compiled shaders of
// the libxenon "cube" sample (tools/xenos_disasm.py decodes both).
//
// A shader is a list of control-flow instructions (two per three dwords)
// followed by ALU/fetch instructions (three dwords each). Exec clauses hold up
// to 6 instructions; the builder splits them and emits the allocs between the
// fetch, position-export and interpolator/color-export phases.
//
// Portable (no libxenon dependency) so it can be unit-tested on the PC.
#pragma once

#include <cstdint>
#include <vector>

namespace Xenos {

// ---- Opcodes ----
enum VectorOp : uint32_t {
  V_ADD = 0, V_MUL = 1, V_MAX = 2, V_MIN = 3, V_SEQ = 4, V_SGT = 5, V_SGE = 6, V_SNE = 7,
  V_FRC = 8, V_TRUNC = 9, V_FLOOR = 10, V_MAD = 11, V_CNDEQ = 12, V_CNDGE = 13, V_CNDGT = 14,
  V_DP4 = 15, V_DP3 = 16, V_DP2ADD = 17, V_CUBE = 18, V_MAX4 = 19,
  V_KILLEQ = 24, V_KILLGT = 25, V_KILLGE = 26, V_KILLNE = 27,
};
enum ScalarOp : uint32_t {
  S_ADDS = 0, S_MULS = 2, S_MAXS = 5, S_MINS = 6, S_FRCS = 11, S_FLOORS = 13, S_EXP = 14, S_LOG = 16,
  S_RCP = 19, S_RSQ = 22, S_SUBS = 25, S_SQRT = 40, S_RETAIN_PREV = 50,
};

// Export register numbers
constexpr uint32_t EXPORT_POSITION = 62;  // vertex shader oPos
constexpr uint32_t EXPORT_COLOR0 = 0;     // pixel shader oC0 / vertex interpolator 0

// Vertex fetch data formats (xenos::VertexFormat)
constexpr uint32_t VFMT_8_8_8_8 = 6;
constexpr uint32_t VFMT_32_32_FLOAT = 37;
constexpr uint32_t VFMT_32_32_32_32_FLOAT = 38;
constexpr uint32_t VFMT_32_32_32_FLOAT = 57;

// Fetch destination swizzles: 3 bits per component (0-3 = xyzw, 4 = 0, 5 = 1, 7 = keep)
constexpr uint32_t FetchSwz(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | (y << 3) | (z << 6) | (w << 9); }
constexpr uint32_t FETCH_XYZW = FetchSwz(0, 1, 2, 3);
constexpr uint32_t FETCH_XY01 = FetchSwz(0, 1, 4, 5);

// Swizzles are written as 4 absolute components, e.g. Swz(0,1,2,3) = xyzw.
constexpr uint32_t Swz(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | (y << 2) | (z << 4) | (w << 6); }
constexpr uint32_t SWZ_XYZW = Swz(0, 1, 2, 3);
constexpr uint32_t SWZ_XXXX = Swz(0, 0, 0, 0);
constexpr uint32_t SWZ_YYYY = Swz(1, 1, 1, 1);
constexpr uint32_t SWZ_ZZZZ = Swz(2, 2, 2, 2);
constexpr uint32_t SWZ_WWWW = Swz(3, 3, 3, 3);

// ALU operand: temporary register or float constant, with swizzle/negate/abs.
struct Src {
  uint32_t reg = 0;
  bool is_temp = true;
  uint32_t swizzle = SWZ_XYZW;  // absolute
  bool negate = false;
  bool abs = false;  // temporaries only

  static Src R(uint32_t r, uint32_t swz = SWZ_XYZW) { Src s; s.reg = r; s.swizzle = swz; return s; }
  static Src C(uint32_t c, uint32_t swz = SWZ_XYZW) { Src s; s.reg = c; s.is_temp = false; s.swizzle = swz; return s; }
  Src Neg() const { Src s = *this; s.negate = !s.negate; return s; }
  Src Abs() const { Src s = *this; s.abs = true; return s; }
};

// The hardware stores operand swizzles relative to the destination component.
inline uint32_t RelativeSwizzle(uint32_t abs_swz) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < 4; i++) {
    uint32_t c = (abs_swz >> (2 * i)) & 3;
    r |= ((c - i) & 3) << (2 * i);
  }
  return r;
}

struct Instr {
  uint32_t w[3];
  bool fetch;
};

enum Phase { PHASE_MAIN, PHASE_POSITION, PHASE_INTERP };

class ShaderBuilder {
 public:
  explicit ShaderBuilder(bool pixel) : m_pixel(pixel) {}

  // ---- ALU (vector op + optional scalar op co-issued in one instruction) ----
  // Vector op writing temp `dst` (mask: bit0 = x ... bit3 = w).
  void Vec(VectorOp op, uint32_t dst, uint32_t mask, const Src& a, const Src& b = Src(), const Src& c = Src(),
           bool clamp = false) {
    Alu(op, dst, mask, false, a, b, c, S_RETAIN_PREV, 0, 0, Src(), clamp);
  }
  // Vector op exported (position, interpolator or pixel color).
  void VecExport(VectorOp op, uint32_t export_reg, uint32_t mask, const Src& a, const Src& b = Src(),
                 const Src& c = Src(), bool clamp = false) {
    Alu(op, export_reg, mask, true, a, b, c, S_RETAIN_PREV, 0, 0, Src(), clamp);
  }
  // Scalar op on src.ab ("a" = swizzle component w, "b" = component x) into temp dst.
  void Scalar(ScalarOp op, uint32_t dst, uint32_t mask, const Src& src, bool clamp = false) {
    Alu(V_MAX, 0, 0, false, Src(), Src(), src, op, dst, mask, src, clamp);
  }
  // Convenience moves (max(a, a) is the compiler's mov)
  void Mov(uint32_t dst, uint32_t mask, const Src& a, bool clamp = false) { Vec(V_MAX, dst, mask, a, a, Src(), clamp); }
  void MovExport(uint32_t export_reg, uint32_t mask, const Src& a, bool clamp = false) {
    VecExport(V_MAX, export_reg, mask, a, a, Src(), clamp);
  }

  // ---- Fetch ----
  // Vertex fetch from vertex fetch constant `vf` (95 = libxenon stream 0):
  // dst.xyzw <- element at dword `offset` of each `stride`-dword vertex.
  void VFetch(uint32_t dst, uint32_t index_reg, uint32_t vf, uint32_t format, uint32_t offset, uint32_t stride,
              uint32_t dst_swz = FETCH_XYZW, bool normalized = false) {
    Instr in;
    in.fetch = true;
    in.w[0] = 0 | (index_reg << 5) | (dst << 12) | (1u << 19) | ((vf / 3) << 20) | ((vf % 3) << 25);
    in.w[1] = dst_swz | ((normalized ? 0u : 1u) << 13) | (format << 16);
    in.w[2] = (stride & 0xFF) | ((offset & 0x7FFFFF) << 8);
    m_instr[m_phase].push_back(in);
  }
  // 2D texture fetch: dst.xyzw <- sample(tf, src.xy), filters from the fetch constant.
  void TFetch2D(uint32_t dst, uint32_t src, uint32_t tf, uint32_t src_swz = Swz(0, 1, 0, 0)) {
    Instr in;
    in.fetch = true;
    uint32_t sx = src_swz & 3, sy = (src_swz >> 2) & 3, sz = (src_swz >> 4) & 3;
    in.w[0] = 1 | (src << 5) | (dst << 12) | (1u << 19) | (tf << 20) | ((sx | (sy << 2) | (sz << 4)) << 26);
    in.w[1] = 0x1f1ff000 | 0x688;  // keep filters, xyzw, use computed LOD (as XDK output)
    in.w[2] = 0x00004000;          // 2D
    m_instr[m_phase].push_back(in);
  }

  // Following instructions belong to the position export / interpolator or
  // color export phase (vertex shaders: MAIN -> POSITION -> INTERP; pixel
  // shaders: MAIN -> INTERP, the color export).
  void BeginPhase(Phase p) { m_phase = p; }

  uint32_t TempCount() const { return m_temps; }
  void UseTemps(uint32_t n) { if (n > m_temps) m_temps = n; }

  // Assembles control flow + instructions.
  std::vector<uint32_t> Build() const;

 private:
  void Alu(VectorOp vop, uint32_t vdst, uint32_t vmask, bool exp, const Src& a, const Src& b, const Src& c,
           ScalarOp sop, uint32_t sdst, uint32_t smask, const Src& s, bool clamp);

  bool m_pixel;
  Phase m_phase = PHASE_MAIN;
  std::vector<Instr> m_instr[3];
  uint32_t m_temps = 1;
};

inline void ShaderBuilder::Alu(VectorOp vop, uint32_t vdst, uint32_t vmask, bool exp, const Src& a, const Src& b,
                               const Src& c, ScalarOp sop, uint32_t sdst, uint32_t smask, const Src& s, bool clamp) {
  // Operands 1, 2 are the vector op's a, b; operand 3 is c, or the scalar
  // op's source when a scalar op is issued.
  Src src[3] = {a, b, (sop != S_RETAIN_PREV) ? s : c};
  Instr in;
  in.fetch = false;
  uint32_t w0 = (vdst & 0x3F) | ((sdst & 0x3F) << 8) | ((exp ? 1u : 0u) << 15) | ((vmask & 0xF) << 16) |
                ((smask & 0xF) << 20) | ((clamp ? 1u : 0u) << 24) | ((clamp ? 1u : 0u) << 25) | (sop << 26);
  uint32_t w1 = 0, w2 = 0;
  // swizzles: src3 bits 0-7, src2 8-15, src1 16-23; negates 24 (src3), 25, 26
  for (int i = 0; i < 3; i++) {
    uint32_t sh = 16 - 8 * i;  // src1 -> 16, src2 -> 8, src3 -> 0
    w1 |= RelativeSwizzle(src[i].swizzle) << sh;
    if (src[i].negate) w1 |= 1u << (26 - i);
    uint32_t reg = src[i].reg & (src[i].is_temp ? 0x3F : 0xFF);
    if (src[i].is_temp && src[i].abs) reg |= 0x80;
    w2 |= reg << sh;
    if (src[i].is_temp) w2 |= 1u << (31 - i);
  }
  w2 |= (vop & 0x1F) << 24;
  in.w[0] = w0;
  in.w[1] = w1;
  in.w[2] = w2;
  m_instr[m_phase].push_back(in);
  if (!exp && vmask) UseTemps(vdst + 1);
  if (smask) UseTemps(sdst + 1);
}

inline std::vector<uint32_t> ShaderBuilder::Build() const {
  struct CF {
    uint32_t w0, w1;  // 32 + 16 bits
  };
  // Exec clauses: max 6 instructions each
  struct Clause {
    uint32_t first, count, seq;
  };
  std::vector<CF> cf;
  std::vector<Instr> code;
  std::vector<std::pair<size_t, Clause>> pending;  // cf index -> clause (address patched later)

  auto add_execs = [&](const std::vector<Instr>& list, bool last_overall) {
    for (size_t i = 0; i < list.size(); i += 6) {
      Clause c{(uint32_t)code.size(), 0, 0};
      for (size_t k = i; k < list.size() && k < i + 6; k++) {
        if (list[k].fetch) c.seq |= 1u << (2 * c.count);
        code.push_back(list[k]);
        c.count++;
      }
      bool end = last_overall && i + 6 >= list.size();
      pending.push_back({cf.size(), c});
      cf.push_back({0, (end ? 2u : 1u) << 12});  // exec / exec_end
    }
  };
  auto alloc = [&](uint32_t type) { cf.push_back({0, (12u << 12) | (type << 9)}); };

  if (m_pixel) {
    add_execs(m_instr[PHASE_MAIN], false);
    alloc(2);  // colors
    add_execs(m_instr[PHASE_INTERP], true);
  } else {
    add_execs(m_instr[PHASE_MAIN], false);
    alloc(1);  // position
    add_execs(m_instr[PHASE_POSITION], false);
    alloc(2);  // interpolators
    add_execs(m_instr[PHASE_INTERP], true);
  }
  if (cf.size() & 1) cf.push_back({0, 0});  // pad with a nop
  uint32_t cf_slots = (uint32_t)cf.size() / 2;
  for (auto& p : pending) {
    const Clause& c = p.second;
    CF& f = cf[p.first];
    f.w0 = (c.first + cf_slots) | (c.count << 12) | (c.seq << 16);
    f.w1 |= 1u << 9;  // as emitted by the XDK compiler
  }
  std::vector<uint32_t> out;
  for (size_t i = 0; i < cf.size(); i += 2) {
    const CF& a = cf[i];
    const CF& b = cf[i + 1];
    out.push_back(a.w0);
    out.push_back((a.w1 & 0xFFFF) | (b.w0 << 16));
    out.push_back((b.w0 >> 16) | (b.w1 << 16));
  }
  for (const Instr& in : code) {
    out.push_back(in.w[0]);
    out.push_back(in.w[1]);
    out.push_back(in.w[2]);
  }
  return out;
}

// SQ_PROGRAM_CNTL for a vertex/pixel shader pair built with ShaderBuilder.
inline uint32_t VertexProgramControl(uint32_t temps, uint32_t interpolators) {
  return ((temps ? temps - 1 : 0) & 0x3F) | (1u << 16) | (((interpolators ? interpolators - 1 : 0) & 0xF) << 20);
}
inline uint32_t PixelProgramControl(uint32_t temps) {
  return (((temps ? temps - 1 : 0) & 0x3F) << 8) | (2u << 27);
}

}  // namespace Xenos
