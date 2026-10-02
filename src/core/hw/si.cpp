// SPDX-License-Identifier: GPL-2.0-or-later
// Serial Interface: GameCube controller ports (standard pad emulation).
#include "core/hw/hw.h"
#include "core/state.h"
#include "platform/platform.h"

namespace SI {

namespace {
enum : u32 {
  COMCSR_TSTART = 0x00000001,
  COMCSR_COMERR = 0x20000000,
  COMCSR_RDSTINT = 0x10000000,
  COMCSR_RDSTINTMSK = 0x08000000,
  COMCSR_TCINTMSK = 0x40000000,
  COMCSR_TCINT = 0x80000000,
};

struct Channel {
  u32 out;     // SICxOUTBUF
  u32 in_hi;   // SICxINBUFH
  u32 in_lo;   // SICxINBUFL
};

Channel s_ch[4];
u32 s_poll;
u32 s_comcsr;
u32 s_status;  // SISR
u32 s_exilk;
u8 s_iobuf[128];
PadState s_pads[4];

constexpr u32 SR_NOREP = 0x08;
constexpr u32 SR_RDST = 0x20;
inline u32 StatusShift(int ch) { return (3 - ch) * 8; }

void UpdateInterrupt() {
  bool tc = (s_comcsr & COMCSR_TCINT) && (s_comcsr & COMCSR_TCINTMSK);
  bool rd = (s_comcsr & COMCSR_RDSTINT) && (s_comcsr & COMCSR_RDSTINTMSK);
  PI::SetInterrupt(PI::INT_SI, tc || rd);
}

void RefreshPad(int ch) {
  PadState& p = s_pads[ch];
  p = PadState();
  if (g_host) g_host->PollPad(ch, p);
}

// Standard controller response to the poll command (mode 3).
void BuildPollResponse(int ch) {
  const PadState& p = s_pads[ch];
  u32 buttons = p.buttons | 0x0080;  // bit 7 of byte 1: "use origin"
  s_ch[ch].in_hi = (buttons << 16) | ((u32)p.stick_x << 8) | p.stick_y;
  s_ch[ch].in_lo = ((u32)p.cstick_x << 24) | ((u32)p.cstick_y << 16) | ((u32)p.trigger_l << 8) | p.trigger_r;
}

void RunTransfer() {
  int ch = (s_comcsr >> 1) & 3;
  u32 out_len = (s_comcsr >> 16) & 0x7F;
  u32 in_len = (s_comcsr >> 8) & 0x7F;
  if (out_len == 0) out_len = 128;
  if (in_len == 0) in_len = 128;
  (void)out_len;

  RefreshPad(ch);
  u8 cmd = s_iobuf[0];
  u8 resp[16] = {};
  u32 resp_len = 0;
  if (!s_pads[ch].connected) {
    s_comcsr |= COMCSR_COMERR;
    s_status |= SR_NOREP << StatusShift(ch);
  } else {
    switch (cmd) {
      case 0x00:  // get type
      case 0xFF:  // reset
        resp[0] = 0x09;  // standard controller
        resp[1] = 0x00;
        resp[2] = 0x00;
        resp_len = 3;
        break;
      case 0x40: {  // direct poll
        BuildPollResponse(ch);
        StoreBE32(resp, s_ch[ch].in_hi);
        StoreBE32(resp + 4, s_ch[ch].in_lo);
        resp_len = 8;
        break;
      }
      case 0x41:  // get origin
      case 0x42:  // recalibrate
        resp[0] = 0x00;
        resp[1] = 0x80;
        resp[2] = resp[3] = resp[4] = resp[5] = 0x80;  // sticks centred
        resp[6] = resp[7] = 0x00;                      // triggers released
        resp_len = 10;
        break;
      default: LOG("SI: unknown command %02x on channel %d\n", cmd, ch); break;
    }
    s_comcsr &= ~COMCSR_COMERR;
  }
  memcpy(s_iobuf, resp, resp_len < in_len ? resp_len : in_len);
  s_comcsr &= ~COMCSR_TSTART;
  s_comcsr |= COMCSR_TCINT;
  UpdateInterrupt();
}
}  // namespace

void Reset() {
  memset(s_ch, 0, sizeof(s_ch));
  s_poll = s_comcsr = s_status = s_exilk = 0;
  memset(s_iobuf, 0, sizeof(s_iobuf));
  for (auto& p : s_pads) p = PadState();
  UpdateInterrupt();
}

void DoState(StateBuffer& s) {
  s.Marker("SI");
  s.Do(s_ch);
  s.Do(s_poll);
  s.Do(s_comcsr);
  s.Do(s_status);
  s.Do(s_exilk);
  s.Do(s_iobuf);
  s.Do(s_pads);
}

void UpdatePolling() {
  bool any = false;
  for (int ch = 0; ch < 4; ch++) {
    if (!(s_poll & (0x80u >> ch))) continue;  // EN0..EN3
    RefreshPad(ch);
    if (!s_pads[ch].connected) {
      s_status |= SR_NOREP << StatusShift(ch);
      s_ch[ch].in_hi = 0x80000000u;  // ERRSTAT
      continue;
    }
    BuildPollResponse(ch);
    s_status |= SR_RDST << StatusShift(ch);
    any = true;
  }
  if (any) {
    s_comcsr |= COMCSR_RDSTINT;
    UpdateInterrupt();
  }
}

u32 Read32(u32 off) {
  if (off >= 0x80 && off < 0x100) return LoadBE32(s_iobuf + (off - 0x80));
  if (off < 0x30) {
    int ch = off / 12;
    switch (off % 12) {
      case 0: return s_ch[ch].out;
      case 4:
        // Reading the high word acknowledges the data.
        s_status &= ~(SR_RDST << StatusShift(ch));
        if (!(s_status & 0x20202020)) {
          s_comcsr &= ~COMCSR_RDSTINT;
          UpdateInterrupt();
        }
        return s_ch[ch].in_hi;
      default: return s_ch[ch].in_lo;
    }
  }
  switch (off) {
    case 0x30: return s_poll;
    case 0x34: return s_comcsr;
    case 0x38: return s_status;
    case 0x3C: return s_exilk;
    default: return 0;
  }
}

void Write32(u32 off, u32 v) {
  if (off >= 0x80 && off < 0x100) {
    StoreBE32(s_iobuf + (off - 0x80), v);
    return;
  }
  if (off < 0x30) {
    if (off % 12 == 0) s_ch[off / 12].out = v;
    return;
  }
  switch (off) {
    case 0x30: s_poll = v; break;
    case 0x34:
      if (v & COMCSR_TCINT) s_comcsr &= ~COMCSR_TCINT;  // write 1 to clear
      s_comcsr = (s_comcsr & (COMCSR_TCINT | COMCSR_RDSTINT | COMCSR_COMERR)) |
                 (v & ~(COMCSR_TCINT | COMCSR_RDSTINT | COMCSR_COMERR));
      UpdateInterrupt();
      if (v & COMCSR_TSTART) RunTransfer();
      break;
    case 0x38:
      // Error bits are write-one-to-clear; bit 31 (WR) copies the out buffers.
      s_status &= ~(v & 0x0F0F0F0F);
      break;
    case 0x3C: s_exilk = v; break;
    default: break;
  }
}

}  // namespace SI
