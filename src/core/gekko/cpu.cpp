// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/gekko/cpu.h"

#include "core/coretiming.h"
#include "core/memory.h"

CPUState cpu;

namespace CPU {

u32 g_cycles_per_instruction = 1;

namespace {
int s_dec_event = -1;

void DecrementerCallback(u64, s64) { cpu.exceptions |= EXC_DECREMENTER; }

void TakeException(u32 vector, u32 srr0, u32 srr1_extra) {
  cpu.spr[SPR_SRR0] = srr0;
  cpu.spr[SPR_SRR1] = (cpu.msr & 0x87C0FFFF) | srr1_extra;
  cpu.msr &= ~0x04EF36u;
  cpu.pc = vector | ((cpu.msr & MSR_IP) ? 0xFFF00000 : 0);
  cpu.reserve = false;
}
}  // namespace

void Init() {
  Interpreter::Init();
  s_dec_event = CoreTiming::RegisterEvent("Decrementer", DecrementerCallback);
}

void Reset() {
  memset(&cpu, 0, sizeof(cpu));
  cpu.spr[SPR_PVR] = 0x00083214;  // Gekko
  cpu.msr = 0;
  cpu.pc = 0xFFF00100;
  Mem::UpdateBATs();
}

void RaiseException(u32 exc) { cpu.exceptions |= exc; }

void SetExternalInterrupt(bool active) {
  if (active)
    cpu.exceptions |= EXC_EXTERNAL;
  else
    cpu.exceptions &= ~EXC_EXTERNAL;
}

void CheckExceptions() {
  u32 exc = cpu.exceptions;
  if (exc & EXC_SYNC_MASK) {
    if (exc & EXC_ISI) {
      TakeException(0x400, cpu.pc, 0x40000000);
    } else if (exc & EXC_DSI) {
      TakeException(0x300, cpu.pc, 0);
    } else if (exc & EXC_ALIGNMENT) {
      TakeException(0x600, cpu.pc, 0);
    } else if (exc & EXC_PROGRAM) {
      TakeException(0x700, cpu.pc, cpu.program_reason);
    } else if (exc & EXC_FPU_UNAVAILABLE) {
      TakeException(0x800, cpu.pc, 0);
    } else if (exc & EXC_SYSCALL) {
      TakeException(0xC00, cpu.pc + 4, 0);
    }
    cpu.exceptions &= ~EXC_SYNC_MASK;
    return;
  }
  if (!(cpu.msr & MSR_EE)) return;
  if (exc & EXC_EXTERNAL) {
    // Level triggered: the flag is cleared by the processor interface, not here.
    TakeException(0x500, cpu.pc, 0);
  } else if (exc & EXC_DECREMENTER) {
    cpu.exceptions &= ~EXC_DECREMENTER;
    TakeException(0x900, cpu.pc, 0);
  }
}

static inline void ExecuteOne() {
  u32 inst = Mem::ReadInstr(cpu.pc);
  if (UNLIKELY(cpu.exceptions & EXC_ISI)) {
    CheckExceptions();
    return;
  }
  cpu.npc = cpu.pc + 4;
  Interpreter::Execute(inst);
  cpu.cycles += g_cycles_per_instruction;
  if (UNLIKELY(cpu.exceptions)) {
    if (!(cpu.exceptions & EXC_SYNC_MASK)) cpu.pc = cpu.npc;
    CheckExceptions();
    return;
  }
  cpu.pc = cpu.npc;
}

void Run() {
  // Pending async interrupts may have become deliverable since the last slice.
  if (cpu.exceptions) CheckExceptions();
  while (cpu.cycles < CoreTiming::g_slice_end) ExecuteOne();
}

void Step() { ExecuteOne(); }

bool RunUntil(u32 stop_pc, u64 max_instructions) {
  for (u64 i = 0; i < max_instructions; i++) {
    if (cpu.pc == stop_pc) return true;
    ExecuteOne();
  }
  return cpu.pc == stop_pc;
}

u64 GetTimeBase() { return cpu.cycles / TB_DIVIDER + cpu.tb_offset; }

void SetTimeBase(u64 tb) { cpu.tb_offset = tb - cpu.cycles / TB_DIVIDER; }

u32 GetDecrementer() { return cpu.dec_start - (u32)(GetTimeBase() - cpu.dec_start_tb); }

void SetDecrementer(u32 value) {
  u32 old = GetDecrementer();
  cpu.dec_start = value;
  cpu.dec_start_tb = GetTimeBase();
  CoreTiming::RemoveEvent(s_dec_event);
  if (!(value & 0x80000000)) {
    CoreTiming::ScheduleEvent(s_dec_event, ((s64)value + 1) * TB_DIVIDER);
  } else if (!(old & 0x80000000)) {
    cpu.exceptions |= EXC_DECREMENTER;
  }
}

}  // namespace CPU
