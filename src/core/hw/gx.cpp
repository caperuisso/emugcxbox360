// SPDX-License-Identifier: GPL-2.0-or-later
// Command Processor / Pixel Engine registers and the GX FIFO command parser.
// Commands are decoded here and executed by the software GPU in core/video.
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "core/hw/hw.h"
#include "core/memory.h"
#include "core/prof.h"
#include "core/state.h"
#include "core/video/video.h"
#include "platform/platform.h"

namespace GX {

namespace {
// ---- CP / PE MMIO ----
u16 s_cp_regs[0x40];
u16 s_pe_ctrl;
u16 s_pe_token;

enum : u16 {
  PE_TOKEN_ENABLE = 1 << 0,
  PE_FINISH_ENABLE = 1 << 1,
  PE_TOKEN_INT = 1 << 2,
  PE_FINISH_INT = 1 << 3,
};

void UpdatePEInterrupts() {
  PI::SetInterrupt(PI::INT_PE_TOKEN, (s_pe_ctrl & PE_TOKEN_INT) && (s_pe_ctrl & PE_TOKEN_ENABLE));
  PI::SetInterrupt(PI::INT_PE_FINISH, (s_pe_ctrl & PE_FINISH_INT) && (s_pe_ctrl & PE_FINISH_ENABLE));
}

std::vector<u8> s_pending;

// ---- Dual core ----
bool s_threaded = false;
constexpr u32 RING_SIZE = 1u << 20;  // queued FIFO bytes (a power of two)
u8* s_ring = nullptr;
u32 s_head = 0;           // bytes produced (CPU thread), wraps around
u32 s_tail = 0;           // bytes consumed (GX thread)
bool s_stop = false;      // asks the GX thread to return
bool s_running = false;   // the GX thread is in its loop
// Pixel Engine events raised on the GX thread, applied on the CPU thread
enum : u32 { ASYNC_TOKEN = 1, ASYNC_TOKEN_INT = 2, ASYNC_FINISH_INT = 4 };
u32 s_async_flags = 0;
u16 s_async_token = 0;
u32 s_busy_ticks = 0;  // written by the GX thread only

void ProcessNow(const u8* data, u32 len);

// Parses one command at p (avail bytes). Returns bytes consumed, or 0 if incomplete.
u32 ParseCommand(const u8* p, u32 avail, bool in_display_list);

void RunDisplayList(u32 addr, u32 size) {
  const u8* p = Mem::PhysPtr(addr & 0x01FFFFFF, size);
  if (!p) return;
  u32 pos = 0;
  while (pos < size) {
    u32 n = ParseCommand(p + pos, size - pos, true);
    if (n == 0) break;
    pos += n;
  }
}

u32 ParseCommand(const u8* p, u32 avail, bool in_display_list) {
  u8 cmd = p[0];
  switch (cmd) {
    case 0x00:  // NOP
    case 0x44:  // unknown metrics
    case 0x48:  // invalidate vertex cache
      return 1;
    case 0x08:  // load CP register
      if (avail < 6) return 0;
      Video::LoadCPReg(p[1], LoadBE32(p + 2));
      return 6;
    case 0x10: {  // load XF registers
      if (avail < 5) return 0;
      u32 header = LoadBE32(p + 1);
      u32 n = ((header >> 16) & 0xF) + 1;
      if (avail < 5 + n * 4) return 0;
      Video::LoadXF(header & 0xFFFF, n, p + 5);
      return 5 + n * 4;
    }
    case 0x20:
    case 0x28:
    case 0x30:
    case 0x38:  // indexed XF loads (A..D)
      if (avail < 5) return 0;
      Video::LoadIndexedXF((cmd - 0x20) >> 3, LoadBE32(p + 1));
      return 5;
    case 0x40:  // call display list
      if (avail < 9) return 0;
      if (!in_display_list) RunDisplayList(LoadBE32(p + 1), LoadBE32(p + 5));
      return 9;
    case 0x61:  // load BP register
      if (avail < 5) return 0;
      Video::LoadBP(LoadBE32(p + 1));
      return 5;
    default:
      if (cmd & 0x80) {  // draw primitive
        if (avail < 3) return 0;
        u32 count = LoadBE16(p + 1);
        u32 total = 3 + count * Video::VertexSize(cmd & 7);
        if (avail < total) return 0;
        Video::Draw(cmd, count, p + 3);
        return total;
      }
      {
        static int warn = 0;
        if (warn++ < 16) LOG("GX: unknown FIFO opcode %02x\n", cmd);
      }
      return 1;
  }
}
}  // namespace

void Reset() {
  Sync();
  memset(s_cp_regs, 0, sizeof(s_cp_regs));
  s_pe_ctrl = 0;
  s_pe_token = 0;
  s_pending.clear();
  Video::Reset();
  UpdatePEInterrupts();
}

void DoState(StateBuffer& s) {
  Sync();  // nothing queued: the state is complete
  s.Marker("GX");
  s.Do(s_cp_regs);
  s.Do(s_pe_ctrl);
  s.Do(s_pe_token);
  s.Do(s_pending);
  Video::DoState(s);
}

namespace {

void ProcessNow(const u8* data, u32 len) {
  s_pending.insert(s_pending.end(), data, data + len);
  u32 pos = 0;
  while (pos < s_pending.size()) {
    u32 n = ParseCommand(s_pending.data() + pos, (u32)s_pending.size() - pos, false);
    if (n == 0) break;
    pos += n;
  }
  s_pending.erase(s_pending.begin(), s_pending.begin() + pos);
}

void ThreadMain(void*) {
  __atomic_store_n(&s_running, true, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
    u32 head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    u32 tail = s_tail;
    if (head == tail) {
      SpinPause();
      continue;
    }
    u32 off = tail & (RING_SIZE - 1);
    u32 n = std::min(head - tail, RING_SIZE - off);
    u64 t0 = Prof::Now();
    ProcessNow(s_ring + off, n);
    __atomic_store_n(&s_busy_ticks, s_busy_ticks + (u32)(Prof::Now() - t0), __ATOMIC_RELAXED);
    __atomic_store_n(&s_tail, tail + n, __ATOMIC_RELEASE);
  }
  __atomic_store_n(&s_running, false, __ATOMIC_RELEASE);
}

// CPU thread, GX thread idle: applies the Pixel Engine events it raised.
void ApplyAsync() {
  u32 flags = __atomic_exchange_n(&s_async_flags, 0, __ATOMIC_ACQUIRE);
  if (!flags) return;
  if (flags & ASYNC_TOKEN) s_pe_token = s_async_token;
  if (flags & ASYNC_TOKEN_INT) s_pe_ctrl |= PE_TOKEN_INT;
  if (flags & ASYNC_FINISH_INT) s_pe_ctrl |= PE_FINISH_INT;
  UpdatePEInterrupts();
}

}  // namespace

void ProcessFifo(const u8* data, u32 len) {
  if (!s_threaded) {
    Prof::Scope prof(Prof::GX);
    ProcessNow(data, len);
    return;
  }
  while (len) {
    u32 head = s_head;
    u32 room = RING_SIZE - (head - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE));
    if (!room) {  // the GX thread is behind: wait for it
      Prof::Scope prof(Prof::GX);
      while (RING_SIZE - (head - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE)) == 0) SpinPause();
      continue;
    }
    u32 off = head & (RING_SIZE - 1);
    u32 n = std::min(std::min(len, room), RING_SIZE - off);
    memcpy(s_ring + off, data, n);
    __atomic_store_n(&s_head, head + n, __ATOMIC_RELEASE);
    data += n;
    len -= n;
  }
}

