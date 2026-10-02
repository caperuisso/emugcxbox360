// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/system.h"

#include <cstdarg>

#include "core/coretiming.h"
#include "core/gekko/cpu.h"
#include "core/hle.h"
#include "core/hw/hw.h"
#include "core/loader/boot.h"
#include "core/memory.h"
#include "core/state.h"

Host* g_host = nullptr;

void LogPrint(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (g_host)
    g_host->Log(buf);
  else
    fputs(buf, stderr);
}

void BootCheckpoint(const char* where) {
  if (g_host) g_host->Checkpoint(where);
}

void CodeGuard(const char* where) {
  if (g_host) g_host->CodeGuard(where);
}

namespace System {

namespace {
bool s_initialized = false;
}

bool Init(Host* host) {
  g_host = host;
  if (s_initialized) return true;
  BootCheckpoint("System::Init");
  if (!Mem::Init()) {
    LOG("System: out of memory\n");
    return false;
  }
  BootCheckpoint("Mem::Init");
  CoreTiming::Init();
  CPU::Init();
  BootCheckpoint("CPU::Init");
  HW::Init();
  BootCheckpoint("HW::Init");
  s_initialized = true;
  return true;
}

void Shutdown() {
  DI::CloseDisc();
  Mem::Shutdown();
  s_initialized = false;
}

bool Boot(const std::string& path) {
  CoreTiming::Init();
  Mem::Clear();
  BootCheckpoint("Mem::Clear");
  CPU::Reset();
  BootCheckpoint("CPU::Reset");
  HW::Reset();
  BootCheckpoint("HW::Reset");
  HLE::Clear();
  bool ok = Boot::BootFile(path);
  Mem::MarkWritten(0, Mem::MEM1_SIZE);  // loaders write RAM directly
  return ok;
}

namespace {
constexpr char STATE_MAGIC[8] = {'E', 'M', 'U', 'G', 'C', 'S', 'T', '1'};

void DoMachineState(StateBuffer& s) {
  CPU::DoState(s);
  Mem::DoState(s);
  CoreTiming::DoState(s);
  HW::DoState(s);
  s.Marker("End");
}
}  // namespace

bool SaveState(const std::string& path) {
  StateBuffer s(StateBuffer::Mode::Write);
  DoMachineState(s);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) {
    LOG("State: cannot write %s\n", path.c_str());
    return false;
  }
  u8 game_id[8] = {};
  memcpy(game_id, Mem::g_mem1, 6);
  bool ok = fwrite(STATE_MAGIC, 1, 8, f) == 8 && fwrite(game_id, 1, 8, f) == 8 &&
            fwrite(s.Data().data(), 1, s.Data().size(), f) == s.Data().size();
  fclose(f);
  LOG("State: saved %s (%u KiB)\n", path.c_str(), (unsigned)(s.Data().size() / 1024));
  return ok;
}

bool LoadState(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    LOG("State: cannot open %s\n", path.c_str());
    return false;
  }
  char magic[8];
  u8 game_id[8];
  bool header_ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, STATE_MAGIC, 8) == 0 && fread(game_id, 1, 8, f) == 8;
  if (!header_ok || memcmp(game_id, Mem::g_mem1, 6) != 0) {
    LOG("State: %s is not a state for the running game\n", path.c_str());
    fclose(f);
    return false;
  }
  StateBuffer s(StateBuffer::Mode::Read);
  fseek(f, 0, SEEK_END);
  long size = ftell(f) - 16;
  fseek(f, 16, SEEK_SET);
  s.Data().resize((size_t)size);
  bool read_ok = fread(s.Data().data(), 1, (size_t)size, f) == (size_t)size;
  fclose(f);
  if (!read_ok) return false;
  DoMachineState(s);
  if (!s.Ok()) {
    LOG("State: %s is corrupt or from another version\n", path.c_str());
    return false;
  }
  LOG("State: loaded %s\n", path.c_str());
  return true;
}

void RunFrame() {
  VI::g_field_done = false;
  // Safety bound: never spin more than ~4 guest frames without a VI field.
  u64 limit = cpu.cycles + (u64)CPU_CLOCK / 15;
  while (!VI::g_field_done && cpu.cycles < limit) CoreTiming::Advance();
}

}  // namespace System
