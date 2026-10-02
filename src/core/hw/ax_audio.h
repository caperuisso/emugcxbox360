// emugcxbox360 - audio mixing of the AX DSP microcode (used by most games)
// SPDX-License-Identifier: GPL-2.0-or-later
// Port of Dolphin's AX HLE (AX.cpp / AXVoice.h / DSPAccelerator.cpp, GPL-2.0-or-later).
#pragma once

#include "core/common.h"

class AXMixer {
 public:
  explicit AXMixer(u32 crc) : m_crc(crc) {}

  // Command list handlers
  void SetupProcessing(u32 init_addr);
  void DownloadAndMixWithVolume(u32 addr, u16 vol_main, u16 vol_auxa, u16 vol_auxb);
  void ProcessPBList(u32 pb_addr);
  void MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr);
  void UploadLRS(u32 dst_addr);
  void SetMainLR(u32 src_addr);
  void RunCompressor(u16 threshold, u16 release_frames, u32 table_addr, u32 millis);
  void OutputSamples(u32 lr_addr, u32 surround_addr);
  void MixAUXBLR(u32 ul_addr, u32 dl_addr);
  void SetOppositeLR(u32 src_addr);
  void SendAUXAndMix(u32 auxa_lrs_up, u32 auxb_s_up, u32 main_l_dl, u32 main_r_dl, u32 auxb_l_dl,
                     u32 auxb_r_dl);

 private:
  static constexpr int SAMPLES = 32 * 5;  // 5 ms at 32 kHz
  u32 ConvertMixerControl(u32 mixer_control) const;
  bool HasLPF() const { return m_crc != 0x4E8A8B21; }

  u32 m_crc;
  u16 m_compressor_pos = 0;
  int m_main_left[SAMPLES] = {}, m_main_right[SAMPLES] = {}, m_main_surround[SAMPLES] = {};
  int m_auxa_left[SAMPLES] = {}, m_auxa_right[SAMPLES] = {}, m_auxa_surround[SAMPLES] = {};
  int m_auxb_left[SAMPLES] = {}, m_auxb_right[SAMPLES] = {}, m_auxb_surround[SAMPLES] = {};
};
