// SPDX-License-Identifier: GPL-2.0-or-later
// Command Processor / Pixel Engine registers and the GX FIFO command parser.
// Milestone 1 does not rasterize: the parser tracks state so draw-done/token
// synchronisation works, and EFB->XFB copies write the clear colour.
#include <vector>

#include "core/hw/hw.h"
#include "core/memory.h"

namespace GX {

namespace {
// ---- CP / PE MMIO ----
u16 s_cp_regs[0x40];
u16 s_pe_ctrl;
u16 s_pe_token;

enum : u16 {
  PE_TOKEN_ENABLE = 1 << 0,
  PE_FINISH_ENABLE = 1 << 1,
  PE_TOKEN_INT = 1 << 2,
  PE_FINISH_INT = 1 << 3,
};

void UpdatePEInterrupts() {
  PI::SetInterrupt(PI::INT_PE_TOKEN, (s_pe_ctrl & PE_TOKEN_INT) && (s_pe_ctrl & PE_TOKEN_ENABLE));
  PI::SetInterrupt(PI::INT_PE_FINISH, (s_pe_ctrl & PE_FINISH_INT) && (s_pe_ctrl & PE_FINISH_ENABLE));
}

// ---- Internal GPU state ----
u32 s_cp[0x100];  // CP internal registers (VCD, VAT, array bases/strides)
u32 s_bp[0x100];  // BP (raster/pixel) registers
u32 s_xf_writes;  // XF registers are not stored yet; counted for debugging
std::vector<u8> s_pending;
u32 s_efb_color = 0xFF000000;  // ARGB the EFB was last cleared to

// Component sizes for formats u8, s8, u16, s16, f32
const u32 kFmtSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};
const u32 kColorSize[8] = {2, 3, 4, 2, 3, 4, 0, 0};

u32 IndexSize(u32 mode, u32 direct_size) {
  switch (mode) {
    case 1: return direct_size;
    case 2: return 1;
    case 3: return 2;
    default: return 0;
  }
}

u32 VertexSize(u32 vat) {
  u32 vcd_lo = s_cp[0x50], vcd_hi = s_cp[0x60];
  u32 a = s_cp[0x70 + vat], b = s_cp[0x80 + vat], c = s_cp[0x90 + vat];
  u32 size = 0;
  if (vcd_lo & 1) size++;                      // position/normal matrix index
  for (int t = 0; t < 8; t++)
    if (vcd_lo & (2u << t)) size++;            // texcoord matrix indices

  u32 pos_comps = (a & 1) ? 3 : 2;
  size += IndexSize((vcd_lo >> 9) & 3, pos_comps * kFmtSize[(a >> 1) & 7]);

  u32 nrm_mode = (vcd_lo >> 11) & 3;
  if (nrm_mode) {
    bool nbt = (a >> 9) & 1;
    u32 comps = nbt ? 9 : 3;
    u32 direct = comps * kFmtSize[(a >> 10) & 7];
    if (nrm_mode == 1)
      size += direct;
    else
      size += (nrm_mode == 2 ? 1 : 2) * ((nbt && (a >> 31)) ? 3 : 1);
  }

  size += IndexSize((vcd_lo >> 13) & 3, kColorSize[(a >> 14) & 7]);
  size += IndexSize((vcd_lo >> 15) & 3, kColorSize[(a >> 18) & 7]);

  // Texcoord count/format bits, scattered over VAT A/B/C.
  const u32 tc_cnt[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1,
                         (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
  const u32 tc_fmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7,
                         (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
  for (int t = 0; t < 8; t++) {
    u32 mode = (vcd_hi >> (t * 2)) & 3;
    size += IndexSize(mode, (tc_cnt[t] + 1) * kFmtSize[tc_fmt[t]]);
  }
  return size;
}

void YUVFromARGB(u32 argb, u8& y, u8& u, u8& v) {
  int r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
  y = (u8)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
  u = (u8)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
  v = (u8)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

void CopyEFB(u32 cmd) {
  bool clear = (cmd >> 11) & 1;
  bool to_xfb = (cmd >> 14) & 1;
  if (to_xfb) {
    u32 dest = (s_bp[0x4B] & 0x00FFFFFF) << 5;
    u32 stride = (s_bp[0x4D] & 0x3FF) << 5;
    u32 width = (s_bp[0x4A] & 0x3FF) + 1;
    u32 height = ((s_bp[0x4A] >> 10) & 0x3FF) + 1;
    u8 y, u, v;
    YUVFromARGB(s_efb_color, y, u, v);
    for (u32 row = 0; row < height; row++) {
      u8* p = Mem::PhysPtr(dest + row * stride, width * 2);
      if (!p) break;
      for (u32 x = 0; x + 1 < width; x += 2, p += 4) {
        p[0] = y;
        p[1] = u;
        p[2] = y;
        p[3] = v;
      }
    }
  }
  if (clear) {
    u32 ar = s_bp[0x4F], gb = s_bp[0x50];
    s_efb_color = ((ar & 0xFF00) << 16) | ((ar & 0xFF) << 16) | ((gb & 0xFF00)) | (gb & 0xFF);
  }
}

void WriteBP(u32 value) {
  u32 reg = value >> 24, data = value & 0x00FFFFFF;
  // BP mask register (0xFE) applies to the next write only.
  if (reg != 0xFE && s_bp[0xFE] != 0x00FFFFFF) {
    data = (s_bp[reg] & ~s_bp[0xFE]) | (data & s_bp[0xFE]);
    s_bp[0xFE] = 0x00FFFFFF;
  }
  s_bp[reg] = data;
  switch (reg) {
    case 0x45:  // PE_DONE: draw done
      s_pe_ctrl |= PE_FINISH_INT;
      UpdatePEInterrupts();
      break;
    case 0x47:  // PE_TOKEN_INT
      s_pe_token = (u16)data;
      s_pe_ctrl |= PE_TOKEN_INT;
      UpdatePEInterrupts();
      break;
    case 0x48:  // PE_TOKEN
      s_pe_token = (u16)data;
      break;
    case 0x52:  // EFB copy execute
      CopyEFB(data);
      break;
    default: break;
  }
}

// Parses one command at p (avail bytes). Returns bytes consumed, or 0 if incomplete.
u32 ParseCommand(const u8* p, u32 avail, bool in_display_list);

void RunDisplayList(u32 addr, u32 size) {
  const u8* p = Mem::PhysPtr(addr & 0x03FFFFFF, size);
  if (!p) return;
  u32 pos = 0;
  while (pos < size) {
    u32 n = ParseCommand(p + pos, size - pos, true);
    if (n == 0) break;
    pos += n;
  }
}

u32 ParseCommand(const u8* p, u32 avail, bool in_display_list) {
  u8 cmd = p[0];
  switch (cmd) {
    case 0x00:  // NOP
    case 0x44:  // unknown metrics
    case 0x48:  // invalidate vertex cache
      return 1;
    case 0x08:  // load CP register
      if (avail < 6) return 0;
      s_cp[p[1]] = LoadBE32(p + 2);
      return 6;
    case 0x10: {  // load XF registers
      if (avail < 5) return 0;
      u32 n = ((LoadBE32(p + 1) >> 16) & 0xF) + 1;
      if (avail < 5 + n * 4) return 0;
      s_xf_writes += n;
      return 5 + n * 4;
    }
    case 0x20:
    case 0x28:
    case 0x30:
    case 0x38:  // indexed XF loads
      return avail < 5 ? 0 : 5;
    case 0x40:  // call display list
      if (avail < 9) return 0;
      if (!in_display_list) RunDisplayList(LoadBE32(p + 1), LoadBE32(p + 5));
      return 9;
    case 0x61:  // load BP register
      if (avail < 5) return 0;
      WriteBP(LoadBE32(p + 1));
      return 5;
    default:
      if (cmd & 0x80) {  // draw primitive
        if (avail < 3) return 0;
        u32 count = LoadBE16(p + 1);
        u32 total = 3 + count * VertexSize(cmd & 7);
        return avail < total ? 0 : total;
      }
      {
        static int warn = 0;
        if (warn++ < 16) LOG("GX: unknown FIFO opcode %02x\n", cmd);
      }
      return 1;
  }
}
}  // namespace

void Reset() {
  memset(s_cp_regs, 0, sizeof(s_cp_regs));
  memset(s_cp, 0, sizeof(s_cp));
  memset(s_bp, 0, sizeof(s_bp));
  s_bp[0xFE] = 0x00FFFFFF;
  s_pe_ctrl = 0;
  s_pe_token = 0;
  s_xf_writes = 0;
  s_pending.clear();
  s_efb_color = 0xFF000000;
  UpdatePEInterrupts();
}

void ProcessFifo(const u8* data, u32 len) {
  s_pending.insert(s_pending.end(), data, data + len);
  u32 pos = 0;
  while (pos < s_pending.size()) {
    u32 n = ParseCommand(s_pending.data() + pos, (u32)s_pending.size() - pos, false);
    if (n == 0) break;
    pos += n;
  }
  s_pending.erase(s_pending.begin(), s_pending.begin() + pos);
}

u16 CPRead16(u32 off) {
  switch (off) {
    case 0x00: return 0x0002 | 0x0004 | 0x0008;  // FIFO empty, GP read idle, command idle
    default: return off < 0x80 ? s_cp_regs[off >> 1] : 0;
  }
}

void CPWrite16(u32 off, u16 v) {
  if (off < 0x80) s_cp_regs[off >> 1] = v;
  // The FIFO is consumed as soon as it is written: keep read pointer == write pointer.
  if (off == 0x34 || off == 0x36) {
    s_cp_regs[0x38 >> 1] = s_cp_regs[0x34 >> 1];
    s_cp_regs[0x3A >> 1] = s_cp_regs[0x36 >> 1];
    s_cp_regs[0x30 >> 1] = 0;
    s_cp_regs[0x32 >> 1] = 0;
  }
}

u16 PERead16(u32 off) {
  switch (off) {
    case 0x0A: return s_pe_ctrl;
    case 0x0E: return s_pe_token;
    default: return 0;
  }
}

void PEWrite16(u32 off, u16 v) {
  if (off == 0x0A) {
    u16 ints = PE_TOKEN_INT | PE_FINISH_INT;
    s_pe_ctrl = (s_pe_ctrl & ints & ~(v & ints)) | (v & (PE_TOKEN_ENABLE | PE_FINISH_ENABLE));
    UpdatePEInterrupts();
  } else if (off == 0x0E) {
    s_pe_token = v;
  }
}

}  // namespace GX
