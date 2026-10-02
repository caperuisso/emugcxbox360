// emugcxbox360 - cached interpreter: pre-decoded basic blocks
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/common.h"

namespace BlockCache {

void Init();
void Clear();
// Physical address range whose code changed (icbi, DMA, HLE patches).
void Invalidate(u32 pa, u32 len);
// Runs until cpu.cycles reaches the scheduler's slice end.
void Run();

extern bool g_enabled;

}  // namespace BlockCache
