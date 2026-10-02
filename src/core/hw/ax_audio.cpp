// SPDX-License-Identifier: GPL-2.0-or-later
// Port of Dolphin's AX HLE mixing (GameCube variant).
#include "core/hw/ax_audio.h"

#include <algorithm>
#include <cstdlib>

#include "core/hw/hw.h"
#include "core/memory.h"

namespace {

inline s16 ClampS16(s64 v) { return (s16)std::clamp<s64>(v, -0x8000, 0x7FFF); }
inline u32 HiLo(const u16* w, u32 i) { return ((u32)w[i] << 16) | w[i + 1]; }

// ---- Guest memory helpers (big-endian) ----
void ReadWords(u32 addr, u16* dst, u32 count) {
  const u8* p = Mem::PhysPtr(addr & 0x01FFFFFF, count * 2);
  for (u32 i = 0; i < count; i++) dst[i] = p ? LoadBE16(p + i * 2) : 0;
}
void WriteWords(u32 addr, const u16* src, u32 count) {
  u8* p = Mem::PhysPtr(addr & 0x01FFFFFF, count * 2);
  if (!p) return;
  for (u32 i = 0; i < count; i++) StoreBE16(p + i * 2, src[i]);
}
void ReadInts(u32 addr, int* dst, u32 count) {
  const u8* p = Mem::PhysPtr(addr & 0x01FFFFFF, count * 4);
  for (u32 i = 0; i < count; i++) dst[i] = p ? (int)LoadBE32(p + i * 4) : 0;
}
void WriteInts(u32 addr, const int* src, u32 count) {
  u8* p = Mem::PhysPtr(addr & 0x01FFFFFF, count * 4);
  if (!p) return;
  for (u32 i = 0; i < count; i++) StoreBE32(p + i * 4, (u32)src[i]);
}

// ---- AX parameter block (GameCube layout, 122 words) ----
enum PB : u32 {
  PB_NEXT = 0,
  PB_SRC_TYPE = 4,
  PB_COEF_SELECT = 5,
  PB_MIXER_CONTROL = 6,
  PB_RUNNING = 7,
  PB_IS_STREAM = 8,
  PB_MIXER = 9,  // 9 x {volume, delta}
  PB_UPDATES_NUM = 34,
  PB_UPDATES_DATA = 39,
  PB_DPOP = 41,
  PB_VOL = 50,
  PB_VOL_DELTA = 51,
  PB_LOOPING = 55,
  PB_FORMAT = 56,
  PB_LOOP_ADDR = 57,
  PB_END_ADDR = 59,
  PB_CUR_ADDR = 61,
  PB_COEFS = 63,
  PB_GAIN = 79,
  PB_PRED_SCALE = 80,
  PB_YN1 = 81,
  PB_YN2 = 82,
  PB_RATIO = 83,
  PB_CUR_FRAC = 85,
  PB_LAST_SAMPLES = 86,
  PB_LOOP_PRED_SCALE = 90,
  PB_LOOP_YN1 = 91,
  PB_LOOP_YN2 = 92,
  PB_LPF = 93,  // on, yn1, a0, b0
  PB_LOOP_COUNTER = 97,
  PB_WORDS = 122,
};

enum : u32 {
  MIX_MAIN_L = 0x000001, MIX_MAIN_L_RAMP = 0x000002, MIX_MAIN_R = 0x000004, MIX_MAIN_R_RAMP = 0x000008,
  MIX_MAIN_S = 0x000010, MIX_MAIN_S_RAMP = 0x000020, MIX_AUXA_L = 0x000040, MIX_AUXA_L_RAMP = 0x000080,
  MIX_AUXA_R = 0x000100, MIX_AUXA_R_RAMP = 0x000200, MIX_AUXA_S = 0x000400, MIX_AUXA_S_RAMP = 0x000800,
  MIX_AUXB_L = 0x001000, MIX_AUXB_L_RAMP = 0x002000, MIX_AUXB_R = 0x004000, MIX_AUXB_R_RAMP = 0x008000,
  MIX_AUXB_S = 0x010000, MIX_AUXB_S_RAMP = 0x020000, MIX_ALL_RAMPS = 0x02AAAA,
};

// ---- DSP accelerator: streams ADPCM / PCM samples from ARAM ----
struct Accelerator {
  u16* pb;
  u32 start, end, current;
  u16 format;
  s16 yn1, yn2, gain;
  u16 pred_scale;
  bool reads_stopped = false;

