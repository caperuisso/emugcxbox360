// emugcxbox360 - high level emulation of the GameCube DSP microcodes
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The DSP runs microcode uploaded by the game. Instead of emulating the DSP
// core, we recognise the microcode by hash and reproduce its mailbox protocol
// (approach and protocol details from Dolphin's DSPHLE, GPL-2.0-or-later).
#pragma once

#include "core/common.h"

namespace DSPHLE {

void Reset();

// DSP control register bits owned by the DSP side (mask 0x0C07).
constexpr u16 CONTROL_MASK = 0x0C07;
u16 WriteControl(u16 value);
u16 ReadControl();

// DSP -> CPU mailbox
u16 ReadMailHigh();
u16 ReadMailLow();
// CPU -> DSP mailbox: a complete 32-bit mail written by the CPU.
void SendMail(u32 mail);

// Called about once per millisecond of guest time.
void Update();

// Name of the active microcode, for logs.
const char* CurrentUCodeName();

}  // namespace DSPHLE

namespace DSP {
// Raises the DSP->CPU interrupt (DSP_CONTROL bit 7) after a delay.
void GenerateDSPInterrupt(s64 cycles_into_future = 0);
}  // namespace DSP
