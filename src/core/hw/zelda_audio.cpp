// SPDX-License-Identifier: GPL-2.0-or-later
// Port of Dolphin's ZeldaAudioRenderer (Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp).
#include "core/hw/zelda_audio.h"

#include <algorithm>

#include "core/hw/hw.h"
#include "core/memory.h"

namespace {

template <typename T>
inline T Clamp16(T v) {
  return v < -0x8000 ? (T)-0x8000 : (v > 0x7FFF ? (T)0x7FFF : v);
}

// Big-endian u16 array transfers between guest RAM and host buffers
void ReadWords(u32 addr, void* dst, u32 count) {
  u16* d = (u16*)dst;
  const u8* p = Mem::PhysPtr(addr & 0x01FFFFFF, count * 2);
  for (u32 i = 0; i < count; i++) d[i] = p ? LoadBE16(p + i * 2) : 0;
}
void WriteWords(u32 addr, const void* src, u32 count) {
  const u16* s = (const u16*)src;
  u8* p = Mem::PhysPtr(addr & 0x01FFFFFF, count * 2);
  if (!p) return;
  for (u32 i = 0; i < count; i++) StoreBE16(p + i * 2, s[i]);
  Mem::MarkWritten(addr & 0x01FFFFFF, count * 2);
}

template <size_t B>
void ApplyVolumeInPlace(s16* buf, u16 vol) {
  for (int i = 0; i < ZeldaAudioRenderer::N; i++) {
    s32 tmp = (s32)((u32)buf[i] * (u32)vol);
    tmp >>= 16 - B;
    buf[i] = (s16)Clamp16(tmp);
  }
}

s32 AddBuffersWithVolumeRamp(s16* dst, const s16* src, s32 vol, s32 step) {
  if (!vol && !step) return vol;
  for (int i = 0; i < ZeldaAudioRenderer::N; i++) {
    dst[i] += (s16)(((vol >> 16) * src[i]) >> 16);
    vol += step;
  }
  return vol;
}

void AddBuffersWithVolume(s16* dst, const s16* src, size_t count, u16 vol) {
  while (count--) {
    s32 v = ((s32)*src++ * (s32)vol) >> 15;
    *dst++ += (s16)Clamp16(v);
  }
}

}  // namespace

// Voice parameter block, accessed as 0xC0 big-endian words (layout from Dolphin).
struct ZeldaAudioRenderer::VPB {
  u16 w[0xC0];
  u16& operator[](u32 i) { return w[i]; }
  u16 enabled() const { return w[0x00]; }
  u16& done() { return w[0x01]; }
  u16 ratio() const { return w[0x02]; }
  u16& reset() { return w[0x04]; }
  u16& end_reached() { return w[0x05]; }
  u16 use_constant() const { return w[0x06]; }
  u16& chan_id(int i) { return w[0x08 + i * 4]; }
  s16 chan_target(int i) const { return (s16)w[0x09 + i * 4]; }
  void set_chan_target(int i, s16 v) { w[0x09 + i * 4] = (u16)v; }
  s16 chan_current(int i) const { return (s16)w[0x0A + i * 4]; }
  void set_chan_current(int i, s16 v) { w[0x0A + i * 4] = (u16)v; }
  u16 dolby_pos() const { return w[0x28]; }
  s16 dolby_reverb() const { return (s16)w[0x29]; }
  s16 dolby_current() const { return (s16)w[0x2A]; }
  s16 dolby_target() const { return (s16)w[0x2B]; }
  u16 use_dolby() const { return w[0x2C]; }
  u16& pos_frac() { return w[0x30]; }
  u16& afc_remaining() { return w[0x32]; }
  s16* constant_sample() { return (s16*)&w[0x33]; }
  u32 Get32(u32 i) const { return ((u32)w[i] << 16) | w[i + 1]; }
  void Set32(u32 i, u32 v) {
    w[i] = (u16)(v >> 16);
    w[i + 1] = (u16)v;
  }
  u32 current_position() const { return Get32(0x34); }
  void set_current_position(u32 v) { Set32(0x34, v); }
  u16& current_position_h() { return w[0x34]; }
  u16& samples_before_loop() { return w[0x36]; }
  u32 aram_addr() const { return Get32(0x38); }
  void set_aram_addr(u32 v) { Set32(0x38, v); }
  u32 remaining() const { return Get32(0x3A); }
  void set_remaining(u32 v) { Set32(0x3A, v); }
  s16* resample_buffer() { return (s16*)&w[0x3C]; }
  s16* afc_samples() { return (s16*)&w[0x58]; }
  s16* afc_yn2() { return (s16*)&w[0x66]; }
  s16* afc_yn1() { return (s16*)&w[0x67]; }
  u16 source_type() const { return w[0x80]; }
  u16 is_looping() const { return w[0x81]; }
  s16 loop_yn1() const { return (s16)w[0x82]; }
  s16 loop_yn2() const { return (s16)w[0x83]; }
  bool biquad_enabled() const { return (w[0x84] >> 5) & 1; }
  u16 end_requested() const { return w[0x85]; }
  u32 loop_address() const { return Get32(0x88); }
  u32 loop_start() const { return Get32(0x8A); }
  u16 loop_start_h() const { return w[0x8A]; }
  u32 base_address() const { return Get32(0x8C); }
  void set_base_address(u32 v) { Set32(0x8C, v); }
  u16 low_pass_coeff() const { return w[0xA8]; }