  u8 ReadARAM(u32 addr) const { return DSP::ARAMPtr()[addr % DSP::ARAMSize()]; }

  void SetYn2(s16 v) {
    yn2 = v;
    reads_stopped = false;
  }

  u16 CurrentSample() const {
    switch (format & 3) {
      case 0: {  // 4-bit
        u16 v = ReadARAM(current >> 1);
        return (current & 1) ? (v & 0xF) : (v >> 4);
      }
      case 1: return ReadARAM(current);
      case 2: return (u16)((ReadARAM(current * 2) << 8) | ReadARAM(current * 2 + 1));
      default: return 0;
    }
  }

  void OnEnd() {
    if (pb[PB_LOOPING]) {
      pred_scale = pb[PB_LOOP_PRED_SCALE] & 0x7F;
      if (pb[PB_IS_STREAM] != 1) {
        yn1 = (s16)pb[PB_LOOP_YN1];
        SetYn2((s16)pb[PB_LOOP_YN2]);
      } else {
        SetYn2(yn2);
        pb[PB_LOOP_COUNTER]++;
      }
    } else {
      pb[PB_RUNNING] = 0;
    }
  }

  s16 ReadSample() {
    if (reads_stopped) return 0;
    const s16* coefs = (const s16*)&pb[PB_COEFS];
    u32 decode = (format >> 2) & 3, gain_scale = (format >> 4) & 3;
    s16 raw = (s16)CurrentSample();
    int coef_idx = (pred_scale >> 4) & 7;
    s32 coef1 = coefs[coef_idx * 2], coef2 = coefs[coef_idx * 2 + 1];
    s16 val = 0;
    u32 step = 0;
    if (decode == 0) {  // ADPCM
      raw &= 0xF;
      int scale = 1 << (pred_scale & 0xF);
      if (raw >= 8) raw -= 16;
      s32 v = scale * raw + ((0x400 + coef1 * yn1 + coef2 * yn2) >> 11);
      val = ClampS16(v);
      step = 2;
      yn2 = yn1;
      yn1 = val;
      current += 1;
      if ((end & 0xF) == 0x0 && current == end) {
        current = start + 1;
      } else if ((end & 0xF) == 0x1 && current == end - 1) {
        current = start;
      } else if ((current & 15) == 0) {
        pred_scale = ReadARAM((current & ~15u) >> 1) & 0x7F;
        current += 2;
        step += 2;
      }
    } else {  // PCM
      u32 shift = gain_scale == 0 ? 11 : (gain_scale == 2 ? 16 : 0);
      s32 v = ((s32)gain * raw >> shift) + ((coef1 * yn1) >> shift) + ((coef2 * yn2) >> shift);
      val = (s16)v;
      yn2 = yn1;
      yn1 = val;
      step = 2;
      if (decode != 1) current += 1;
    }
    if (current == end + step - 1) {
      current = start;
      reads_stopped = true;
      OnEnd();
    }
    current &= 0xBFFFFFFF;
    return val;
  }
};

// Linear resampler (the polyphase filter needs coefficients from the DSP ROM,
// which is not shipped; Dolphin falls back to the same linear path).
u32 Resample(Accelerator& acc, s16* out, u32 count, s16* last, u32 pos, u32 ratio, u32 src_type) {
  if (src_type == 2) {  // nearest: no resampling
    for (u32 i = 0; i < count; ++i) out[i] = acc.ReadSample();
    memcpy(last, out + count - 4, 4 * sizeof(s16));
    return pos;
  }
  s16 temp[4];
  u32 idx = 0;
  for (int k = 0; k < 4; k++) temp[idx++ & 3] = last[k];
  for (u32 i = 0; i < count; ++i) {
    pos += ratio;
    while (pos >= 0x10000) {
      temp[idx++ & 3] = acc.ReadSample();
      pos -= 0x10000;
    }
    u16 frac = pos & 0xFFFF;
    u16 inv = (u16)-frac;
    if (frac) {
      s32 s0 = temp[idx++ & 3], s1 = temp[idx++ & 3];
      out[i] = (s16)(((s0 * inv) + (s1 * frac)) >> 16);
      idx += 2;
    } else {
      out[i] = temp[idx++ & 3];
      idx += 3;
    }
  }
  last[3] = temp[--idx & 3];
  last[2] = temp[--idx & 3];
  last[1] = temp[--idx & 3];
  last[0] = temp[--idx & 3];
  return pos;
}

void MixAdd(int* out, const s16* in, u32 count, u16* vol, s16* dpop, bool ramp) {
  u16& volume = vol[0];
  u16 delta = ramp ? vol[1] : 0;
  for (u32 i = 0; i < count; ++i) {
    s64 s = (s64)in[i] * volume >> 15;
    s16 s16v = ClampS16(s);
    out[i] += s16v;
    volume = (u16)(volume + delta);
    *dpop = s16v;
  }
}

void ProcessVoice(u16* pb, int* const* buffers, u32 count, u32 mctrl) {
  if (pb[PB_RUNNING] != 1) return;
  Accelerator acc;
  acc.pb = pb;
  acc.start = HiLo(pb, PB_LOOP_ADDR) & 0x3FFFFFFF;
  acc.end = HiLo(pb, PB_END_ADDR) & 0x3FFFFFFF;
  acc.current = HiLo(pb, PB_CUR_ADDR) & 0xBFFFFFFF;
  acc.format = pb[PB_FORMAT];
  acc.yn1 = (s16)pb[PB_YN1];
  acc.SetYn2((s16)pb[PB_YN2]);
  acc.gain = (s16)pb[PB_GAIN];
  acc.pred_scale = pb[PB_PRED_SCALE] & 0x7F;

  s16 samples[32];
  u32 pos = Resample(acc, samples, count, (s16*)&pb[PB_LAST_SAMPLES], pb[PB_CUR_FRAC], HiLo(pb, PB_RATIO),
                     pb[PB_SRC_TYPE]);
  pb[PB_CUR_FRAC] = (u16)(pos & 0xFFFF);
  pb[PB_CUR_ADDR] = (u16)(acc.current >> 16);
  pb[PB_CUR_ADDR + 1] = (u16)acc.current;
  pb[PB_YN1] = (u16)acc.yn1;
  pb[PB_YN2] = (u16)acc.yn2;
  pb[PB_PRED_SCALE] = acc.pred_scale;

  // Volume envelope
  for (u32 i = 0; i < count; ++i) {
    s32 volume = (s16)pb[PB_VOL];
    samples[i] = ClampS16(((s32)samples[i] * volume) >> 15);
    pb[PB_VOL] = (u16)(pb[PB_VOL] + pb[PB_VOL_DELTA]);
  }
  // Low pass filter
  if (pb[PB_LPF]) {
    s16 yn1 = (s16)pb[PB_LPF + 1];
    s32 a0 = pb[PB_LPF + 2], b0 = (s16)pb[PB_LPF + 3];
    for (u32 i = 0; i < count; ++i) yn1 = samples[i] = ClampS16((a0 * (s32)samples[i] + b0 * (s32)yn1) >> 15);
    pb[PB_LPF + 1] = (u16)yn1;
  }

  // buffers: main L, R, S, auxA L, R, S, auxB L, R, S
  struct Route {
    u32 mix, ramp, buffer, vol_word, dpop_word;
  } routes[9] = {
      {MIX_MAIN_L, MIX_MAIN_L_RAMP, 0, PB_MIXER + 0, PB_DPOP + 0},
      {MIX_MAIN_R, MIX_MAIN_R_RAMP, 1, PB_MIXER + 2, PB_DPOP + 3},
      {MIX_MAIN_S, MIX_MAIN_S_RAMP, 2, PB_MIXER + 14, PB_DPOP + 6},
      {MIX_AUXA_L, MIX_AUXA_L_RAMP, 3, PB_MIXER + 4, PB_DPOP + 1},
      {MIX_AUXA_R, MIX_AUXA_R_RAMP, 4, PB_MIXER + 6, PB_DPOP + 4},
      {MIX_AUXA_S, MIX_AUXA_S_RAMP, 5, PB_MIXER + 16, PB_DPOP + 7},
      {MIX_AUXB_L, MIX_AUXB_L_RAMP, 6, PB_MIXER + 8, PB_DPOP + 2},
      {MIX_AUXB_R, MIX_AUXB_R_RAMP, 7, PB_MIXER + 10, PB_DPOP + 5},
      {MIX_AUXB_S, MIX_AUXB_S_RAMP, 8, PB_MIXER + 12, PB_DPOP + 8},
  };
  for (const Route& r : routes)
    if (mctrl & r.mix)
      MixAdd(buffers[r.buffer], samples, count, &pb[r.vol_word], (s16*)&pb[r.dpop_word], (mctrl & r.ramp) != 0);
}

}  // namespace

