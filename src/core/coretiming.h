// emugcxbox360 - event scheduler driven by emulated CPU cycles
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/common.h"

namespace CoreTiming {

using Callback = void (*)(u64 userdata, s64 cycles_late);

void Init();
void DoState(StateBuffer& s);
int RegisterEvent(const char* name, Callback cb);
void ScheduleEvent(int type, s64 cycles_into_future, u64 userdata = 0);
void RemoveEvent(int type);
u64 GetTicks();

// Runs the CPU until the next event, then fires every due event.
void Advance();

// The CPU interpreter runs while cpu.cycles < g_slice_end.
extern u64 g_slice_end;

}  // namespace CoreTiming
