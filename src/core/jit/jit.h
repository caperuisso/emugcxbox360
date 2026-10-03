// emugcxbox360 - PowerPC -> PowerPC dynamic recompiler (host = Xenon)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Blocks found by the block cache are compiled to host code that performs
// exactly the per-instruction bookkeeping of the cached interpreter (so the
// emulation stays bit-identical), with the instruction bodies either calling
// the interpreter handlers or inlined. Only available on PowerPC hosts (the
// Xbox 360, or the qemu-ppc test build).
#pragma once

#include "core/gekko/interp_internal.h"

namespace Jit {

using BlockFn = void (*)();

// True when the host can run generated PowerPC code and the JIT is enabled.
bool Enabled();
void SetEnabled(bool on);

// Generated code size in bytes (statistics).
size_t CodeBytes();

// Drops all generated code (the block cache is cleared with it).
void Clear();

// Compiles a block of `count` instructions; returns nullptr when out of code
// space (the caller then clears the cache).
BlockFn Compile(const u32* insts, const Interpreter::OpFn* fns, u32 count);

}  // namespace Jit
