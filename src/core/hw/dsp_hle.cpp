// SPDX-License-Identifier: GPL-2.0-or-later
// DSP microcode HLE. Mail protocols follow Dolphin's DSPHLE implementation.
// Audio rendering is not implemented yet: the microcodes answer every request
// so games run, and produce silence.
#include "core/hw/dsp_hle.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "core/coretiming.h"
#include "core/hw/ax_audio.h"
#include "core/hw/zelda_audio.h"
#include "core/memory.h"
#include "core/state.h"

namespace DSPHLE {

namespace {

constexpr u32 UCODE_ROM = 0x00000000;
constexpr u32 UCODE_INIT_AUDIO_SYSTEM = 0x00000001;

// Task mails shared by the Nintendo microcodes
constexpr u32 TASK_MAIL_MASK = 0xFFFF0000;
constexpr u32 TASK_MAIL_TO_CPU = 0xDCD10000;
constexpr u32 DSP_INIT = TASK_MAIL_TO_CPU | 0;
constexpr u32 DSP_RESUME = TASK_MAIL_TO_CPU | 1;
constexpr u32 DSP_YIELD = TASK_MAIL_TO_CPU | 2;
constexpr u32 DSP_DONE = TASK_MAIL_TO_CPU | 3;
constexpr u32 DSP_SYNC = TASK_MAIL_TO_CPU | 4;
constexpr u32 DSP_FRAME_END = TASK_MAIL_TO_CPU | 5;
constexpr u32 TASK_MAIL_TO_DSP = 0xCDD10000;
constexpr u32 MAIL_RESUME = TASK_MAIL_TO_DSP | 0;
constexpr u32 MAIL_NEW_UCODE = TASK_MAIL_TO_DSP | 1;
constexpr u32 MAIL_RESET = TASK_MAIL_TO_DSP | 2;
constexpr u32 MAIL_CONTINUE = TASK_MAIL_TO_DSP | 3;

// ---- DSP -> CPU mail queue ----
struct PendingMail {
  u32 mail;
  bool interrupt;  // raise the DSP interrupt once this mail has been read
};

// Fixed-size FIFO (no constructor needed, unlike std::deque).
struct MailQueue {
  static constexpr u32 CAPACITY = 256;
  PendingMail items[CAPACITY];
  u32 head = 0, count = 0;
  bool empty() const { return count == 0; }
  void clear() { head = count = 0; }
  PendingMail& front() { return items[head]; }
  void pop_front() {
    head = (head + 1) % CAPACITY;
    count--;
  }
  void push_back(const PendingMail& m) {
    if (count == CAPACITY) {
      LOG("DSP: mail queue full, dropping %08x\n", m.mail);
      return;
    }
    items[(head + count) % CAPACITY] = m;
    count++;
  }
};
MailQueue s_mails;
u32 s_last_mail = 0;
bool s_halted = true;

void PushMail(u32 mail, bool interrupt = false, s64 cycles = 0) {
  if (interrupt) {
    if (s_mails.empty())
      DSP::GenerateDSPInterrupt(cycles);
    else
      s_mails.front().interrupt = true;
  }
  s_mails.push_back({mail, false});
}

// ---- Microcode interface ----
class UCode;
std::unique_ptr<UCode> s_ucode, s_last_ucode;
u32 s_pending_crc = 0;
bool s_pending_switch = false, s_pending_swap = false;

void RequestUCode(u32 crc, bool swap) {
  s_pending_crc = crc;
  s_pending_switch = true;
  s_pending_swap = swap;
}

u32 HashEctor(const u8* data, u32 len) {
  u32 crc = 0;
  for (u32 i = 0; i < len; i++) {
    crc ^= data[i];
    crc = (crc << 3) | (crc >> 29);
  }
  return crc;
}

class UCode {
 public:
  explicit UCode(u32 crc) : m_crc(crc) {}
  virtual ~UCode() = default;
  virtual const char* Name() const = 0;
  virtual void Initialize() = 0;
  virtual void HandleMail(u32 mail) = 0;
  virtual void Update() {
    if (m_needs_resume_mail) {
      m_needs_resume_mail = false;
      PushMail(DSP_RESUME, true);
    }
  }
  u32 crc() const { return m_crc; }
  virtual void DoState(StateBuffer& s) {
    s.Do(m_upload_setup_in_progress);
    s.Do(m_needs_resume_mail);
    s.Do(m_next_steps);
    s.Do(m_iram_mram_addr);
    s.Do(m_iram_size);
  }

