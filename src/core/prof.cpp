// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/prof.h"

#include <chrono>
#include <cstring>

namespace Prof {

u64 g_ticks[NUM_CATS];

namespace {
Cat s_cur = OTHER;
u64 s_start = 0;
}  // namespace

u64 Now() {
#if defined(XENON)
  u32 tb;
  asm volatile("mftb %0" : "=r"(tb));
  return tb;  // 32 bits are plenty between two samples (wraps after ~80 s)
#else
  return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
#endif
}

Cat Switch(Cat c) {
  u64 now = Now();
  g_ticks[s_cur] += (u64)(u32)(now - s_start);
  s_start = now;
  Cat prev = s_cur;
  s_cur = c;
  return prev;
}

void Take(u64 out[NUM_CATS]) {
  Switch(s_cur);
  memcpy(out, g_ticks, sizeof(g_ticks));
  memset(g_ticks, 0, sizeof(g_ticks));
}

}  // namespace Prof