u32 AXMixer::ConvertMixerControl(u32 mc) const {
  u32 ret = 0;
  if (m_crc == 0x4E8A8B21) {
    if (mc & 0x0010) {  // Dolby Pro Logic II mixing
      ret |= MIX_MAIN_L | MIX_MAIN_R;
      if ((mc & 0x0006) == 0) ret |= MIX_AUXB_L | MIX_AUXB_R;
      if ((mc & 0x0007) == 1) ret |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_S;
    } else {
      ret |= MIX_MAIN_L | MIX_MAIN_R;
      if (mc & 0x0001) ret |= MIX_AUXA_L | MIX_AUXA_R;
      if (mc & 0x0002) ret |= MIX_AUXB_L | MIX_AUXB_R;
      if (mc & 0x0004) {
        ret |= MIX_MAIN_S;
        if (ret & MIX_AUXA_L) ret |= MIX_AUXA_S;
        if (ret & MIX_AUXB_L) ret |= MIX_AUXB_S;
      }
    }
    if (mc & 0x0008) ret |= MIX_ALL_RAMPS;
  } else {
    if (mc & 0x0001) ret |= MIX_MAIN_L;
    if (mc & 0x0002) ret |= MIX_MAIN_R;
    if (mc & 0x0004) ret |= MIX_MAIN_S;
    if (mc & 0x0008) ret |= MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP | MIX_MAIN_S_RAMP;
    if (mc & 0x0010) ret |= MIX_AUXA_L;
    if (mc & 0x0020) ret |= MIX_AUXA_R;
    if (mc & 0x0040) ret |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
    if (mc & 0x0080) ret |= MIX_AUXA_S;
    if (mc & 0x0100) ret |= MIX_AUXA_S_RAMP;
    if (mc & 0x0200) ret |= MIX_AUXB_L;
    if (mc & 0x0400) ret |= MIX_AUXB_R;
    if (mc & 0x0800) ret |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
    if (mc & 0x1000) ret |= MIX_AUXB_S;
    if (mc & 0x2000) ret |= MIX_AUXB_S_RAMP;
  }
  return ret;
}