 protected:
  // Ten mails describing the next microcode (shared by AX, Zelda, GBA...).
  void PrepareBootUCode(u32 mail) {
    switch (m_next_steps) {
      case 3: m_iram_mram_addr = mail; break;
      case 4: m_iram_size = mail & 0xFFFF; break;
      default: break;
    }
    if (++m_next_steps < 10) return;
    m_next_steps = 0;
    m_needs_resume_mail = true;
    m_upload_setup_in_progress = false;
    const u8* code = Mem::PhysPtr(m_iram_mram_addr & 0x01FFFFFF, m_iram_size);
    u32 crc = code ? HashEctor(code, m_iram_size) : 0xFFFFFFFF;
    RequestUCode(crc, true);
  }

  u32 m_crc;
  bool m_upload_setup_in_progress = false;
  bool m_needs_resume_mail = false;
  int m_next_steps = 0;
  u32 m_iram_mram_addr = 0;
  u32 m_iram_size = 0;
};

// The DSP boot ROM: receives the parameters of the microcode to upload.
class ROMUCode : public UCode {
 public:
  using UCode::UCode;
  const char* Name() const override { return "ROM"; }
  void Initialize() override { PushMail(0x8071FEED); }
  void HandleMail(u32 mail) override {
    if (m_next_parameter == 0) {
      if ((mail & 0xFFFF0000) != 0x80F30000)
        PushMail(0xFEEE0000 | (mail & 0xFFFF));
      else
        m_next_parameter = mail;
      return;
    }
    switch (m_next_parameter) {
      case 0x80F3A001: m_ram_address = mail; break;
      case 0x80F3A002: m_length = mail & 0xFFFF; break;
      case 0x80F3D001: {
        const u8* code = Mem::PhysPtr(m_ram_address & 0x01FFFFFF, m_length);
        RequestUCode(code ? HashEctor(code, m_length) : 0xFFFFFFFF, false);
        break;
      }
      default: break;  // IMEM destination, DMEM length: not needed for HLE
    }
    m_next_parameter = 0;
  }

 public:
  void DoState(StateBuffer& s) override {
    UCode::DoState(s);
    s.Do(m_next_parameter);
    s.Do(m_ram_address);
    s.Do(m_length);
  }

 private:
  u32 m_next_parameter = 0, m_ram_address = 0, m_length = 0;
};

// Tiny microcode the SDK runs from __OSInitAudioSystem.
class INITUCode : public UCode {
 public:
  using UCode::UCode;
  const char* Name() const override { return "INIT"; }
  void Initialize() override { PushMail(0x80544348); }
  void HandleMail(u32) override {}
  void Update() override {}
};

// Memory card unlock microcode.
class CARDUCode : public UCode {
 public:
  using UCode::UCode;
  const char* Name() const override { return "CARD"; }
  void Initialize() override { PushMail(DSP_INIT); }
  void Update() override {
    if (!s_mails.empty()) DSP::GenerateDSPInterrupt();
  }
  void HandleMail(u32 mail) override {
    if (mail != 0xFF000000) LOG("DSP CARD: unknown command %08x\n", mail);
    PushMail(DSP_DONE);
    RequestUCode(UCODE_ROM, false);
  }
};

// GBA link microcode. The key derivation itself is not implemented yet.
class GBAUCode : public UCode {
 public:
  using UCode::UCode;
  const char* Name() const override { return "GBA"; }
  void Initialize() override { PushMail(DSP_INIT); }
  void HandleMail(u32 mail) override {
    if (m_upload_setup_in_progress) {
      PrepareBootUCode(mail);
      return;
    }
    switch (m_state) {
      case 0:
        if (mail == 0xABBA0000) m_state = 1;
        break;
      case 1:
        LOG("DSP GBA: crypto request at %08x not implemented\n", mail & 0x0FFFFFFF);
        PushMail(DSP_DONE);
        m_state = 2;
        break;
      default:
        if (mail == MAIL_NEW_UCODE)
          m_upload_setup_in_progress = true;
        else if (mail == MAIL_RESET)
          RequestUCode(UCODE_ROM, false);
        break;
    }
  }

