// SPDX-License-Identifier: GPL-2.0-or-later
// External Interface: three channels, with the IPL/RTC/SRAM device on channel 0.
// Memory cards and other devices report "not connected" for now.
#include <ctime>

#include "core/hw/hw.h"
#include "core/memory.h"

namespace EXI {

namespace {
enum : u32 {
  CSR_EXIINTMASK = 1 << 0,
  CSR_EXIINT = 1 << 1,
  CSR_TCINTMASK = 1 << 2,
  CSR_TCINT = 1 << 3,
  CSR_CS_SHIFT = 7,
  CSR_EXTINTMASK = 1 << 10,
  CSR_EXTINT = 1 << 11,
  CSR_INT_BITS = CSR_EXIINT | CSR_TCINT | CSR_EXTINT,
};

struct Channel {
  u32 csr, mar, length, cr, data;
};
Channel s_ch[3];

// ---- IPL device: mask ROM (not shipped), RTC and SRAM ----
struct IPLDevice {
  bool have_address = false;
  u32 address = 0;
  u32 position = 0;
  u8 sram[64];

  void Deselect() {
    have_address = false;
    position = 0;
  }

  // Returns a pointer to the addressed register file, or null for mask ROM.
  u8* Region(u32& size) {
    static u8 rtc[4];
    u32 region = address & 0x7FFFFF00;
    if (region == 0x20000000) {
      u32 now = (u32)(time(nullptr) - 946684800);  // seconds since 2000-01-01
      StoreBE32(rtc, now);
      size = 4;
      return rtc;
    }
    if (region == 0x20000100) {
      size = sizeof(sram);
      return sram;
    }
    size = 0;
    return nullptr;
  }

  u8 TransferByte(u8 in) {
    if (!have_address) {
      address = (address << 8) | in;
      if (++position == 4) {
        have_address = true;
        position = 0;
      }
      return 0xFF;
    }
    u32 size;
    u8* reg = Region(size);
    u8 out = 0;
    if (reg && position < size) {
      if (address & 0x80000000)
        reg[position] = in;
      else
        out = reg[position];
    }
    position++;
    return out;
  }
};
IPLDevice s_ipl;

void InitSRAM() {
  u8* s = s_ipl.sram;
  memset(s, 0, 64);
  s[0x11] = 0x00;  // NTSC, non-progressive
  s[0x12] = 0x00;  // English
  s[0x13] = 0x2C;  // flags: stereo sound, settings initialised
  u16 sum = 0, inv = 0;
  for (int i = 0; i < 4; i++) {
    u16 w = LoadBE16(s + 0x0C + i * 2);
    sum += w;
    inv += (u16)~w;
  }
  StoreBE16(s + 0, sum);
  StoreBE16(s + 2, inv);
}

int SelectedDevice(const Channel& c) {
  u32 cs = (c.csr >> CSR_CS_SHIFT) & 7;
  if (cs & 1) return 0;
  if (cs & 2) return 1;
  if (cs & 4) return 2;
  return -1;
}

bool HasIPL(int ch, int dev) { return ch == 0 && dev == 1; }

u8 Transfer(int ch, int dev, u8 in) { return HasIPL(ch, dev) ? s_ipl.TransferByte(in) : 0; }

void UpdateInterrupts() {
  bool active = false;
  for (auto& c : s_ch) {
    u32 v = c.csr;
    if (((v & CSR_EXIINT) && (v & CSR_EXIINTMASK)) || ((v & CSR_TCINT) && (v & CSR_TCINTMASK)) ||
        ((v & CSR_EXTINT) && (v & CSR_EXTINTMASK)))
      active = true;
  }
  PI::SetInterrupt(PI::INT_EXI, active);
}

void StartTransfer(int ch) {
  Channel& c = s_ch[ch];
  int dev = SelectedDevice(c);
  u32 rw = (c.cr >> 2) & 3;
  if (c.cr & 2) {  // DMA
    u32 len = c.length & ~31u;
    u8* mem = Mem::PhysPtr(c.mar & 0x03FFFFE0, len);
    if (mem) {
      for (u32 k = 0; k < len; k++) {
        if (rw == 0)
          mem[k] = Transfer(ch, dev, 0);
        else
          Transfer(ch, dev, mem[k]);
      }
    }
  } else {  // immediate, 1..4 bytes MSB first
    u32 len = ((c.cr >> 4) & 3) + 1;
    u32 result = 0;
    for (u32 k = 0; k < len; k++) {
      u8 in = (u8)(c.data >> (24 - 8 * k));
      u8 out = Transfer(ch, dev, rw == 0 ? 0 : in);
      result |= (u32)out << (24 - 8 * k);
    }
    if (rw != 1) c.data = result;
  }
  c.cr &= ~1u;
  c.csr |= CSR_TCINT;
  UpdateInterrupts();
}
}  // namespace

void Reset() {
  memset(s_ch, 0, sizeof(s_ch));
  s_ipl = IPLDevice();
  InitSRAM();
  UpdateInterrupts();
}

u32 Read32(u32 off) {
  int ch = off / 0x14;
  if (ch > 2) return 0;
  Channel& c = s_ch[ch];
  switch (off % 0x14) {
    case 0x00: return c.csr;  // EXT bit stays 0: no memory card inserted
    case 0x04: return c.mar;
    case 0x08: return c.length;
    case 0x0C: return c.cr;
    default: return c.data;
  }
}

void Write32(u32 off, u32 v) {
  int ch = off / 0x14;
  if (ch > 2) return;
  Channel& c = s_ch[ch];
  switch (off % 0x14) {
    case 0x00: {
      int old_dev = SelectedDevice(c);
      u32 cleared = c.csr & ~(v & CSR_INT_BITS) & CSR_INT_BITS;
      c.csr = (v & ~CSR_INT_BITS) | cleared;
      c.csr &= ~(1u << 12);  // EXT: nothing attached
      int new_dev = SelectedDevice(c);
      if (old_dev != new_dev && HasIPL(ch, 1)) s_ipl.Deselect();
      UpdateInterrupts();
      break;
    }
    case 0x04: c.mar = v & 0x03FFFFE0; break;
    case 0x08: c.length = v & 0x03FFFFE0; break;
    case 0x0C:
      c.cr = v;
      if (v & 1) StartTransfer(ch);
      break;
    default: c.data = v; break;
  }
}

}  // namespace EXI