void Sync() {
  if (!s_threaded) return;
  if (__atomic_load_n(&s_tail, __ATOMIC_ACQUIRE) != s_head) {
    Prof::Scope prof(Prof::GX);
    while (__atomic_load_n(&s_tail, __ATOMIC_ACQUIRE) != s_head) SpinPause();
  }
  ApplyAsync();
}

bool Threaded() { return s_threaded; }

u32 BusyTicks() { return __atomic_load_n(&s_busy_ticks, __ATOMIC_RELAXED); }

bool StartThread() {
  if (s_threaded) return true;
  if (!g_host) return false;
  if (!s_ring) s_ring = (u8*)malloc(RING_SIZE);
  if (!s_ring) return false;
  s_head = s_tail = 0;
  s_stop = false;
  s_async_flags = 0;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (!g_host->StartThread(ThreadMain, nullptr)) return false;
  s_threaded = true;
  LOG("GX: dual core (command stream on its own thread)\n");
  return true;
}

void StopThread() {
  if (!s_threaded) return;
  Sync();
  __atomic_store_n(&s_stop, true, __ATOMIC_RELEASE);
  while (__atomic_load_n(&s_running, __ATOMIC_ACQUIRE)) SpinPause();
  s_threaded = false;
}

u16 CPRead16(u32 off) {
  switch (off) {
    case 0x00: return 0x0002 | 0x0004 | 0x0008;  // FIFO empty, GP read idle, command idle
    default: return off < 0x80 ? s_cp_regs[off >> 1] : 0;
  }
}

void CPWrite16(u32 off, u16 v) {
  if (off < 0x80) s_cp_regs[off >> 1] = v;
  // The FIFO is consumed as soon as it is written: keep read pointer == write pointer.
  if (off == 0x34 || off == 0x36) {
    s_cp_regs[0x38 >> 1] = s_cp_regs[0x34 >> 1];
    s_cp_regs[0x3A >> 1] = s_cp_regs[0x36 >> 1];
    s_cp_regs[0x30 >> 1] = 0;
    s_cp_regs[0x32 >> 1] = 0;
  }
}

u16 PERead16(u32 off) {
  Sync();
  switch (off) {
    case 0x0A: return s_pe_ctrl;
    case 0x0E: return s_pe_token;
    default: return 0;
  }
}

void PEWrite16(u32 off, u16 v) {
  Sync();
  if (off == 0x0A) {
    u16 ints = PE_TOKEN_INT | PE_FINISH_INT;
    s_pe_ctrl = (s_pe_ctrl & ints & ~(v & ints)) | (v & (PE_TOKEN_ENABLE | PE_FINISH_ENABLE));
    UpdatePEInterrupts();
  } else if (off == 0x0E) {
    s_pe_token = v;
  }
}

}  // namespace GX

namespace Video {

void SignalDrawDone() {
  if (GX::s_threaded) {  // GX thread: applied by the CPU thread at its next sync
    __atomic_or_fetch(&GX::s_async_flags, GX::ASYNC_FINISH_INT, __ATOMIC_RELEASE);
    return;
  }
  GX::s_pe_ctrl |= GX::PE_FINISH_INT;
  GX::UpdatePEInterrupts();
}

void SignalToken(u16 token, bool interrupt) {
  if (GX::s_threaded) {
    GX::s_async_token = token;
    __atomic_or_fetch(&GX::s_async_flags, GX::ASYNC_TOKEN | (interrupt ? GX::ASYNC_TOKEN_INT : 0), __ATOMIC_RELEASE);
    return;
  }
  GX::s_pe_token = token;
  if (interrupt) {
    GX::s_pe_ctrl |= GX::PE_TOKEN_INT;
    GX::UpdatePEInterrupts();
  }
}

}  // namespace Video