 public:
  void DoState(StateBuffer& s) override {
    UCode::DoState(s);
    s.Do(m_state);
  }

 private:
  int m_state = 0;
};

// "AX": the audio microcode used by most games.
class AXUCode : public UCode {
 public:
  using UCode::UCode;
  const char* Name() const override { return "AX"; }
  void Initialize() override { PushMail(DSP_INIT, true); }

  void HandleMail(u32 mail) override {
    if (m_upload_setup_in_progress) {
      PrepareBootUCode(mail);
      return;
    }
    switch (m_state) {
      case State::WaitingForCmdListSize:
        if ((mail & 0xFFFF0000) == 0xBABE0000) {
          m_cmdlist_size = mail & 0xFFFF;
          m_state = State::WaitingForCmdListAddress;
        } else {
          LOG("DSP AX: unexpected mail %08x\n", mail);
        }
        break;
      case State::WaitingForCmdListAddress:
        RunCommandList(mail, m_cmdlist_size);
        // The SDK times command lists; ~2500 cycles is the empty-list cost.
        PushMail(DSP_YIELD, true, 2500);
        m_state = State::WaitingForNextTask;
        break;
      case State::WaitingForNextTask:
        mail = TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK);  // the ucode ignores the prefix
        switch (mail) {
          case MAIL_RESUME:
            PushMail(DSP_RESUME, true);
            m_state = State::WaitingForCmdListSize;
            break;
          case MAIL_NEW_UCODE:
            m_upload_setup_in_progress = true;
            m_state = State::WaitingForCmdListSize;
            break;
          case MAIL_RESET: RequestUCode(UCODE_ROM, false); break;
          case MAIL_CONTINUE: m_state = State::WaitingForCmdListSize; break;
          default: LOG("DSP AX: unknown task mail %08x\n", mail); break;
        }
        break;
    }
  }

 public:
  void DoState(StateBuffer& s) override {
    UCode::DoState(s);
    s.Do(m_state);
    s.Do(m_cmdlist_size);
    s.Do(m_mixer);
  }

 private:
  enum class State { WaitingForCmdListSize, WaitingForCmdListAddress, WaitingForNextTask };