void AXMixer::SetupProcessing(u32 init_addr) {
  int* buffers[9] = {m_main_left, m_main_right, m_main_surround, m_auxa_left, m_auxa_right,
                     m_auxa_surround, m_auxb_left, m_auxb_right, m_auxb_surround};
  u16 init[27];
  ReadWords(init_addr, init, 27);
  for (int i = 0; i < 9; ++i) {
    s32 value = (s32)(((u32)init[3 * i] << 16) | init[3 * i + 1]);
    s16 delta = (s16)init[3 * i + 2];
    for (int j = 0; j < SAMPLES; ++j) buffers[i][j] = value ? value + j * delta : 0;
  }
}

void AXMixer::DownloadAndMixWithVolume(u32 addr, u16 vol_main, u16 vol_auxa, u16 vol_auxb) {
  int* groups[3][3] = {{m_main_left, m_main_right, m_main_surround},
                       {m_auxa_left, m_auxa_right, m_auxa_surround},
                       {m_auxb_left, m_auxb_right, m_auxb_surround}};
  u16 volumes[3] = {vol_main, vol_auxa, vol_auxb};
  static int src[3 * SAMPLES];
  for (int i = 0; i < 3; ++i) {
    ReadInts(addr, src, 3 * SAMPLES);  // the same source data feeds all three groups
    int* p = src;
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < SAMPLES; ++k) groups[i][j][k] += (s32)(((s64)*p++ * volumes[i]) >> 15);
  }
}

