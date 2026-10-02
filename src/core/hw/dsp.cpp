// SPDX-License-Identifier: GPL-2.0-or-later
// DSP interface registers, ARAM + ARAM DMA, audio DMA, and the Audio Interface.
// The DSP microcodes themselves are high level emulated in dsp_hle.cpp.
#include <cstdlib>
#include <vector>

#include "core/coretiming.h"
#include "core/gekko/block_cache.h"
#include "core/hw/dsp_hle.h"
#include "core/hw/hw.h"
#include "core/memory.h"
#include "core/state.h"
#include "platform/platform.h"

namespace DSP {

namespace {
constexpr u32 ARAM_SIZE = 16 * 1024 * 1024;

enum : u16 {
  CSR_RES = 0x0001,
  CSR_PIINT = 0x0002,
  CSR_HALT = 0x0004,
  CSR_AIDINT = 0x0008,
  CSR_AIDINTMSK = 0x0010,
  CSR_ARINT = 0x0020,
  CSR_ARINTMSK = 0x0040,
  CSR_DSPINT = 0x0080,
  CSR_DSPINTMSK = 0x0100,
  CSR_DMA = 0x0200,
  CSR_INT_BITS = CSR_AIDINT | CSR_ARINT | CSR_DSPINT,
};

u8* s_aram = nullptr;
u16 s_csr;  // bits outside DSPHLE::CONTROL_MASK
u32 s_mail_to_dsp;
u16 s_ar_size, s_ar_mode, s_ar_refresh;
u32 s_ar_mm, s_ar_aram, s_ar_cnt;
u32 s_aid_addr, s_aid_cur_addr;
u16 s_aid_ctrl, s_aid_left;
int s_aid_event = -1;
int s_dsp_int_event = -1;
int s_hle_event = -1;
constexpr s64 HLE_UPDATE_CYCLES = CPU_CLOCK / 1000;
std::vector<s16> s_audio;

void UpdateInterrupt() {
  bool active = ((s_csr & CSR_AIDINT) && (s_csr & CSR_AIDINTMSK)) || ((s_csr & CSR_ARINT) && (s_csr & CSR_ARINTMSK)) ||
                ((s_csr & CSR_DSPINT) && (s_csr & CSR_DSPINTMSK));
  PI::SetInterrupt(PI::INT_DSP, active);
}

void RunARAMDMA() {
  u32 len = s_ar_cnt & 0x7FFFFFE0;
  bool to_mram = s_ar_cnt & 0x80000000;
  u8* mem = Mem::PhysPtr(s_ar_mm, len);
  if (mem) {
    for (u32 k = 0; k < len; k++) {
      u32 a = (s_ar_aram + k) & (ARAM_SIZE - 1);
      if (to_mram)
        mem[k] = s_aram[a];
      else
        s_aram[a] = mem[k];
    }
  }
  if (to_mram) {
    BlockCache::Invalidate(s_ar_mm, len);
    Mem::MarkWritten(s_ar_mm, len);
  }
  s_ar_mm += len;
  s_ar_aram += len;
  s_ar_cnt &= 0x80000000;
  s_csr |= CSR_ARINT;
  UpdateInterrupt();
}

u32 AudioBlockCycles(u32 blocks) {
  // 32 bytes = 8 stereo s16 frames.
  u64 frames = (u64)blocks * 8;
  return (u32)(frames * CPU_CLOCK / AI::GetDSPSampleRate());
}

void AudioDMACallback(u64, s64 late) {
  if (!(s_aid_ctrl & 0x8000)) return;
  u32 blocks = s_aid_ctrl & 0x7FFF;
  if (blocks == 0) return;
  u32 bytes = blocks * 32;
  if (const u8* src = Mem::PhysPtr(s_aid_cur_addr, bytes)) {
    s_audio.resize(bytes / 2);
    // The audio interface receives right then left; hosts expect left first.
    for (u32 k = 0; k < bytes / 2; k += 2) {
      s_audio[k] = (s16)LoadBE16(src + k * 2 + 2);
      s_audio[k + 1] = (s16)LoadBE16(src + k * 2);
    }
    if (g_host) g_host->PushAudio(s_audio.data(), (int)(bytes / 4), (int)AI::GetDSPSampleRate());
  }
  // Block finished: raise AIDINT, hardware restarts from the programmed address.
  s_aid_cur_addr = s_aid_addr;
  s_aid_left = 0;
  s_csr |= CSR_AIDINT;
  UpdateInterrupt();
  CoreTiming::ScheduleEvent(s_aid_event, (s64)AudioBlockCycles(blocks) - late);
}

void DSPInterruptCallback(u64, s64) {
  s_csr |= CSR_DSPINT;
  UpdateInterrupt();
}

void HLEUpdateCallback(u64, s64 late) {
  DSPHLE::Update();
  CoreTiming::ScheduleEvent(s_hle_event, HLE_UPDATE_CYCLES - late);
}
}  // namespace

void DoState(StateBuffer& s) {
  s.Marker("DSP");
  s.DoBytes(s_aram, ARAM_SIZE);
  s.Do(s_csr);
  s.Do(s_mail_to_dsp);
  s.Do(s_ar_size);
  s.Do(s_ar_mode);
  s.Do(s_ar_refresh);
  s.Do(s_ar_mm);
  s.Do(s_ar_aram);
  s.Do(s_ar_cnt);
  s.Do(s_aid_addr);
  s.Do(s_aid_cur_addr);
  s.Do(s_aid_ctrl);
  s.Do(s_aid_left);
  DSPHLE::DoState(s);
}

u8* ARAMPtr() { return s_aram; }
u32 ARAMSize() { return ARAM_SIZE; }

void GenerateDSPInterrupt(s64 cycles_into_future) {
  if (cycles_into_future <= 0) {
    s_csr |= CSR_DSPINT;
    UpdateInterrupt();
  } else {
    CoreTiming::ScheduleEvent(s_dsp_int_event, cycles_into_future);
  }
}

void Init() {
  if (!s_aram) s_aram = (u8*)calloc(1, ARAM_SIZE);
  s_aid_event = CoreTiming::RegisterEvent("Audio DMA", AudioDMACallback);
  s_dsp_int_event = CoreTiming::RegisterEvent("DSP interrupt", DSPInterruptCallback);
  s_hle_event = CoreTiming::RegisterEvent("DSP HLE update", HLEUpdateCallback);
}

void Reset() {
  memset(s_aram, 0, ARAM_SIZE);
  s_csr = 0;
  s_mail_to_dsp = 0;
  DSPHLE::Reset();
  CoreTiming::RemoveEvent(s_dsp_int_event);
  CoreTiming::RemoveEvent(s_hle_event);
  CoreTiming::ScheduleEvent(s_hle_event, HLE_UPDATE_CYCLES);
  s_ar_size = s_ar_mode = s_ar_refresh = 0;
  s_ar_mm = s_ar_aram = s_ar_cnt = 0;
  s_aid_addr = s_aid_cur_addr = 0;
  s_aid_ctrl = s_aid_left = 0;
  CoreTiming::RemoveEvent(s_aid_event);
  UpdateInterrupt();
}

u16 Read16(u32 off) {
  switch (off) {
    case 0x00: return (u16)(s_mail_to_dsp >> 16);  // MSB cleared once the DSP took it
    case 0x02: return (u16)s_mail_to_dsp;
    case 0x04: return DSPHLE::ReadMailHigh();
    case 0x06: return DSPHLE::ReadMailLow();
    case 0x0A: return (s_csr & ~DSPHLE::CONTROL_MASK) | (DSPHLE::ReadControl() & DSPHLE::CONTROL_MASK);
    case 0x12: return s_ar_size;
    case 0x16: return s_ar_mode | 1;  // ARAM ready
    case 0x1A: return s_ar_refresh;
    case 0x20: return (u16)(s_ar_mm >> 16);
    case 0x22: return (u16)s_ar_mm;
    case 0x24: return (u16)(s_ar_aram >> 16);
    case 0x26: return (u16)s_ar_aram;
    case 0x28: return (u16)(s_ar_cnt >> 16);
    case 0x2A: return (u16)s_ar_cnt;
    case 0x30: return (u16)(s_aid_addr >> 16);
    case 0x32: return (u16)s_aid_addr;
    case 0x36: return s_aid_ctrl;
    case 0x3A: return s_aid_left;
    default: return 0;
  }
}

void Write16(u32 off, u16 v) {
  switch (off) {
    case 0x00: s_mail_to_dsp = (s_mail_to_dsp & 0xFFFF) | ((u32)v << 16); break;
    case 0x02:
      s_mail_to_dsp = (s_mail_to_dsp & 0xFFFF0000u) | v;
      DSPHLE::SendMail(s_mail_to_dsp);
      s_mail_to_dsp &= 0x7FFFFFFF;  // the DSP has taken the mail
      break;
    case 0x0A: {
      DSPHLE::WriteControl(v);
      if (v & CSR_RES) {
        s_aid_ctrl = 0;  // reset also stops audio DMA
        CoreTiming::RemoveEvent(s_aid_event);
      }
      // Interrupt status bits are write-one-to-clear; masks are latched.
      u16 cleared = s_csr & CSR_INT_BITS & ~(v & CSR_INT_BITS);
      s_csr = (v & ~(CSR_INT_BITS | DSPHLE::CONTROL_MASK | CSR_DMA)) | cleared;
      UpdateInterrupt();
      break;
    }
    case 0x12: s_ar_size = v; break;
    case 0x16: s_ar_mode = v; break;
    case 0x1A: s_ar_refresh = v; break;
    case 0x20: s_ar_mm = (s_ar_mm & 0xFFFF) | ((u32)(v & 0x03FF) << 16); break;
    case 0x22: s_ar_mm = (s_ar_mm & 0xFFFF0000u) | (v & 0xFFE0); break;
    case 0x24: s_ar_aram = (s_ar_aram & 0xFFFF) | ((u32)(v & 0x03FF) << 16); break;
    case 0x26: s_ar_aram = (s_ar_aram & 0xFFFF0000u) | (v & 0xFFE0); break;
    case 0x28: s_ar_cnt = (s_ar_cnt & 0xFFFF) | ((u32)v << 16); break;
    case 0x2A:
      s_ar_cnt = (s_ar_cnt & 0xFFFF0000u) | (v & 0xFFE0);
      RunARAMDMA();
      break;
    case 0x30: s_aid_addr = (s_aid_addr & 0xFFFF) | ((u32)(v & 0x03FF) << 16); break;
    case 0x32: s_aid_addr = (s_aid_addr & 0xFFFF0000u) | (v & 0xFFE0); break;
    case 0x36: {
      bool was_on = s_aid_ctrl & 0x8000;
      s_aid_ctrl = v;
      if ((v & 0x8000) && !was_on) {
        s_aid_cur_addr = s_aid_addr;
        s_aid_left = v & 0x7FFF;
        CoreTiming::RemoveEvent(s_aid_event);
        CoreTiming::ScheduleEvent(s_aid_event, AudioBlockCycles(v & 0x7FFF));
      } else if (!(v & 0x8000)) {
        CoreTiming::RemoveEvent(s_aid_event);
      }
      break;
    }
    default: break;
  }
}

}  // namespace DSP