  // Executes a command list (formats from Dolphin's AXUCode::HandleCommandList).
  void RunCommandList(u32 addr, u32 size) {
    u16 list[512];
    auto hilo = [&](u32 i) { return ((u32)list[i] << 16) | list[i + 1]; };
    for (int guard = 0; guard < 64; guard++) {
      if (size > 512) return;
      const u8* p = Mem::PhysPtr(addr & 0x01FFFFFF, size * 2);
      if (!p) return;
      for (u32 i = 0; i < size; i++) list[i] = LoadBE16(p + i * 2);
      u32 i = 0, pb_addr = 0;
      bool more = false;
      while (i < size && !more) {
        u16 cmd = list[i++];
        switch (cmd) {
          case 0x00: m_mixer.SetupProcessing(hilo(i)); i += 2; break;
          case 0x01: m_mixer.DownloadAndMixWithVolume(hilo(i), list[i + 2], list[i + 3], list[i + 4]); i += 5; break;
          case 0x02: pb_addr = hilo(i); i += 2; break;
          case 0x03: m_mixer.ProcessPBList(pb_addr); break;
          case 0x04:
          case 0x05: m_mixer.MixAUXSamples(cmd - 0x04, hilo(i), hilo(i + 2)); i += 4; break;
          case 0x06: m_mixer.UploadLRS(hilo(i)); i += 2; break;
          case 0x07: m_mixer.SetMainLR(hilo(i)); i += 2; break;
          case 0x08: i += 10; break;  // unknown, unused
          case 0x09: m_mixer.MixAUXSamples(1, 0, hilo(i)); i += 2; break;
          case 0x0A: case 0x0B: case 0x0C: break;
          case 0x0D:  // continue with another list
            if (i + 3 > size) return;
            addr = hilo(i);
            size = list[i + 2];
            more = true;
            break;
          case 0x0E: m_mixer.OutputSamples(hilo(i + 2), hilo(i)); i += 4; break;
          case 0x0F: return;  // end
          case 0x10: m_mixer.MixAUXBLR(hilo(i), hilo(i + 2)); i += 4; break;
          case 0x11: m_mixer.SetOppositeLR(hilo(i)); i += 2; break;
          case 0x12: m_mixer.RunCompressor(list[i], list[i + 1], hilo(i + 2), 5); i += 4; break;
          case 0x13:
            m_mixer.SendAUXAndMix(hilo(i), hilo(i + 2), hilo(i + 4), hilo(i + 6), hilo(i + 8), hilo(i + 10));
            i += 12;
            break;
          default:
            LOG("DSP AX: unknown command %04x\n", cmd);
            return;
        }
      }
      if (!more) return;
    }
  }

  State m_state = State::WaitingForCmdListSize;
  u32 m_cmdlist_size = 0;
  AXMixer m_mixer{m_crc};
};

// "Zelda"/DAC microcode used by many Nintendo EAD games (Wind Waker, Mario
// Sunshine, Pikmin, Twilight Princess GC...).
class ZeldaUCode : public UCode {
 public:
  enum Flags : u32 {
    MAKE_DOLBY_LOUDER = 0x2,
    LIGHT_PROTOCOL = 0x4,
    FOUR_MIXING_DESTS = 0x8,
    TINY_VPB = 0x10,
    VOLUME_EXPLICIT_STEP = 0x20,
    SYNC_PER_FRAME = 0x40,
    NO_CMD_0D = 0x80,
    SUPPORTS_GBA_CRYPTO = 0x100,
    WEIRD_CMD_0C = 0x200,
    COMBINED_CMD_0D = 0x400,
  };

  ZeldaUCode(u32 crc, u32 flags) : UCode(crc), m_flags(flags) {
    m_renderer.SetFlags(flags & (MAKE_DOLBY_LOUDER | FOUR_MIXING_DESTS | TINY_VPB | VOLUME_EXPLICIT_STEP));
    for (auto& f : m_skip_flags) f = 0;
  }
  const char* Name() const override { return "Zelda"; }

  void Initialize() override {
    if (m_flags & LIGHT_PROTOCOL) {
      PushMail(0x88881111);
    } else {
      PushMail(DSP_INIT, true);
      PushMail(0xF3551111);  // handshake
    }
  }

  void HandleMail(u32 mail) override {
    if (m_upload_setup_in_progress) {
      PrepareBootUCode(mail);
      return;
    }
    if (m_flags & LIGHT_PROTOCOL)
      HandleMailLight(mail);
    else
      HandleMailDefault(mail);
  }

 public:
  void DoState(StateBuffer& s) override {
    UCode::DoState(s);
    s.Do(m_flags);
    s.Do(m_mail_state);
    s.Do(m_expected_cmd_mails);
    s.Do(m_cmd_buffer);
    s.Do(m_read);
    s.Do(m_write);
    s.Do(m_pending_commands);
    s.Do(m_cmd_can_execute);
    s.Do(m_requested_frames);
    s.Do(m_curr_frame);
    s.Do(m_voices_per_frame);
    s.Do(m_curr_voice);
    s.Do(m_sync_max_voice);
    s.Do(m_sync_second_half);
    s.Do(m_skip_flags);
    s.Do(m_renderer);
  }

