// emugcxbox360 - GameCube memory card (EXI device), backed by a raw card image
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdio>
#include <string>

#include "core/common.h"

class MemoryCard {
 public:
  static constexpr u32 SIZE_MBITS = 4;                      // 59 usable blocks
  static constexpr u32 SIZE_BYTES = SIZE_MBITS * 1024 * 1024 / 8;
  static constexpr u32 BLOCK_SIZE = 0x2000;

  // Opens (or creates and formats) the card image. flash_id: 12 bytes from SRAM.
  bool Open(const std::string& path, const u8* flash_id);
  void Close();
  // Protocol state only: the card contents live in the image file.
  void DoState(StateBuffer& s);

  void Select(bool selected);
  u8 TransferByte(u8 in);
  // True (once) when the card raised its "command done" interrupt.
  bool TakeInterrupt();
  // Called by the scheduler when a delayed command completes.
  void CommandDone();
  // Cycles until CommandDone should run, or 0 if none is pending (consumed on read).
  s64 TakePendingDelay();

 private:
  void Format(const u8* flash_id);
  void Flush(u32 offset, u32 len);

  FILE* m_file = nullptr;
  u8* m_data = nullptr;
  u32 m_position = 0;
  u8 m_command = 0;
  u32 m_address = 0;
  u8 m_status = 0;
  u8 m_interrupt_switch = 0;
  bool m_interrupt = false;
  s64 m_pending_delay = 0;
  u8 m_program[128] = {};
};