  void Uncompress() {
    for (int i = 0; i < 0x40; ++i) {
      w[0x80 + i] = w[0x40 + i];
      w[0x40 + i] = 0;
    }
    for (int i = 0; i < 0x10; ++i) {
      w[0x58 + i] = w[0x30 + i];
      w[0x30 + i] = 0;
    }
    for (int i = 0; i < 0x18; ++i) {
      w[0x30 + i] = w[0x18 + i];
      w[0x18 + i] = 0;
    }
  }
  void Compress() {
    for (int i = 0; i < 0x18; ++i) {
      w[0x18 + i] = w[0x30 + i];
      w[0x30 + i] = 0;
    }
    for (int i = 0; i < 0x10; ++i) {
      w[0x30 + i] = w[0x58 + i];
      w[0x58 + i] = 0;
    }
    for (int i = 0; i < 0x40; ++i) {
      w[0x40 + i] = w[0x80 + i];
      w[0x80 + i] = 0;
    }
  }
};

enum SourceType : u16 {
  SRC_SQUARE_WAVE = 0,
  SRC_SAW_WAVE = 1,
  SRC_SQUARE_WAVE_25PCT = 3,
  SRC_CONST_PATTERN_1 = 4,
  SRC_AFC_LQ_FROM_ARAM = 5,
  SRC_CONST_PATTERN_0 = 7,
  SRC_PCM8_FROM_ARAM = 8,
  SRC_AFC_HQ_FROM_ARAM = 9,
  SRC_CONST_PATTERN_0_VARIABLE_STEP = 10,
  SRC_CONST_PATTERN_2 = 11,
  SRC_CONST_PATTERN_3 = 12,
  SRC_PCM16_FROM_ARAM = 16,
  SRC_PCM16_FROM_MRAM = 33,
};

void ZeldaAudioRenderer::Setup(u32 vpb_base, u32 coeffs_addr, u32 afc_coeffs_addr, u32 reverb_base,
                               bool load_sine) {
  m_vpb_base = vpb_base;
  ReadWords(coeffs_addr, m_resampling_coeffs, 0x100);
  ReadWords(coeffs_addr + 0x200, m_const_patterns, 0x100);
  if (load_sine) ReadWords(coeffs_addr + 0x400, m_sine_table, 0x80);
  ReadWords(afc_coeffs_addr, m_afc_coeffs, 0x20);
  m_reverb_pb_base = reverb_base;
}

s16* ZeldaAudioRenderer::BufferForID(u16 id) {
  switch (id) {
    case 0x0D00: return m_front_left;
    case 0x0D60: return m_front_right;
    case 0x0F40: return m_back_left;
    case 0x0CA0: return m_back_right;
    case 0x0E80: return m_front_left_reverb;
    case 0x0EE0: return m_front_right_reverb;
    case 0x0C00: return m_back_left_reverb;
    case 0x0C50: return m_back_right_reverb;
    case 0x0DC0: return m_unk0_reverb;
    case 0x0E20: return m_unk1_reverb;
    case 0x09A0: return m_unk0;
    case 0x0FA0: return m_unk1;
    case 0x0B00: return m_unk2;
    default: return nullptr;
  }
}

