// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/system.h"

#include <cstdarg>

#include "core/coretiming.h"
#include "core/gekko/cpu.h"
#include "core/hw/hw.h"
#include "core/loader/boot.h"
#include "core/memory.h"

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

namespace System {

namespace {
bool s_initialized = false;
}

bool Init(Host* host) {
  g_host = host;
  if (s_initialized) return true;
  if (!Mem::Init()) {
    LOG("System: out of memory\n");
    return false;
  }
  CoreTiming::Init();
  CPU::Init();
  HW::Init();
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
  CPU::Reset();
  HW::Reset();
  return Boot::BootFile(path);
}

void RunFrame() {
  VI::g_field_done = false;
  // Safety bound: never spin more than ~4 guest frames without a VI field.
  u64 limit = cpu.cycles + (u64)CPU_CLOCK / 15;
  while (!VI::g_field_done && cpu.cycles < limit) CoreTiming::Advance();
}

}  // namespace System
