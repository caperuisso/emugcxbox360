// emugcxbox360 - simulator for the host PowerPC code produced by the JIT
// SPDX-License-Identifier: GPL-2.0-or-later
//
// On a PC the JIT's output cannot run natively, so it is executed by this
// small simulator to verify the code generator. It implements exactly the
// instruction subset the emitter produces, on a 32-bit address space split in
// regions: native-endian host memory (the CPU state, a stack, the code arena)
// and big-endian guest RAM.
#pragma once

#include "core/common.h"

namespace PPCSim {

// Virtual 32-bit addresses used by JIT code when it runs in the simulator
constexpr u32 STATE_BASE = 0x10000000;
constexpr u32 RAM_BASE = 0x20000000;
constexpr u32 STACK_BASE = 0x30000000;
constexpr u32 HELPER_BASE = 0x40000000;
constexpr u32 CODE_BASE = 0x50000000;

// A host helper call: the simulator passes r3..r6 and stores the result in r3.
using Helper = u32 (*)(u32 a0, u32 a1, u32 a2, u32 a3);

void Setup(u8* state, u32 state_size, u8* ram, u32 ram_size, const u32* code, u32 code_words,
           const Helper* helpers, u32 helper_count);

// Calls the function at a CODE_BASE address with r3/r4 arguments; returns r3.
u32 Call(u32 entry, u32 r3, u32 r4);

// Store observer for lockstep verification (guest RAM writes only)
using StoreHook = void (*)(u32 ram_offset, u32 size);
void SetStoreHook(StoreHook hook);

}  // namespace PPCSim