void ZeldaAudioRenderer::PrepareFrame() {
  if (m_prepared) return;
  memset(m_front_left, 0, sizeof(Buffer));
  memset(m_front_right, 0, sizeof(Buffer));
  ApplyVolumeInPlace<1>(m_back_left, 0x6784);
  ApplyVolumeInPlace<1>(m_back_right, 0x6784);

  ApplyReverb(false);
  AddBuffersWithVolume(m_front_left_reverb, m_back_left_reverb, 0x50, 0x7FFF);
  AddBuffersWithVolume(m_front_right_reverb, m_back_left_reverb, 0x50, 0xB820);
  AddBuffersWithVolume(m_front_left_reverb, m_back_right_reverb + 0x28, 0x28, 0xB820);
  AddBuffersWithVolume(m_front_right_reverb, m_back_right_reverb + 0x28, 0x28, 0x7FFF);
  memset(m_back_left_reverb, 0, sizeof(Buffer));
  memset(m_back_right_reverb, 0, sizeof(Buffer));

  // Constant patterns 2 and 3 are regenerated every frame.
  s16* pattern2 = m_const_patterns + 2 * 0x40;
  s32 yn2 = pattern2[0x40 - 2], yn1 = pattern2[0x40 - 1], v;
  for (int i = 0; i < 0x40; i += 2) {
    v = yn2 * yn1 - (pattern2[i] << 16);
    yn2 = yn1;
    yn1 = pattern2[i];
    pattern2[i] = (s16)(v >> 16);
    v = 2 * (yn2 * yn1 + (pattern2[i + 1] << 16));
    yn2 = yn1;
    yn1 = pattern2[i + 1];
    pattern2[i + 1] = (s16)(v >> 16);
  }
  s16* pattern3 = m_const_patterns + 3 * 0x40;
  yn2 = pattern3[0x40 - 2];
  yn1 = pattern3[0x40 - 1];
  s16 acc = (s16)yn1;
  s16 step = (s16)(pattern3[0] + ((yn1 * yn2 + ((yn2 << 16) + yn1)) >> 16));
  step = (s16)((step & 0x1FF) | 0x2000);
  for (s32 i = 0; i < 0x40; ++i) pattern3[i] = (s16)(acc + (i + 1) * step);
  m_prepared = true;
}

void ZeldaAudioRenderer::ApplyReverb(bool post_rendering) {
  if (!m_reverb_pb_base) return;
  s16* reverb_buffers[4] = {m_unk0_reverb, m_unk1_reverb, m_front_left_reverb, m_front_right_reverb};
  s16* last8[4] = {m_unk0_reverb_last8, m_unk1_reverb_last8, m_front_left_reverb_last8, m_front_right_reverb_last8};
  for (u16 idx = 0; idx < 4; ++idx) {
    // ReverbPB: enabled, size, base h/l, dest[2] {id, volume}, filter_coeffs[8] (16 words)
    u16 rpb[16];
    ReadWords(m_reverb_pb_base + idx * 32, rpb, 16);
    if (!rpb[0]) continue;
    u16 frame = m_reverb_pb_frames_count[idx];
    u32 mram = (((u32)rpb[2] << 16) | rpb[3]) + frame * 0x50 * 2;
    if (!post_rendering) {
      s16 buffer[0x58];
      for (u16 i = 0; i < 8; ++i) buffer[i] = last8[idx][i];
      ReadWords(mram, buffer + 8, 0x50);
      for (u16 i = 0; i < 8; ++i) last8[idx][i] = buffer[0x50 + i];
      auto filter = [&] {
        for (u16 i = 0; i < 0x50; ++i) {
          s32 sample = 0;
          for (u16 j = 0; j < 8; ++j) sample += (s32)buffer[i + j] * (s16)rpb[8 + j];
          sample >>= 15;
          buffer[i] = (s16)Clamp16(sample);
        }
      };
      if (rpb[0] & 1) filter();
      for (int d = 0; d < 2; d++) {
        u16 id = rpb[4 + d * 2], vol = rpb[5 + d * 2];
        if (!id) continue;
        if (s16* dst = BufferForID(id)) AddBuffersWithVolume(dst, buffer, 0x50, vol);
      }
      if (rpb[0] & 2) filter();
      for (u16 i = 0; i < 0x50; ++i) reverb_buffers[idx][i] = buffer[i];
    } else {
      WriteWords(mram, reverb_buffers[idx], 0x50);
      m_reverb_pb_frames_count[idx] = rpb[1] ? (u16)((frame + 1) % rpb[1]) : 0;
    }
  }
}