 private:
  enum class MailState { Waiting, Rendering, WritingCmd, Halted };

  bool RenderingInProgress() const { return m_curr_frame != m_requested_frames; }

  void HandleMailDefault(u32 mail) {
    switch (m_mail_state) {
      case MailState::Waiting:
        if (mail & 0x80000000) {
          mail = TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK);
          switch (mail) {
            case MAIL_NEW_UCODE:
              m_cmd_can_execute = true;
              RunPendingCommands();
              m_upload_setup_in_progress = true;
              break;
            case MAIL_RESET:
              m_mail_state = MailState::Halted;
              RequestUCode(UCODE_ROM, false);
              break;
            case MAIL_CONTINUE:
              m_cmd_can_execute = true;
              RunPendingCommands();
              break;
            default:  // MAIL_RESUME and unknown: halt
              m_mail_state = MailState::Halted;
              break;
          }
        } else if (!(mail & 0xFFFF)) {
          m_mail_state = RenderingInProgress() ? MailState::Rendering : MailState::Halted;
        } else {
          m_mail_state = MailState::WritingCmd;
          m_expected_cmd_mails = mail & 0xFFFF;
        }
        break;
      case MailState::Rendering:
        if (m_flags & SYNC_PER_FRAME) {
          int base = m_sync_second_half ? 2 : 0;
          m_skip_flags[base] = (u16)(mail >> 16);
          m_skip_flags[base + 1] = (u16)mail;
          if (m_sync_second_half) m_sync_max_voice = 0xFFFF;
          RenderAudio();
          if (m_sync_second_half) m_mail_state = MailState::Waiting;
          m_sync_second_half = !m_sync_second_half;
        } else {
          m_sync_max_voice = (((mail >> 16) & 0xF) + 1) << 4;
          m_skip_flags[(mail >> 16) & 0xFF] = (u16)mail;
          RenderAudio();
          m_mail_state = MailState::Waiting;
        }
        break;
      case MailState::WritingCmd:
        Write32(mail);
        if (--m_expected_cmd_mails == 0) {
          m_pending_commands++;
          m_mail_state = MailState::Waiting;
          RunPendingCommands();
        }
        break;
      case MailState::Halted: break;
    }
  }

  void HandleMailLight(u32 mail) {
    bool add_command = true;
    switch (m_mail_state) {
      case MailState::Waiting:
        Write32(mail);
        switch ((mail >> 24) & 0x7F) {
          case 0x00: m_expected_cmd_mails = 0; break;
          case 0x01: m_expected_cmd_mails = 4; break;
          case 0x02: m_expected_cmd_mails = 2; break;
          case 0x03: add_command = false; break;
          case 0x0C:
            m_expected_cmd_mails = (m_flags & SUPPORTS_GBA_CRYPTO) ? 1 : (m_flags & WEIRD_CMD_0C) ? 2 : 0;
            break;
          default: LOG("DSP Zelda: unknown light command %08x\n", mail); break;
        }
        if (m_expected_cmd_mails) {
          m_mail_state = MailState::WritingCmd;
        } else if (add_command) {
          m_pending_commands++;
          RunPendingCommands();
        }
        break;
      case MailState::WritingCmd:
        Write32(mail);
        if (--m_expected_cmd_mails == 0) {
          m_pending_commands++;
          m_mail_state = MailState::Waiting;
          RunPendingCommands();
        }
        break;
      case MailState::Rendering:
        m_sync_max_voice = 0xFFFFFFFF;
        for (auto& f : m_skip_flags) f = 0xFFFF;
        RenderAudio();
        DSP::GenerateDSPInterrupt();
        break;
      case MailState::Halted: break;
    }
  }