void AXMixer::ProcessPBList(u32 pb_addr) {
  constexpr u32 spms = 32;
  u16 pb[PB_WORDS];
  for (int guard = 0; pb_addr && guard < 256; guard++) {
    // Some microcode versions store the PB without its low pass filter words.
    if (HasLPF()) {
      ReadWords(pb_addr, pb, PB_WORDS);
    } else {
      ReadWords(pb_addr, pb, PB_LPF);
      memset(&pb[PB_LPF], 0, 4 * sizeof(u16));
      ReadWords(pb_addr + PB_LPF * 2, &pb[PB_LOOP_COUNTER], PB_WORDS - PB_LOOP_COUNTER);
    }
    // Per-millisecond PB updates
    u16 updates[64];
    ReadWords(HiLo(pb, PB_UPDATES_DATA), updates, 64);
    u16 num_updates[5];
    for (int i = 0; i < 5; i++) num_updates[i] = pb[PB_UPDATES_NUM + i];

    int* buffers[9] = {m_main_left, m_main_right, m_main_surround, m_auxa_left, m_auxa_right,
                       m_auxa_surround, m_auxb_left, m_auxb_right, m_auxb_surround};
    for (int ms = 0; ms < 5; ++ms) {
      u32 start = 0;
      for (int i = 0; i < ms; ++i) start += num_updates[i];
      if (start < 32 && num_updates[ms] <= 32 - start) {
        for (u32 i = start; i < start + num_updates[ms]; ++i) {
          u16 off = updates[i * 2], val = updates[i * 2 + 1];
          if (off < PB_WORDS) pb[off] = val;
        }
      }
      ProcessVoice(pb, buffers, spms, ConvertMixerControl(pb[PB_MIXER_CONTROL]));
      for (int*& b : buffers) b += spms;
    }

    if (HasLPF()) {
      WriteWords(pb_addr, pb, PB_WORDS);
    } else {
      WriteWords(pb_addr, pb, PB_LPF);
      WriteWords(pb_addr + PB_LPF * 2, &pb[PB_LOOP_COUNTER], PB_WORDS - PB_LOOP_COUNTER);
    }
    pb_addr = HiLo(pb, PB_NEXT);
  }
}

void AXMixer::MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr) {
  int* buffers[3] = {aux_id == 0 ? m_auxa_left : m_auxb_left, aux_id == 0 ? m_auxa_right : m_auxb_right,
                     aux_id == 0 ? m_auxa_surround : m_auxb_surround};
  if (write_addr) {
    for (int* b : buffers) {
      WriteInts(write_addr, b, SAMPLES);
      write_addr += SAMPLES * 4;
    }
  }
  static int in[3 * SAMPLES];
  ReadInts(read_addr, in, 3 * SAMPLES);
  for (int i = 0; i < SAMPLES; i++) m_main_left[i] += in[i];
  for (int i = 0; i < SAMPLES; i++) m_main_right[i] += in[SAMPLES + i];
  for (int i = 0; i < SAMPLES; i++) m_main_surround[i] += in[2 * SAMPLES + i];
}

void AXMixer::UploadLRS(u32 dst_addr) {
  for (const int* b : {m_main_left, m_main_right, m_main_surround}) {
    WriteInts(dst_addr, b, SAMPLES);
    dst_addr += SAMPLES * 4;
  }
}