namespace AI {

namespace {
enum : u32 {
  CR_PSTAT = 1 << 0,
  CR_AFR = 1 << 1,
  CR_AIINTMSK = 1 << 2,
  CR_AIINT = 1 << 3,
  CR_AIINTVLD = 1 << 4,
  CR_SCRESET = 1 << 5,
  CR_DSR = 1 << 6,
};
u32 s_cr, s_vr, s_scnt, s_it;
u64 s_scnt_start_cycles;
int s_int_event = -1;

u32 StreamRate() { return (s_cr & CR_AFR) ? 48000 : 32000; }

u32 SampleCounter() {
  if (!(s_cr & CR_PSTAT)) return s_scnt;
  u64 elapsed = CoreTiming::GetTicks() - s_scnt_start_cycles;
  return s_scnt + (u32)(elapsed * StreamRate() / CPU_CLOCK);
}

void UpdateInterrupt() { PI::SetInterrupt(PI::INT_AI, (s_cr & CR_AIINT) && (s_cr & CR_AIINTMSK)); }

void ScheduleCounterInterrupt() {
  CoreTiming::RemoveEvent(s_int_event);
  if (!(s_cr & CR_PSTAT) || (s_cr & CR_AIINTVLD)) return;
  u32 now = SampleCounter();
  u32 delta = s_it - now;
  CoreTiming::ScheduleEvent(s_int_event, (s64)((u64)delta * CPU_CLOCK / StreamRate()));
}

void IntCallback(u64, s64) {
  s_cr |= CR_AIINT;
  UpdateInterrupt();
}
}  // namespace

void Init() { s_int_event = CoreTiming::RegisterEvent("AI counter", IntCallback); }

void Reset() {
  s_cr = s_vr = s_scnt = s_it = 0;
  s_scnt_start_cycles = 0;
  CoreTiming::RemoveEvent(s_int_event);
  UpdateInterrupt();
}

void DoState(StateBuffer& s) {
  s.Marker("AI");
  s.Do(s_cr);
  s.Do(s_vr);
  s.Do(s_scnt);
  s.Do(s_it);
  s.Do(s_scnt_start_cycles);
}

u32 GetDSPSampleRate() { return (s_cr & CR_DSR) ? 32000 : 48000; }

u32 Read32(u32 off) {
  switch (off) {
    case 0x00: return s_cr;
    case 0x04: return s_vr;
    case 0x08: return SampleCounter();
    case 0x0C: return s_it;
    default: return 0;
  }
}

void Write32(u32 off, u32 v) {
  switch (off) {
    case 0x00: {
      u32 latched = s_scnt;
      if (s_cr & CR_PSTAT) latched = SampleCounter();
      u32 old = s_cr;
      s_cr = (v & ~CR_AIINT) | (old & CR_AIINT);
      if (v & CR_AIINT) s_cr &= ~CR_AIINT;  // write 1 to clear
      if (v & CR_SCRESET) latched = 0;
      s_cr &= ~CR_SCRESET;
      s_scnt = latched;
      s_scnt_start_cycles = CoreTiming::GetTicks();
      UpdateInterrupt();
      ScheduleCounterInterrupt();
      break;
    }
    case 0x04: s_vr = v; break;
    case 0x08:
      s_scnt = v;
      s_scnt_start_cycles = CoreTiming::GetTicks();
      ScheduleCounterInterrupt();
      break;
    case 0x0C:
      s_it = v;
      ScheduleCounterInterrupt();
      break;
    default: break;
  }
}

}  // namespace AI
