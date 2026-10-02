// SPDX-License-Identifier: GPL-2.0-or-later
// DVD Interface: high level emulation of the drive commands games use,
// backed by a raw GCM/ISO image.
#include "core/disc/disc_reader.h"
#include "core/gekko/block_cache.h"
#include "core/hw/hw.h"
#include "core/memory.h"
#include "core/state.h"

namespace DI {

namespace {
enum : u32 {
  SR_DEINTMASK = 1 << 1,
  SR_DEINT = 1 << 2,
  SR_TCINTMASK = 1 << 3,
  SR_TCINT = 1 << 4,
  SR_BRKINTMASK = 1 << 5,
  SR_BRKINT = 1 << 6,
  CVR_CVRINTMASK = 1 << 1,
  CVR_CVRINT = 1 << 2,
};

std::unique_ptr<DiscReader> s_disc;
u32 s_sr, s_cvr, s_cmd[3], s_mar, s_length, s_cr, s_immbuf, s_cfg;
u32 s_error;

void UpdateInterrupt() {
  bool active = ((s_sr & SR_DEINT) && (s_sr & SR_DEINTMASK)) || ((s_sr & SR_TCINT) && (s_sr & SR_TCINTMASK)) ||
                ((s_sr & SR_BRKINT) && (s_sr & SR_BRKINTMASK)) ||
                ((s_cvr & CVR_CVRINT) && (s_cvr & CVR_CVRINTMASK));
  PI::SetInterrupt(PI::INT_DI, active);
}

void ExecuteCommand() {
  u32 cmd = s_cmd[0] >> 24;
  bool error = false;
  switch (cmd) {
    case 0xA8: {  // read
      u64 offset = (u64)s_cmd[1] << 2;
      u32 len = s_cmd[2];
      if (s_cr & 2) {
        u8* dst = Mem::PhysPtr(s_mar, s_length);
        if (!dst || !ReadDisc(offset, dst, s_length)) error = true;
        BlockCache::Invalidate(s_mar, s_length);
        s_mar += s_length;
        s_length = 0;
      } else {
        u8 tmp[4] = {};
        error = !ReadDisc(offset, tmp, 4);
        s_immbuf = LoadBE32(tmp);
      }
      (void)len;
      break;
    }
    case 0x12: {  // inquiry: drive revision / date
      if (u8* dst = Mem::PhysPtr(s_mar, 32)) {
        memset(dst, 0, 32);
        StoreBE16(dst + 2, 0x0002);
        StoreBE16(dst + 4, 0x0606);
        StoreBE32(dst + 6, 0x20010608);
      }
      s_length = 0;
      break;
    }
    case 0xAB: break;                          // seek
    case 0xE0: s_immbuf = s_error; break;      // request error
    case 0xE1: case 0xE2: break;               // audio streaming (not emulated)
    case 0xE3: break;                          // stop motor
    case 0xE4: break;                          // audio buffer config
    default: LOG("DI: unhandled command %08x %08x %08x\n", s_cmd[0], s_cmd[1], s_cmd[2]); break;
  }
  s_cr &= ~1u;
  if (error) {
    s_error = 0x03023A00;  // "no seek complete" style error
    s_sr |= SR_DEINT;
  } else {
    s_sr |= SR_TCINT;
  }
  UpdateInterrupt();
}
}  // namespace

void Reset() {
  s_sr = s_cmd[0] = s_cmd[1] = s_cmd[2] = s_mar = s_length = s_cr = s_immbuf = s_cfg = s_error = 0;
  s_cvr = s_disc ? 0 : 1;  // bit 0: lid open
  UpdateInterrupt();
}

void DoState(StateBuffer& s) {
  s.Marker("DI");
  s.Do(s_sr);
  s.Do(s_cvr);
  s.Do(s_cmd);
  s.Do(s_mar);
  s.Do(s_length);
  s.Do(s_cr);
  s.Do(s_immbuf);
  s.Do(s_cfg);
  s.Do(s_error);
}

bool OpenDisc(const std::string& path) {
  CloseDisc();
  std::string error;
  s_disc = OpenDiscImage(path, &error);
  if (!s_disc) {
    LOG("DI: cannot open %s: %s\n", path.c_str(), error.c_str());
    return false;
  }
  LOG("DI: %s image, %llu bytes\n", s_disc->FormatName(), (unsigned long long)s_disc->Size());
  s_cvr = 0;
  return true;
}

void CloseDisc() {
  s_disc.reset();
}

bool HasDisc() { return s_disc != nullptr; }

bool ReadDisc(u64 offset, void* dst, u32 len) {
  return s_disc && s_disc->Read(offset, dst, len);
}

u32 Read32(u32 off) {
  switch (off) {
    case 0x00: return s_sr;
    case 0x04: return s_cvr;
    case 0x08: return s_cmd[0];
    case 0x0C: return s_cmd[1];
    case 0x10: return s_cmd[2];
    case 0x14: return s_mar;
    case 0x18: return s_length;
    case 0x1C: return s_cr;
    case 0x20: return s_immbuf;
    case 0x24: return s_cfg;
    default: return 0;
  }
}

void Write32(u32 off, u32 v) {
  switch (off) {
    case 0x00: {
      u32 ints = SR_DEINT | SR_TCINT | SR_BRKINT;
      s_sr = (s_sr & ints & ~(v & ints)) | (v & ~ints);
      UpdateInterrupt();
      break;
    }
    case 0x04:
      s_cvr = (s_cvr & 1) | (v & CVR_CVRINTMASK) | ((s_cvr & CVR_CVRINT) & ~(v & CVR_CVRINT));
      UpdateInterrupt();
      break;
    case 0x08: s_cmd[0] = v; break;
    case 0x0C: s_cmd[1] = v; break;
    case 0x10: s_cmd[2] = v; break;
    case 0x14: s_mar = v & 0x03FFFFE0; break;
    case 0x18: s_length = v & ~31u; break;
    case 0x1C:
      s_cr = v;
      if (v & 1) ExecuteCommand();
      break;
    case 0x20: s_immbuf = v; break;
    default: break;
  }
}

}  // namespace DI