void AXMixer::SetMainLR(u32 src_addr) {
  static int in[SAMPLES];
  ReadInts(src_addr, in, SAMPLES);
  for (int i = 0; i < SAMPLES; ++i) {
    m_main_left[i] = in[i];
    m_main_right[i] = in[i];
    m_main_surround[i] = 0;
  }
}

void AXMixer::RunCompressor(u16 threshold, u16 release_frames, u32 table_addr, u32 millis) {
  bool triggered = false;
  for (u32 i = 0; i < 32 * millis; ++i) {
    if (std::abs(m_main_left[i]) > (int)threshold || std::abs(m_main_right[i]) > (int)threshold) {
      triggered = true;
      break;
    }
  }
  const u32 frame_bytes = 32 * millis * 2;
  u32 table_offset;
  if (triggered) {
    table_offset = m_compressor_pos * frame_bytes;
    m_compressor_pos = release_frames;
  } else if (m_compressor_pos) {
    --m_compressor_pos;
    table_offset = (11 + m_compressor_pos) * frame_bytes;
  } else {
    return;
  }
  static u16 ramp[32 * 5];
  ReadWords(table_addr + table_offset, ramp, 32 * millis);
  for (u32 i = 0; i < 32 * millis; ++i) {
    m_main_left[i] = (int)(((s64)m_main_left[i] * ramp[i]) >> 15);
    m_main_right[i] = (int)(((s64)m_main_right[i] * ramp[i]) >> 15);
  }
}

void AXMixer::OutputSamples(u32 lr_addr, u32 surround_addr) {
  WriteInts(surround_addr, m_main_surround, SAMPLES);
  u16 out[SAMPLES * 2];
  for (int i = 0; i < SAMPLES; ++i) {
    out[2 * i + 0] = (u16)ClampS16(m_main_right[i]);  // the AI expects right then left
    out[2 * i + 1] = (u16)ClampS16(m_main_left[i]);
  }
  WriteWords(lr_addr, out, SAMPLES * 2);
}

void AXMixer::MixAUXBLR(u32 ul_addr, u32 dl_addr) {
  WriteInts(ul_addr, m_auxb_left, SAMPLES);
  WriteInts(ul_addr + SAMPLES * 4, m_auxb_right, SAMPLES);
  static int in[2 * SAMPLES];
  ReadInts(dl_addr, in, 2 * SAMPLES);
  for (int i = 0; i < SAMPLES; ++i) {
    m_auxb_left[i] = in[i];
    m_main_left[i] += in[i];
    m_auxb_right[i] = in[SAMPLES + i];
    m_main_right[i] += in[SAMPLES + i];
  }
}

void AXMixer::SetOppositeLR(u32 src_addr) {
  static int in[SAMPLES];
  ReadInts(src_addr, in, SAMPLES);
  for (int i = 0; i < SAMPLES; ++i) {
    m_main_left[i] = -in[i];
    m_main_right[i] = in[i];
    m_main_surround[i] = 0;
  }
}

void AXMixer::SendAUXAndMix(u32 auxa_lrs_up, u32 auxb_s_up, u32 main_l_dl, u32 main_r_dl, u32 auxb_l_dl,
                            u32 auxb_r_dl) {
  WriteInts(auxa_lrs_up, m_auxa_left, SAMPLES);
  WriteInts(auxa_lrs_up + SAMPLES * 4, m_auxa_right, SAMPLES);
  WriteInts(auxa_lrs_up + 2 * SAMPLES * 4, m_auxa_surround, SAMPLES);
  WriteInts(auxb_s_up, m_auxb_surround, SAMPLES);
  int* dst[4] = {m_main_left, m_main_right, m_auxb_left, m_auxb_right};
  u32 src[4] = {main_l_dl, main_r_dl, auxb_l_dl, auxb_r_dl};
  static int in[SAMPLES];
  for (int i = 0; i < 4; ++i) {
    ReadInts(src[i], in, SAMPLES);
    for (int j = 0; j < SAMPLES; ++j) dst[i][j] += in[j];
  }
}