  u32 Read32() {
    if (m_read == m_write) return 0;
    u32 v = m_cmd_buffer[m_read];
    m_read = (m_read + 1) % 64;
    return v;
  }
  void Write32(u32 v) {
    m_cmd_buffer[m_write] = v;
    m_write = (m_write + 1) % 64;
  }

  void RunPendingCommands() {
    if (RenderingInProgress() || !m_cmd_can_execute) return;
    while (m_pending_commands) {
      u32 cmd_mail = Read32();
      if (!(cmd_mail & 0x80000000)) continue;
      u32 command = (cmd_mail >> 24) & 0x7F;
      u16 sync = (u16)(cmd_mail >> 16);
      m_pending_commands--;
      switch (command) {
        case 0x00: case 0x0A: case 0x0B: case 0x0F: SendAck(false, sync); break;
        case 0x03: SendAck(false, sync); break;
        case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:
          m_mail_state = MailState::Halted;
          return;
        case 0x01:  // setup: VPB base, coefficient tables, AFC table, reverb PBs
          m_voices_per_frame = cmd_mail & 0xFFFF;
          {
            u32 vpb = Read32(), coeffs = Read32(), afc = Read32(), reverb = Read32();
            m_renderer.Setup(vpb, coeffs, afc, reverb, !(m_flags & LIGHT_PROTOCOL));
          }
          SendAck(false, sync);
          break;
        case 0x02:  // render frames
          m_requested_frames = (cmd_mail >> 16) & 0xFF;
          {
            u32 left = Read32(), right = Read32();
            m_renderer.SetOutput((u16)cmd_mail, left, right);
          }
          if (m_flags & COMBINED_CMD_0D) {
            Read32();
            Read32();
          }
          m_curr_frame = 0;
          if (m_flags & LIGHT_PROTOCOL) {
            SendAck(false, (u16)m_requested_frames);
            m_mail_state = MailState::Rendering;
          } else {
            RenderAudio();
          }
          return;
        case 0x0C:
          if (m_flags & SUPPORTS_GBA_CRYPTO) {
            Read32();
          } else if (m_flags & WEIRD_CMD_0C) {
            Read32();
            Read32();
          }
          SendAck(false, sync);
          break;
        case 0x0D:
          if (!(m_flags & NO_CMD_0D)) Read32();
          SendAck(false, sync);
          break;
        case 0x0E:
          Read32();  // Wii ARAM base
          SendAck(false, sync);
          break;
        default:
          m_mail_state = MailState::Halted;
          return;
      }
    }
  }

  void SendAck(bool done_rendering, u16 sync) {
    if (m_flags & LIGHT_PROTOCOL) {
      PushMail(0x80000000 | (2 * ((sync >> 8) & 0x7F) + 0x62));
      return;
    }
    PushMail(done_rendering ? DSP_FRAME_END : DSP_SYNC, true);
    if (!done_rendering) PushMail(0xF3550000 | sync);
  }

  // Renders the requested frames voice by voice, waiting for the CPU's sync
  // mails between groups of voices like the real microcode.
  void RenderAudio() {
    if (!RenderingInProgress()) return;
    while (m_curr_frame < m_requested_frames) {
      if (m_curr_voice == 0) m_renderer.PrepareFrame();
      u32 voices = std::min<u32>(m_voices_per_frame, 256u * 16u);
      while (m_curr_voice < voices) {
        if (m_curr_voice >= m_sync_max_voice) return;  // wait for the next sync mail
        u16 flags = m_skip_flags[m_curr_voice >> 4];
        if (flags & (1u << (15 - (m_curr_voice & 0xF)))) m_renderer.AddVoice((u16)m_curr_voice);
        m_curr_voice++;
      }
      if (!(m_flags & LIGHT_PROTOCOL)) SendAck(false, (u16)(0xFF00 | m_curr_frame));
      m_renderer.FinalizeFrame();
      m_curr_voice = 0;
      m_sync_max_voice = 0;
      m_curr_frame++;
    }
    if (!(m_flags & LIGHT_PROTOCOL)) {
      SendAck(true, 0);
      m_cmd_can_execute = false;  // until the CPU acknowledges
    } else {
      m_mail_state = MailState::Waiting;
    }
  }