void ZeldaAudioRenderer::AddVoice(u16 voice_id) {
  VPB vpb;
  memset(&vpb, 0, sizeof(vpb));
  u32 vpb_words = (m_flags & TINY_VPB) ? 0x80 : 0xC0;
  u32 addr = m_vpb_base + voice_id * vpb_words * 2;
  ReadWords(addr, vpb.w, vpb_words);
  if (m_flags & TINY_VPB) vpb.Uncompress();
  if (!vpb.enabled() || vpb.done()) return;

  s16 input[N];
  LoadInputSamples(input, vpb);

  // Low pass filter
  if (vpb.low_pass_coeff() != 0) {
    s32 yn1 = vpb.reset() ? 0 : (s16)vpb[0x68], xn1 = vpb.reset() ? 0 : (s16)vpb[0x69];
    s32 coeff = vpb.low_pass_coeff();
    for (int i = 0; i < N; ++i) {
      s32 xn0 = input[i];
      s64 tmp = ((s64)(xn0 - xn1) * coeff >> 7) + yn1;
      s16 yn0 = (s16)Clamp16(tmp);
      input[i] = yn0;
      yn1 = yn0;
      xn1 = xn0;
    }
    vpb[0x68] = (u16)yn1;
    vpb[0x69] = (u16)xn1;
  }
  // Biquad filter
  s16 bn1 = (s16)vpb[0xA4], bn2 = (s16)vpb[0xA5], an1 = (s16)vpb[0xA6], an2 = (s16)vpb[0xA7];
  if (vpb.biquad_enabled() && (an2 != 0 || an1 != 0 || bn2 != 0 || bn1 != 0x7FFF)) {
    s32 xn1 = (s16)vpb[0x54], xn2 = (s16)vpb[0x55], yn1 = (s16)vpb[0x56], yn2 = (s16)vpb[0x57];
    for (int i = 0; i < N; ++i) {
      s32 xn0 = input[i];
      s64 tmp = (s64)bn1 * xn1 + (s64)bn2 * xn2 + (s64)an1 * yn1 + (s64)an2 * yn2;
      s16 yn0 = (s16)Clamp16(tmp >> 15);
      input[i] = yn0;
      xn2 = xn1;
      xn1 = xn0;
      yn2 = yn1;
      yn1 = yn0;
    }
    vpb[0x54] = (u16)xn1;
    vpb[0x55] = (u16)xn2;
    vpb[0x56] = (u16)yn1;
    vpb[0x57] = (u16)yn2;
  }

  if (vpb.use_dolby()) {
    s16 target = vpb.dolby_target();
    if (vpb.end_requested()) {
      target = (s16)(vpb.dolby_current() / 2);
      vpb[0x2B] = (u16)target;
      if (target == 0) vpb.done() = 1;
    }
    u8 x = (vpb.dolby_pos() >> 8) & 0x7F, y = vpb.dolby_pos() & 0x7F;
    s16 right_volume = m_sine_table[x], back_volume = m_sine_table[y];
    s16 left_volume = m_sine_table[x ^ 0x7F], front_volume = m_sine_table[y ^ 0x7F];
    u16 shift = (m_flags & MAKE_DOLBY_LOUDER) ? 15 : 16;
    s16 quadrant[4] = {(s16)((left_volume * front_volume) >> shift), (s16)((left_volume * back_volume) >> shift),
                       (s16)((right_volume * front_volume) >> shift), (s16)((right_volume * back_volume) >> shift)};
    s16 delta = (s16)(target - vpb.dolby_current());
    s16 deltas[4];
    for (int i = 0; i < 4; ++i) deltas[i] = (s16)(((u16)quadrant[i] * delta) >> shift);
    for (s16& q : quadrant) q = (s16)((q * vpb.dolby_current()) >> shift);
    s16 reverb[4], reverb_deltas[4];
    for (int i = 0; i < 4; ++i) {
      reverb[i] = (s16)((quadrant[i] * vpb.dolby_reverb()) >> shift);
      reverb_deltas[i] = (s16)((deltas[i] * vpb.dolby_reverb()) >> shift);
    }
    struct {
      s16* buf;
      s16 vol, delta;
    } outs[8] = {{m_front_left, quadrant[0], deltas[0]},          {m_back_left, quadrant[1], deltas[1]},
                 {m_front_right, quadrant[2], deltas[2]},         {m_back_right, quadrant[3], deltas[3]},
                 {m_front_left_reverb, reverb[0], reverb_deltas[0]}, {m_back_left_reverb, reverb[1], reverb_deltas[1]},
                 {m_front_right_reverb, reverb[2], reverb_deltas[2]}, {m_back_right_reverb, reverb[3], reverb_deltas[3]}};
    for (auto& o : outs) AddBuffersWithVolumeRamp(o.buf, input, o.vol << 16, (o.delta << 16) / N);
    vpb[0x2A] = (u16)target;
  } else {
    int channels = (m_flags & FOUR_MIXING_DESTS) ? 4 : 6;
    if (vpb.end_requested()) {
      bool all_mute = true;
      for (int i = 0; i < channels; ++i) {
        vpb.set_chan_target(i, (s16)(vpb.chan_current(i) / 2));
        all_mute &= vpb.chan_target(i) == 0;
      }
      if (all_mute) vpb.done() = 1;
    }
    for (int i = 0; i < channels; ++i) {
      if (!vpb.chan_id(i)) continue;
      s16 delta = (m_flags & VOLUME_EXPLICIT_STEP) ? vpb.chan_target(i)
                                                   : (s16)(vpb.chan_target(i) - vpb.chan_current(i));
      s32 step = ((s32)delta << 16) / N;
      if (!vpb.chan_current(i) && !step) continue;
      s16* dst = BufferForID(vpb.chan_id(i));
      if (!dst) continue;
      s32 v = AddBuffersWithVolumeRamp(dst, input, (s32)vpb.chan_current(i) << 16, step);
      vpb.set_chan_current(i, (s16)(v >> 16));
    }
  }
  if (!vpb.use_constant()) vpb.reset() = 0;

  if (m_flags & TINY_VPB) vpb.Compress();
  WriteWords(addr, vpb.w, vpb_words - 0x40);
}

