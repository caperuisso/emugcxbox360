// SPDX-License-Identifier: GPL-2.0-or-later
// DOL loader and a high level replacement for the GameCube IPL ("BS2"):
// sets up low memory, BATs and MSR the way the IPL leaves them, then either
// jumps to a DOL entry or runs the disc's apploader to load the game.
#include "core/loader/boot.h"

#include <algorithm>
#include <cstdio>

#include "core/disc/disc_reader.h"
#include "core/gekko/cpu.h"
#include "core/hw/hw.h"
#include "core/memory.h"

namespace Boot {

namespace {

bool ReadFile(const std::string& path, std::vector<u8>& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize((size_t)size);
  bool ok = fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
  return s;
}

bool EndsWith(const std::string& s, const char* suffix) {
  size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// State the IPL leaves behind before handing over to a game.
void SetupIPLState() {
  // BATs: 0x80000000 cached and 0xC0000000 uncached, both mapping 256 MiB at 0.
  cpu.spr[SPR_IBAT0U] = 0x80001FFF;
  cpu.spr[SPR_IBAT0U + 1] = 0x00000002;
  cpu.spr[SPR_DBAT0U] = 0x80001FFF;
  cpu.spr[SPR_DBAT0U + 1] = 0x00000002;
  cpu.spr[SPR_DBAT0U + 2] = 0xC0001FFF;
  cpu.spr[SPR_DBAT0U + 3] = 0x0000002A;
  Mem::UpdateBATs();

  cpu.msr = MSR_FP | MSR_ME | MSR_IR | MSR_DR | MSR_RI;
  cpu.spr[SPR_HID0] = 0x0011C464;
  cpu.spr[SPR_HID2] = 0xE0000000;  // LSQE | WPE | PSE: paired singles enabled
  cpu.gpr[1] = 0x816FFFF0;         // stack
  cpu.gpr[2] = 0x81465CC0;
  cpu.gpr[13] = 0x81465320;

  // Low memory globals ("OSGlobals") the SDK and libogc read.
  Mem::PhysWrite32(0x20, 0x0D15EA5E);  // boot magic
  Mem::PhysWrite32(0x24, 0x00000001);  // version
  Mem::PhysWrite32(0x28, Mem::MEM1_SIZE);
  Mem::PhysWrite32(0x2C, 0x00000003);  // console type: retail (latest)
  Mem::PhysWrite32(0x30, 0x00000000);  // arena low (OS computes it)
  Mem::PhysWrite32(0x34, 0x817FE8C0);  // arena high
  Mem::PhysWrite32(0xCC, 0x00000000);  // TV mode NTSC
  Mem::PhysWrite32(0xEC, 0x81800000);  // simulated memory top
  Mem::PhysWrite32(0xF0, Mem::MEM1_SIZE);
  Mem::PhysWrite32(0xF8, BUS_CLOCK);
  Mem::PhysWrite32(0xFC, CPU_CLOCK);
  Mem::PhysWrite16(0x30E0, 0x0006);    // production pads
  Mem::PhysWrite8(0x30E3, 0x00);
  Mem::PhysWrite32(0x30D8, 0);

  // Exception vectors: "rfi" stubs so stray exceptions return safely until the
  // game installs its own handlers.
  for (u32 vec = 0x100; vec <= 0x1700; vec += 0x100) Mem::PhysWrite32(vec, 0x4C000064);
}

// Calls a guest function through HLE and returns r3.
u32 CallGuest(u32 addr, u32 r3 = 0, u32 r4 = 0, u32 r5 = 0) {
  cpu.gpr[3] = r3;
  cpu.gpr[4] = r4;
  cpu.gpr[5] = r5;
  LR = 0;
  cpu.pc = addr;
  if (!CPU::RunUntil(0, 500000000ull)) LOG("Boot: guest call %08x did not return\n", addr);
  return cpu.gpr[3];
}

bool RunApploader() {
  u8 header[0x20];
  if (!DI::ReadDisc(0x2440, header, sizeof(header))) return false;
  u32 entry = LoadBE32(header + 0x10);
  u32 size = LoadBE32(header + 0x14) + LoadBE32(header + 0x18);
  constexpr u32 LOAD_ADDR = 0x01200000;
  u8* dst = Mem::PhysPtr(LOAD_ADDR, size);
  if (!dst || !DI::ReadDisc(0x2460, dst, size)) return false;
  LOG("Boot: apploader %.10s entry %08x size %u\n", (const char*)header, entry, size);

  // Pointers handed to the apploader live in a scratch area of low memory.
  constexpr u32 SCRATCH = 0x80004000;
  constexpr u32 BLR_STUB = 0x80004100;  // OSReport replacement: just returns
  Mem::Write32(BLR_STUB, 0x4E800020);
  CallGuest(entry, SCRATCH, SCRATCH + 4, SCRATCH + 8);
  u32 fn_init = Mem::Read32(SCRATCH), fn_main = Mem::Read32(SCRATCH + 4), fn_close = Mem::Read32(SCRATCH + 8);

  CallGuest(fn_init, BLR_STUB);
  for (int guard = 0; guard < 10000; guard++) {
    u32 more = CallGuest(fn_main, SCRATCH, SCRATCH + 4, SCRATCH + 8);
    if (!more) break;
    u32 ram = Mem::Read32(SCRATCH) & 0x01FFFFFF;
    u32 len = Mem::Read32(SCRATCH + 4);
    u32 disc_off = Mem::Read32(SCRATCH + 8);
    u8* p = Mem::PhysPtr(ram, len);
    if (!p || !DI::ReadDisc(disc_off, p, len)) {
      LOG("Boot: apploader read failed ram=%08x len=%u off=%08x\n", ram, len, disc_off);
      return false;
    }
  }
  u32 game_entry = CallGuest(fn_close);
  LOG("Boot: game entry %08x\n", game_entry);
  cpu.pc = game_entry;
  return game_entry != 0;
}

}  // namespace

u32 LoadDOL(const std::vector<u8>& dol) {
  if (dol.size() < 0x100) return 0;
  const u8* h = dol.data();
  for (int s = 0; s < 18; s++) {
    u32 off = LoadBE32(h + s * 4);
    u32 addr = LoadBE32(h + 0x48 + s * 4);
    u32 size = LoadBE32(h + 0x90 + s * 4);
    if (!size) continue;
    u8* dst = Mem::PhysPtr(addr, size);
    if (!dst || (u64)off + size > dol.size()) {
      LOG("DOL: bad section %d (addr %08x size %u)\n", s, addr, size);
      return 0;
    }
    memcpy(dst, h + off, size);
  }
  // BSS is not cleared here: RAM starts zeroed and libogc DOLs often declare a
  // BSS range that overlaps their data sections; their crt0 clears it anyway.
  return LoadBE32(h + 0xE0);
}

bool BootFile(const std::string& path) {
  std::string lower = Lower(path);
  SetupIPLState();

  if (EndsWith(lower, ".dol")) {
    std::vector<u8> data;
    if (!ReadFile(path, data)) {
      LOG("Boot: cannot read %s\n", path.c_str());
      return false;
    }
    u32 entry = LoadDOL(data);
    if (!entry) return false;
    LOG("Boot: DOL entry %08x\n", entry);
    cpu.pc = entry;
    return true;
  }

  if (IsDiscImagePath(lower)) {
    if (!DI::OpenDisc(path)) {
      LOG("Boot: cannot open disc %s\n", path.c_str());
      return false;
    }
    u8 disc_header[0x20];
    DI::ReadDisc(0, disc_header, sizeof(disc_header));
    memcpy(Mem::PhysPtr(0, 0x20), disc_header, 0x20);  // game ID etc. at 0x80000000
    LOG("Boot: disc %.6s\n", (const char*)disc_header);
    return RunApploader();
  }

  LOG("Boot: unsupported file type: %s\n", path.c_str());
  return false;
}

}  // namespace Boot