  u32 m_flags;
  MailState m_mail_state = MailState::Waiting;
  u32 m_expected_cmd_mails = 0;
  u32 m_cmd_buffer[64] = {};
  u32 m_read = 0, m_write = 0;
  u32 m_pending_commands = 0;
  bool m_cmd_can_execute = true;
  u32 m_requested_frames = 0, m_curr_frame = 0;
  u32 m_voices_per_frame = 0, m_curr_voice = 0;
  u32 m_sync_max_voice = 0;
  bool m_sync_second_half = false;
  u16 m_skip_flags[256];
  ZeldaAudioRenderer m_renderer;
};

std::unique_ptr<UCode> CreateUCode(u32 crc) {
  switch (crc) {
    case UCODE_ROM: return std::make_unique<ROMUCode>(crc);
    case UCODE_INIT_AUDIO_SYSTEM: return std::make_unique<INITUCode>(crc);
    case 0x65D6CC6F: return std::make_unique<CARDUCode>(crc);
    case 0xDD7E72D5: return std::make_unique<GBAUCode>(crc);

    // Zelda family, with per-version protocol flags (from Dolphin)
    case 0x24B22038:
      return std::make_unique<ZeldaUCode>(crc, ZeldaUCode::LIGHT_PROTOCOL | ZeldaUCode::FOUR_MIXING_DESTS |
                                                   ZeldaUCode::TINY_VPB | ZeldaUCode::VOLUME_EXPLICIT_STEP |
                                                   ZeldaUCode::NO_CMD_0D | ZeldaUCode::WEIRD_CMD_0C);
    case 0x6BA3B3EA:
      return std::make_unique<ZeldaUCode>(crc, ZeldaUCode::LIGHT_PROTOCOL | ZeldaUCode::FOUR_MIXING_DESTS |
                                                   ZeldaUCode::NO_CMD_0D);
    case 0xDF059F68:
    case 0x4BE6A5CB: return std::make_unique<ZeldaUCode>(crc, ZeldaUCode::LIGHT_PROTOCOL | ZeldaUCode::NO_CMD_0D | ZeldaUCode::SUPPORTS_GBA_CRYPTO);
    case 0x42F64AC4: return std::make_unique<ZeldaUCode>(crc, ZeldaUCode::LIGHT_PROTOCOL | ZeldaUCode::NO_CMD_0D | ZeldaUCode::WEIRD_CMD_0C);
    case 0x267FD05A:
    case 0x56D36052: return std::make_unique<ZeldaUCode>(crc, ZeldaUCode::SYNC_PER_FRAME | ZeldaUCode::NO_CMD_0D);
    case 0x86840740:  // Wind Waker
      return std::make_unique<ZeldaUCode>(crc, 0);
    case 0x2FCDF1EC: return std::make_unique<ZeldaUCode>(crc, ZeldaUCode::MAKE_DOLBY_LOUDER);
    case 0x6CA33A6D:
      return std::make_unique<ZeldaUCode>(crc, ZeldaUCode::MAKE_DOLBY_LOUDER | ZeldaUCode::COMBINED_CMD_0D);

    default:
      // Every other retail microcode is a variant of AX (Dolphin does the same).
      LOG("DSP: microcode %08x, using AX\n", crc);
      return std::make_unique<AXUCode>(crc);
  }
}

void ApplyPendingSwitch() {
  while (s_pending_switch) {
    s_pending_switch = false;
    u32 crc = s_pending_crc;
    s_mails.clear();
    if (s_pending_swap && s_last_ucode && s_last_ucode->crc() == crc) {
      s_ucode = std::move(s_last_ucode);  // resume the previous microcode
    } else {
      if (s_pending_swap && !s_last_ucode) s_last_ucode = std::move(s_ucode);
      s_ucode = CreateUCode(crc);
      LOG("DSP: microcode %s (%08x)\n", s_ucode->Name(), crc);
      s_ucode->Initialize();
    }
  }
}