void ZeldaAudioRenderer::FinalizeFrame() {
  ApplyVolumeInPlace<4>(m_front_left, m_output_volume);
  ApplyVolumeInPlace<4>(m_front_right, m_output_volume);
  WriteWords(m_output_lbuf, m_front_left, N);
  WriteWords(m_output_rbuf, m_front_right, N);
  m_output_lbuf += N * 2;
  m_output_rbuf += N * 2;
  ApplyReverb(true);
  m_prepared = false;
}

void ZeldaAudioRenderer::LoadInputSamples(s16* buffer, VPB& vpb) {
  s16 raw[4 + 0x500 + 0x10];
  for (int i = 0; i < 4; ++i) raw[i] = vpb.resample_buffer()[i];
  if (vpb.use_constant()) {
    for (int i = 0; i < N; i++) buffer[i] = *vpb.constant_sample();
    return;
  }
  switch (vpb.source_type()) {
    case SRC_SQUARE_WAVE:
    case SRC_SQUARE_WAVE_25PCT: {
      u32 shift = vpb.source_type() == SRC_SQUARE_WAVE ? 1 : 2;
      u32 mask = (1u << shift) - 1;
      u32 ratio = (u32)vpb.ratio() << (shift - 1);
      u32 pos = (u32)vpb.pos_frac() << shift;
      for (int i = 0; i < N; i++) {
        buffer[i] = ((pos >> 16) & mask) ? (s16)0xC000 : 0x4000;
        pos += ratio;
      }
      vpb.pos_frac() = (u16)((pos >> shift) & 0xFFFF);
      break;
    }
    case SRC_SAW_WAVE: {
      u32 pos = vpb.pos_frac();
      for (int i = 0; i < N; i++) {
        buffer[i] = (s16)(pos & 0xFFFF);
        pos += vpb.ratio() >> 1;
      }
      vpb.pos_frac() = (u16)(pos & 0xFFFF);
      break;
    }
    case SRC_CONST_PATTERN_0:
    case SRC_CONST_PATTERN_0_VARIABLE_STEP:
    case SRC_CONST_PATTERN_1:
    case SRC_CONST_PATTERN_2:
    case SRC_CONST_PATTERN_3: {
      u32 idx = 0;
      bool variable = false;
      switch (vpb.source_type()) {
        case SRC_CONST_PATTERN_0_VARIABLE_STEP: variable = true; break;
        case SRC_CONST_PATTERN_1: idx = 1; break;
        case SRC_CONST_PATTERN_2: idx = 2; break;
        case SRC_CONST_PATTERN_3: idx = 3; break;
        default: break;
      }
      const s16* pattern = m_const_patterns + idx * 0x40;
      u32 pos = (u32)vpb.pos_frac() << 6;
      u32 step = (u32)vpb.ratio() << 5;
      for (int i = 0; i < N; ++i) {
        buffer[i] = pattern[pos >> 16];
        pos = (pos + step) % (0x40u << 16);
        if (variable) pos = (u32)(((s64)pos << 10) + m_back_right[i] * (s32)vpb.ratio()) >> 10;
      }
      vpb.pos_frac() = (u16)(pos >> 6);
      break;
    }
    case SRC_PCM8_FROM_ARAM:
      DownloadPCMFromARAM<s8>(raw + 4, vpb, NeededRawSamplesCount(vpb));
      Resample(vpb, raw, buffer);
      break;
    case SRC_AFC_HQ_FROM_ARAM:
    case SRC_AFC_LQ_FROM_ARAM:
      DownloadAFCFromARAM(raw + 4, vpb, NeededRawSamplesCount(vpb));
      Resample(vpb, raw, buffer);
      break;
    case SRC_PCM16_FROM_ARAM:
      DownloadPCMFromARAM<s16>(raw + 4, vpb, NeededRawSamplesCount(vpb));
      Resample(vpb, raw, buffer);
      break;
    case SRC_PCM16_FROM_MRAM:
      DownloadRawFromMRAM(raw + 4, vpb, NeededRawSamplesCount(vpb));
      Resample(vpb, raw, buffer);
      break;
    default:
      memset(buffer, 0, N * sizeof(s16));
      break;
  }
}

