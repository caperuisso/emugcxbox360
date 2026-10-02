// emugcxbox360 - audio renderer of the "Zelda"/DAC DSP microcode family
// SPDX-License-Identifier: GPL-2.0-or-later
// Port of Dolphin's ZeldaAudioRenderer (GPL-2.0-or-later).
#pragma once

#include "core/common.h"

class ZeldaAudioRenderer {
 public:
  enum Flags : u32 {
    MAKE_DOLBY_LOUDER = 0x2,
    FOUR_MIXING_DESTS = 0x8,
    TINY_VPB = 0x10,
    VOLUME_EXPLICIT_STEP = 0x20,
  };
  static constexpr int N = 0x50;  // samples per mixing buffer
  using Buffer = s16[N];

  void SetFlags(u32 flags) { m_flags = flags; }
  // Command 01: VPB base, coefficient tables, AFC table, reverb parameter blocks
  void Setup(u32 vpb_base, u32 coeffs_addr, u32 afc_coeffs_addr, u32 reverb_base, bool load_sine);
  void SetOutput(u16 volume, u32 left, u32 right) {
    m_output_volume = volume;
    m_output_lbuf = left;
    m_output_rbuf = right;
  }

  void PrepareFrame();
  void AddVoice(u16 voice_id);
  void FinalizeFrame();

 private:
  struct VPB;
  s16* BufferForID(u16 id);
  void ApplyReverb(bool post_rendering);
  void LoadInputSamples(s16* buffer, VPB& vpb);
  u16 NeededRawSamplesCount(const VPB& vpb);
  void Resample(VPB& vpb, const s16* src, s16* dst);
  template <typename T>
  void DownloadPCMFromARAM(s16* dst, VPB& vpb, u16 count);
  void DownloadAFCFromARAM(s16* dst, VPB& vpb, u16 count);
  void DecodeAFC(VPB& vpb, s16* dst, u32 block_count);
  void DownloadRawFromMRAM(s16* dst, VPB& vpb, u16 count);

  u32 m_flags = 0;
  bool m_prepared = false;
  u32 m_output_lbuf = 0, m_output_rbuf = 0;
  u16 m_output_volume = 0;

  Buffer m_front_left = {}, m_front_right = {}, m_back_left = {}, m_back_right = {};
  Buffer m_front_left_reverb = {}, m_front_right_reverb = {}, m_back_left_reverb = {}, m_back_right_reverb = {};
  Buffer m_unk0_reverb = {}, m_unk1_reverb = {}, m_unk0 = {}, m_unk1 = {}, m_unk2 = {};
  s16 m_unk0_reverb_last8[8] = {}, m_unk1_reverb_last8[8] = {};
  s16 m_front_left_reverb_last8[8] = {}, m_front_right_reverb_last8[8] = {};

  s16 m_resampling_coeffs[0x100] = {};
  s16 m_const_patterns[0x100] = {};
  s16 m_sine_table[0x80] = {};
  s16 m_afc_coeffs[0x20] = {};
  u32 m_vpb_base = 0, m_reverb_pb_base = 0;
  u16 m_reverb_pb_frames_count[4] = {};
};