void SetUCode(u32 crc) {
  RequestUCode(crc, false);
  ApplyPendingSwitch();
}

// Control register state owned by the DSP side
bool s_reset = false, s_assert_int = false, s_halt = true, s_init_code = false, s_init = true;
u64 s_init_code_clear_cycle = 0;

}  // namespace

void Reset() {
  s_mails.clear();
  s_last_mail = 0;
  s_last_ucode.reset();
  s_pending_switch = false;
  s_reset = s_assert_int = s_init_code = false;
  s_halt = s_init = true;
  s_halted = true;
  SetUCode(UCODE_ROM);
}

void DoState(StateBuffer& s) {
  s.Marker("DSPHLE");
  s.Do(s_mails);
  s.Do(s_last_mail);
  s.Do(s_halted);
  s.Do(s_reset);
  s.Do(s_assert_int);
  s.Do(s_halt);
  s.Do(s_init_code);
  s.Do(s_init);
  s.Do(s_init_code_clear_cycle);
  // Microcodes are recreated from their hash, then restore their own state.
  for (std::unique_ptr<UCode>* slot : {&s_ucode, &s_last_ucode}) {
    u32 crc = *slot ? (*slot)->crc() : 0xFFFFFFFF;
    bool present = *slot != nullptr;
    s.Do(present);
    s.Do(crc);
    if (s.IsReading()) *slot = present ? CreateUCode(crc) : nullptr;
    if (*slot) (*slot)->DoState(s);
  }
  s_pending_switch = false;
}

u16 WriteControl(u16 v) {
  bool halt = v & 0x0004;
  if (halt != s_halt) s_halted = halt;
  if (v & 0x0001) SetUCode(UCODE_ROM);  // reset completes immediately
  bool init = v & 0x0800;
  bool init_code = v & 0x0400;
  if (s_init && !init) {
    // Clearing DSPInit runs the 128-byte init microcode the SDK placed at 0x81000000.
    SetUCode(UCODE_INIT_AUDIO_SYSTEM);
    init_code = true;
    s_init_code_clear_cycle = CoreTiming::GetTicks() + 130 * TB_DIVIDER;
  }
  s_reset = false;
  s_assert_int = v & 0x0002;
  s_halt = halt;
  s_init = init;
  s_init_code = init_code;
  return ReadControl();
}

u16 ReadControl() {
  if (s_init_code && CoreTiming::GetTicks() >= s_init_code_clear_cycle) s_init_code = false;
  return (s_reset ? 0x0001 : 0) | (s_assert_int ? 0x0002 : 0) | (s_halt ? 0x0004 : 0) |
         (s_init_code ? 0x0400 : 0) | (s_init ? 0x0800 : 0);
}

u16 ReadMailHigh() {
  if (!s_halted && !s_mails.empty()) s_last_mail = s_mails.front().mail;
  return (u16)(s_last_mail >> 16);
}

u16 ReadMailLow() {
  if (!s_halted && !s_mails.empty()) {
    s_last_mail = s_mails.front().mail;
    bool interrupt = s_mails.front().interrupt;
    s_mails.pop_front();
    if (interrupt) DSP::GenerateDSPInterrupt();
  }
  // Valid bit drops once the low half has been read.
  u16 low = (u16)s_last_mail;
  s_last_mail &= ~0x80000000u;
  return low;
}

void SendMail(u32 mail) {
  if (s_ucode) s_ucode->HandleMail(mail);
  ApplyPendingSwitch();
}

void Update() {
  if (s_ucode) s_ucode->Update();
  ApplyPendingSwitch();
}

const char* CurrentUCodeName() { return s_ucode ? s_ucode->Name() : "none"; }

}  // namespace DSPHLE