u16 ZeldaAudioRenderer::NeededRawSamplesCount(const VPB& vpb) {
  u32 n = ((u32)vpb.w[0x30] + 0x50u * vpb.ratio()) >> 12;
  return (u16)std::min<u32>(n, 0x500);
}

void ZeldaAudioRenderer::Resample(VPB& vpb, const s16* src, s16* dst) {
  u32 ratio = vpb.ratio();
  u32 pos = vpb.pos_frac();
  if ((ratio >> 12) >= 4) {
    for (int i = 0; i < N; i++) {
      pos += ratio;
      dst[i] = src[std::min<u32>(pos >> 12, 4 + 0x500 + 0xF)];
    }
  } else {
    for (int i = 0; i < N; i++) {
      u32 ci = ((pos & 0xFFF) >> 6) * 4;
      const s16* coeffs = &m_resampling_coeffs[ci];
      const s16* in = &src[pos >> 12];
      s64 v = 0;
      for (int k = 0; k < 4; ++k) v += (s64)2 * coeffs[k] * in[k];
      v >>= 16;
      dst[i] = (s16)Clamp16(v);
      pos += ratio;
    }
  }
  for (int i = 0; i < 4; ++i) vpb.resample_buffer()[i] = src[std::min<u32>((pos >> 12) + i, 4 + 0x500 + 0xF)];
  *vpb.constant_sample() = dst[N - 1];
  vpb.pos_frac() = (u16)(pos & 0xFFF);
}

