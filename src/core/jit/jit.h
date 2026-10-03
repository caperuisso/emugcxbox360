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

// True when the host can run generated PowerPC code and the JIT is enabled.
bool Enabled();
void SetEnabled(bool on);
// Inlining of simple instructions (on by default; off = every instruction calls the interpreter).
void SetInlining(bool on);

// The block cache's map (physical word index -> block id, 0 = none), used by
// the generated dispatcher.
void SetBlockMap(const u32* map);

// Generated code size in bytes (statistics).
size_t CodeBytes();

// Drops all generated code (the block cache is cleared with it).
void Clear();

// Compiles block `id` (as stored in the map). Returns false when out of code
// space (the caller then clears the cache); blocks that cannot be compiled
// stay interpreted.
bool Compile(u32 id, const u32* insts, const Interpreter::OpFn* fns, u32 count);
bool HasBlock(u32 id);

// Runs generated code from block `id` (at cpu.pc, compiled) until the end of
// the time slice or something the generated code does not handle.
void Run(u32 id);

}  // namespace Jit
