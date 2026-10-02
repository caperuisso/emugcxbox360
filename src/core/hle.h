// emugcxbox360 - HLE hooks on guest functions
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A hooked function has its first instruction replaced by a reserved opcode
// (primary opcode 1, unused on Gekko). Executing it calls the C++ handler,
// which then returns to the caller (LR) like a "blr".
#pragma once

#include "core/common.h"

namespace HLE {

using Handler = void (*)();

void Clear();
// Hooks the function at guest address addr (must already be loaded in RAM).
bool Patch(u32 addr, Handler handler, const char* name);
// Called by the interpreter for opcode 1.
void Execute(u32 inst);

// Guest printf-style formatting (OSReport and friends): format in r3, args from r4/f1.
void OSReport();

}  // namespace HLE