template <typename T>
void ZeldaAudioRenderer::DownloadPCMFromARAM(s16* dst, VPB& vpb, u16 count) {
  if (vpb.done()) {
    memset(dst, 0, count * sizeof(s16));
    return;
  }
  if (vpb.reset()) {
    vpb.set_remaining(vpb.loop_start() - vpb.current_position());
    vpb.set_aram_addr(vpb.base_address() + vpb.current_position() * (u32)sizeof(T));
  }
  vpb.end_reached() = 0;
  u8* aram = DSP::ARAMPtr();
  u32 aram_size = DSP::ARAMSize();
  while (count) {
    if (vpb.end_reached()) {
      vpb.end_reached() = 0;
      if (!vpb.is_looping()) {
        memset(dst, 0, count * sizeof(s16));
        vpb.done() = 1;
        break;
      }
      vpb.set_current_position(vpb.loop_address());
      vpb.set_remaining(vpb.loop_start() - vpb.current_position());
      vpb.set_aram_addr(vpb.base_address() + vpb.current_position() * (u32)sizeof(T));
    }
    u16 n = (u16)std::min<u32>(vpb.remaining(), count);
    u32 a = vpb.aram_addr();
    for (u16 i = 0; i < n; ++i) {
      u32 off = (a + i * (u32)sizeof(T)) % aram_size;
      if (sizeof(T) == 1)
        *dst++ = (s16)((s8)aram[off] << 8);
      else
        *dst++ = (s16)LoadBE16(aram + (off & ~1u));
    }
    vpb.set_remaining(vpb.remaining() - n);
    vpb.set_aram_addr(a + n * (u32)sizeof(T));
    count -= n;
    if (!vpb.remaining()) vpb.end_reached() = 1;
    if (n == 0 && count) {  // malformed voice: avoid spinning forever
      memset(dst, 0, count * sizeof(s16));
      break;
    }
  }
}

void ZeldaAudioRenderer::DownloadAFCFromARAM(s16* dst, VPB& vpb, u16 count) {
  if (vpb.reset()) {
    *vpb.afc_yn1() = 0;
    *vpb.afc_yn2() = 0;
    vpb.afc_remaining() = 0;
    vpb.set_remaining(vpb.loop_start());
    vpb.set_aram_addr(vpb.base_address());
  }
  if (vpb.done()) {
    memset(dst, 0, count * sizeof(s16));
    return;
  }
  if (vpb.afc_remaining() > 0x10) vpb.afc_remaining() = 0x10;
  for (int guard = 0; guard < 64; guard++) {
    u16 out = std::min<u16>(vpb.afc_remaining(), count);
    s16* base = &vpb.afc_samples()[0x10 - vpb.afc_remaining()];
    for (u16 i = 0; i < out; ++i) *dst++ = base[i];
    vpb.afc_remaining() -= out;
    count -= out;
    if (count == 0) return;
    if (count <= vpb.remaining()) {
      u16 blocks = (u16)((count + 0xF) >> 4);
      u16 decoded = (u16)(blocks << 4);
      if (decoded < vpb.remaining()) {
        vpb.afc_remaining() = (u16)(decoded - count);
        vpb.set_remaining(vpb.remaining() - decoded);
      } else {
        vpb.afc_remaining() = (u16)(vpb.remaining() - count);
        vpb.set_remaining(0);
      }
      DecodeAFC(vpb, dst, blocks);
      if (vpb.afc_remaining()) {
        for (int i = 0; i < 0x10; ++i) vpb.afc_samples()[i] = dst[decoded - 0x10 + i];
        if (!vpb.remaining() && vpb.loop_start()) {
          base = vpb.afc_samples() + ((vpb.loop_start() + 0xF) & 0xF);
          for (u32 i = 0; i < vpb.afc_remaining(); ++i) vpb.afc_samples()[0x10 - i - 1] = *base--;
        }
      }
      return;
    }
    if (vpb.remaining()) {
      count -= (u16)vpb.remaining();
      u16 blocks = (u16)((vpb.remaining() + 0xF) >> 4);
      DecodeAFC(vpb, dst, blocks);
      dst += vpb.remaining();
    }
    if (!vpb.is_looping()) {
      vpb.done() = 1;
      memset(dst, 0, count * sizeof(s16));
      return;
    }
    u32 loop_off = (vpb.loop_address() >> 4) * vpb.source_type();
    vpb.set_aram_addr(vpb.base_address() + loop_off);
    *vpb.afc_yn2() = vpb.loop_yn2();
    *vpb.afc_yn1() = vpb.loop_yn1();
    DecodeAFC(vpb, vpb.afc_samples(), 1);
    vpb.afc_remaining() = (u16)(0x10 - (vpb.loop_address() & 0xF));
    u32 remaining = vpb.loop_start() - vpb.afc_remaining() - vpb.loop_address();
    vpb.set_remaining(remaining);
  }
}

