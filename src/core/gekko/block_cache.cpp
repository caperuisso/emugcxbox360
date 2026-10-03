// SPDX-License-Identifier: GPL-2.0-or-later
// Cached interpreter. Each basic block (up to a branch, a translation-changing
// instruction, 64 instructions or a 4 KiB page boundary) is decoded once into
// direct handler pointers. Blocks are keyed by physical address and dropped a
// page at a time when code is invalidated (icbi, DMA, HLE patches).
#include "core/gekko/block_cache.h"

#include <cstdlib>
#include <vector>

#include "core/coretiming.h"
#include "core/gekko/interp_internal.h"
#include "core/jit/jit.h"
#include "core/memory.h"

namespace BlockCache {

bool g_enabled = true;

namespace {

struct Decoded {
  Interpreter::OpFn fn;
  u32 inst;
};

struct Block {
  u32 first;  // index into s_code
  u32 count;
};

constexpr u32 PAGE_SHIFT = 12;
constexpr u32 NUM_PAGES = Mem::MEM1_SIZE >> PAGE_SHIFT;
constexpr u32 MAX_BLOCK = 64;
constexpr size_t MAX_CODE = 4u << 20;  // decoded instructions before a full flush

u32* s_map = nullptr;       // physical word index -> block id + 1 (0 = none)
bool s_jit_full = false;    // code buffer exhausted: clear everything at the next block
u8* s_page_used = nullptr;  // pages that currently hold blocks
std::vector<Block> s_blocks;
std::vector<Decoded> s_code;

// Instructions after which the block must end: branches, system calls and
// anything that can change address translation.
bool EndsBlock(u32 inst) {
  u32 op = inst >> 26;
  switch (op) {
    case 16: case 17: case 18: case 1:  // bc, sc, b, HLE hook
      return true;
    case 19: {
      u32 ext = (inst >> 1) & 0x3FF;
      return ext == 16 || ext == 528 || ext == 50 || ext == 150;  // bclr, bcctr, rfi, isync
    }
    case 31: {
      u32 ext = (inst >> 1) & 0x3FF;
      if (ext == 467) {  // mtspr: only BAT writes change address translation
        u32 spr = ((inst >> 16) & 31) | (((inst >> 11) & 31) << 5);
        return spr >= 528 && spr < 544;
      }
      return ext == 146 || ext == 210 || ext == 242 || ext == 982;  // mtmsr, mtsr, mtsrin, icbi
    }
    default:
      return false;
  }
}

Block* Compile(u32 pa) {
  if (s_code.size() > MAX_CODE) Clear();
  Block b;
  b.first = (u32)s_code.size();
  b.count = 0;
  u32 addr = pa;
  const u8* mem = Mem::g_mem1;
  do {
    u32 inst = LoadBE32(mem + addr);
    s_code.push_back({Interpreter::Resolve(inst), inst});
    b.count++;
    addr += 4;
    if (EndsBlock(inst)) break;
  } while (b.count < MAX_BLOCK && (addr & ((1u << PAGE_SHIFT) - 1)) != 0 && addr < Mem::MEM1_SIZE);
  s_blocks.push_back(b);
  s_map[pa >> 2] = (u32)s_blocks.size();
  if (Jit::Enabled()) {
    u32 insts[MAX_BLOCK];
    Interpreter::OpFn fns[MAX_BLOCK];
    for (u32 k = 0; k < b.count; k++) {
      insts[k] = s_code[b.first + k].inst;
      fns[k] = s_code[b.first + k].fn;
    }
    if (!Jit::Compile((u32)s_blocks.size(), pa, insts, fns, b.count)) s_jit_full = true;
  }
  s_page_used[pa >> PAGE_SHIFT] = 1;
  return &s_blocks.back();
}

}  // namespace

void Init() {
  if (!s_map) s_map = (u32*)calloc(Mem::MEM1_SIZE / 4, sizeof(u32));
  if (!s_page_used) s_page_used = (u8*)calloc(NUM_PAGES, 1);
  Jit::SetBlockMap(s_map);
  Clear();
}

void Clear() {
  Jit::Clear();
  s_jit_full = false;
  if (s_map) memset(s_map, 0, Mem::MEM1_SIZE);
  if (s_page_used) memset(s_page_used, 0, NUM_PAGES);
  s_blocks.clear();
  s_code.clear();
}

void Invalidate(u32 pa, u32 len) {
  if (!s_map || !len) return;
  pa &= 0x1FFFFFFF;
  if (pa >= Mem::MEM1_SIZE) return;
  Jit::Unlink(pa, len);
  u32 end = pa + len > Mem::MEM1_SIZE ? Mem::MEM1_SIZE : pa + len;
  for (u32 page = pa >> PAGE_SHIFT; page <= (end - 1) >> PAGE_SHIFT; page++) {
    if (!s_page_used[page]) continue;
    memset(s_map + (page << (PAGE_SHIFT - 2)), 0, (1u << PAGE_SHIFT));
    s_page_used[page] = 0;
  }
}

void Run() {
  const u32 cpi = CPU::g_cycles_per_instruction;
  if (cpu.exceptions) CPU::CheckExceptions();
  while (cpu.cycles < CoreTiming::g_slice_end) {
    // Translate the block start (instruction side BATs)
    u32 pa;
    if (UNLIKELY(!Mem::TranslateInstr(cpu.pc, pa) || pa >= Mem::MEM1_SIZE || (pa & 3))) {
      CPU::Step();  // falls back to the plain interpreter (raises ISI if needed)
      continue;
    }
    if (UNLIKELY(s_jit_full)) Clear();
    u32 id = s_map[pa >> 2];
    const Block* b = id ? &s_blocks[id - 1] : Compile(pa);
    if (!id) id = (u32)s_blocks.size();
    // Generated code runs whole blocks only (same budget as Jit's: instructions
    // left in the slice); a block the slice ends inside is interpreted.
    if (Jit::HasBlock(id)) {
      u64 left = CoreTiming::g_slice_end - cpu.cycles;
      u64 budget = left >= 0x80000000ull ? 0x7FFFFFFFull : (left + cpi - 1) / cpi;
      if (budget >= b->count) {
        Jit::Run(id);
        continue;
      }
    }
    const Decoded* code = &s_code[b->first];
    for (u32 k = 0; k < b->count; k++) {
      const u32 pc = cpu.pc;
      cpu.npc = pc + 4;
      code[k].fn(code[k].inst);
      cpu.cycles += cpi;
      if (UNLIKELY(cpu.exceptions)) {
        if (!(cpu.exceptions & EXC_SYNC_MASK)) cpu.pc = cpu.npc;
        CPU::CheckExceptions();
        break;
      }
      cpu.pc = cpu.npc;
      if (UNLIKELY(CPU::g_idle)) {
        CPU::g_idle = false;
        if (CPU::g_idle_skipping && cpu.cycles < CoreTiming::g_slice_end) cpu.cycles = CoreTiming::g_slice_end;
      }
      if (cpu.npc != pc + 4 || cpu.cycles >= CoreTiming::g_slice_end) break;
    }
  }
}

}  // namespace BlockCache
