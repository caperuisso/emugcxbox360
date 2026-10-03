// emugcxbox360 - lightweight host time accounting per emulator component
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Time is charged exclusively to the innermost active category (a Scope
// pauses its parent), so the totals add up to the elapsed host time. The
// Xbox 360 frontend shows the shares on screen.
#pragma once

#include "core/common.h"

namespace Prof {

enum Cat { OTHER, CPU, GX, DSP, VI, NUM_CATS };

extern u64 g_ticks[NUM_CATS];

u64 Now();  // host ticks (timebase on the Xbox 360, nanoseconds elsewhere)
u64 TicksPerSecond();
Cat Switch(Cat c);

struct Scope {
  Cat prev;
  explicit Scope(Cat c) : prev(Switch(c)) {}
  ~Scope() { Switch(prev); }
};

// Snapshot of the totals since the last call (also charges the running span).
void Take(u64 out[NUM_CATS]);

}  // namespace Prof