void ZeldaAudioRenderer::DecodeAFC(VPB& vpb, s16* dst, u32 block_count) {
  u32 addr = vpb.aram_addr();
  u32 block_size = vpb.source_type();  // 9 bytes (HQ) or 5 bytes (LQ) per 16 samples
  u8* aram = DSP::ARAMPtr();
  u32 aram_size = DSP::ARAMSize();
  vpb.set_aram_addr(addr + block_count * block_size);
  for (u32 b = 0; b < block_count; ++b) {
    auto byte = [&](u32 i) { return aram[(addr + i) % aram_size]; };
    s16 nibbles[16];
    u8 header = byte(0);
    s16 delta = (s16)(1 << ((header >> 4) & 0xF));
    s16 idx = header & 0xF;
    if (block_size == SRC_AFC_HQ_FROM_ARAM) {
      for (int i = 0; i < 16; i += 2) {
        u8 v = byte(1 + i / 2);
        nibbles[i] = (s16)((s16)((v >> 4) << 12) >> 1);
        nibbles[i + 1] = (s16)((s16)((v & 0xF) << 12) >> 1);
      }
    } else {
      for (int i = 0; i < 16; i += 4) {
        u8 v = byte(1 + i / 4);
        for (int k = 0; k < 4; k++) nibbles[i + k] = (s16)((s16)(((v >> (6 - 2 * k)) & 3) << 14) >> 1);
      }
    }
    addr += block_size;
    s32 yn1 = *vpb.afc_yn1(), yn2 = *vpb.afc_yn2();
    for (s16 nibble : nibbles) {
      s32 sample = delta * nibble + yn1 * m_afc_coeffs[idx * 2] + yn2 * m_afc_coeffs[idx * 2 + 1];
      sample >>= 11;
      sample = Clamp16(sample);
      *dst++ = (s16)sample;
      yn2 = yn1;
      yn1 = sample;
    }
    *vpb.afc_yn2() = (s16)yn2;
    *vpb.afc_yn1() = (s16)yn1;
  }
}

void ZeldaAudioRenderer::DownloadRawFromMRAM(s16* dst, VPB& vpb, u16 count) {
  u32 addr = vpb.base_address() + vpb.current_position_h() * 2u;
  u32 remaining = vpb.remaining();
  if (count > remaining) {
    s16 last = 0;
    if (remaining) {
      ReadWords(addr, dst, remaining);
      last = dst[remaining - 1];
    }
    for (u32 i = remaining; i < count; ++i) dst[i] = last;
    vpb.current_position_h() += (u16)remaining;
    vpb.set_remaining(0);
    vpb.done() = 1;
  } else {
    vpb.set_remaining(remaining - count);
    vpb.samples_before_loop() = (u16)(vpb.loop_start_h() - vpb.current_position_h());
    u16 before = vpb.samples_before_loop();
    if (count <= before) {
      ReadWords(addr, dst, count);
      vpb.current_position_h() += count;
    } else {
      ReadWords(addr, dst, before);
      vpb.set_base_address(vpb.loop_address());
      vpb.current_position_h() = (u16)(count - before);
      ReadWords(vpb.loop_address(), dst + before, vpb.current_position_h());
    }
  }
}
