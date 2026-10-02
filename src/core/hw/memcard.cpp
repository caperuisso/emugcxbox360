// SPDX-License-Identifier: GPL-2.0-or-later
// Memory card protocol and the SDK's card format (from Dolphin's
// EXI_DeviceMemoryCard and GCMemcard, GPL-2.0-or-later).
#include "core/hw/memcard.h"

#include <cstdlib>
#include <ctime>

#include "core/state.h"

namespace {

enum : u8 {
  CMD_NINTENDO_ID = 0x00,
  CMD_READ_ARRAY = 0x52,
  CMD_SET_INTERRUPT = 0x81,
  CMD_READ_STATUS = 0x83,
  CMD_READ_ID = 0x85,
  CMD_CLEAR_STATUS = 0x89,
  CMD_SECTOR_ERASE = 0xF1,
  CMD_PAGE_PROGRAM = 0xF2,
  CMD_CHIP_ERASE = 0xF4,
};

enum : u8 {
  STATUS_BUSY = 0x80,
  STATUS_UNLOCKED = 0x40,
  STATUS_ERASE_ERROR = 0x10,
  STATUS_PROGRAM_ERROR = 0x08,
  STATUS_READY = 0x01,
};

constexpr u32 CARD_ID = 0xC221;  // Nintendo brand card
constexpr s64 COMMAND_DELAY = 5000;

// 16-bit big-endian word sums used by the card file system.
void Checksums(const u8* data, u32 size, u16& sum, u16& inv) {
  sum = 0;
  inv = 0;
  for (u32 i = 0; i < size; i += 2) {
    u16 w = LoadBE16(data + i);
    sum += w;
    inv += (u16)(w ^ 0xFFFF);
  }
  if (sum == 0xFFFF) sum = 0;
  if (inv == 0xFFFF) inv = 0;
}

}  // namespace

void MemoryCard::Format(const u8* flash_id) {
  memset(m_data, 0xFF, SIZE_BYTES);
  const u32 B = BLOCK_SIZE;

  // Block 0: header. The serial mixes the console's flash ID with a PRNG seeded
  // by the format time, exactly as the SDK does (it verifies this on mount).
  u8* h = m_data;
  u64 format_time = (u64)(time(nullptr) - 946684800) * (BUS_CLOCK / 4);
  u64 rnd = format_time;
  for (int i = 0; i < 12; i++) {
    rnd = ((rnd * 0x41C64E6Dull) + 0x3039ull) >> 16;
    h[i] = (u8)(flash_id[i] + (u32)rnd);
    rnd = ((rnd * 0x41C64E6Dull) + 0x3039ull) >> 16;
    rnd &= 0x7FFF;
  }
  StoreBE64(h + 0x0C, format_time);
  StoreBE32(h + 0x14, 0);           // SRAM RTC bias
  StoreBE32(h + 0x18, 0);           // SRAM language
  StoreBE32(h + 0x1C, 0);           // DTV status
  StoreBE16(h + 0x20, 0);           // device ID
  StoreBE16(h + 0x22, SIZE_MBITS);  // size in Mbits
  StoreBE16(h + 0x24, 0);           // encoding: ANSI
  u16 sum, inv;
  Checksums(h, 0x1FC, sum, inv);
  StoreBE16(h + 0x1FC, sum);
  StoreBE16(h + 0x1FE, inv);

  // Blocks 1-2: directory (127 empty entries), two copies.
  for (int copy = 0; copy < 2; copy++) {
    u8* d = m_data + B * (1 + copy);
    StoreBE16(d + 0x1FFA, 0);  // update counter
    Checksums(d, 0x1FFC, sum, inv);
    StoreBE16(d + 0x1FFC, sum);
    StoreBE16(d + 0x1FFE, inv);
  }
  // Blocks 3-4: block allocation table, two copies.
  for (int copy = 0; copy < 2; copy++) {
    u8* b = m_data + B * (3 + copy);
    memset(b, 0, B);
    StoreBE16(b + 4, 0);                                        // update counter
    StoreBE16(b + 6, (u16)(SIZE_BYTES / B - 5));                // free blocks
    StoreBE16(b + 8, 4);                                        // last allocated block
    Checksums(b + 4, B - 4, sum, inv);
    StoreBE16(b + 0, sum);
    StoreBE16(b + 2, inv);
  }
}

bool MemoryCard::Open(const std::string& path, const u8* flash_id) {
  Close();
  m_data = (u8*)malloc(SIZE_BYTES);
  if (!m_data) return false;
  m_status = STATUS_BUSY | STATUS_UNLOCKED | STATUS_READY;
  m_file = fopen(path.c_str(), "r+b");
  if (m_file && fread(m_data, 1, SIZE_BYTES, m_file) == SIZE_BYTES) {
    LOG("Memcard: loaded %s\n", path.c_str());
    return true;
  }
  if (m_file) fclose(m_file);
  Format(flash_id);
  m_file = fopen(path.c_str(), "w+b");
  if (m_file) {
    fwrite(m_data, 1, SIZE_BYTES, m_file);
    fflush(m_file);
    LOG("Memcard: created formatted card %s\n", path.c_str());
  } else {
    LOG("Memcard: cannot write %s, card changes will not be saved\n", path.c_str());
  }
  return true;
}

void MemoryCard::Close() {
  if (m_file) fclose(m_file);
  m_file = nullptr;
  free(m_data);
  m_data = nullptr;
}

void MemoryCard::Flush(u32 offset, u32 len) {
  if (!m_file) return;
  fseek(m_file, (long)offset, SEEK_SET);
  fwrite(m_data + offset, 1, len, m_file);
  fflush(m_file);
}

void MemoryCard::Select(bool selected) {
  if (selected) {
    m_position = 0;
    return;
  }
  // Commands that act when chip select is released
  switch (m_command) {
    case CMD_SECTOR_ERASE:
      if (m_position > 2) {
        u32 sector = m_address & (SIZE_BYTES - 1) & ~(BLOCK_SIZE - 1);
        memset(m_data + sector, 0xFF, BLOCK_SIZE);
        Flush(sector, BLOCK_SIZE);
        m_status |= STATUS_BUSY;
        m_status &= ~STATUS_READY;
        m_pending_delay = COMMAND_DELAY;
      }
      break;
    case CMD_CHIP_ERASE:
      if (m_position > 2) {
        memset(m_data, 0xFF, SIZE_BYTES);
        Flush(0, SIZE_BYTES);
        m_status &= ~STATUS_BUSY;
      }
      break;
    case CMD_PAGE_PROGRAM:
      if (m_position >= 5) {
        u32 count = m_position - 5, i = 0;
        u32 start = m_address & (SIZE_BYTES - 1);
        m_status &= ~STATUS_BUSY;
        while (count--) {
          m_data[m_address & (SIZE_BYTES - 1)] = m_program[i++];
          i &= 127;
          m_address = (m_address & ~0x1FFu) | ((m_address + 1) & 0x1FF);
        }
        Flush(start & ~0x1FFu, 0x200);
        m_pending_delay = COMMAND_DELAY;
      }
      break;
    default: break;
  }
}

u8 MemoryCard::TransferByte(u8 in) {
  u8 out = 0xFF;
  if (m_position == 0) {
    m_command = in;
    if (m_command == CMD_CLEAR_STATUS) {
      m_status &= ~(STATUS_PROGRAM_ERROR | STATUS_ERASE_ERROR);
      m_status |= STATUS_READY;
      m_interrupt = false;
      m_position = 0;
      return 0xFF;
    }
  } else {
    switch (m_command) {
      case CMD_NINTENDO_ID:
        if (m_position == 1)
          out = 0x80;
        else
          out = (u8)(SIZE_MBITS >> (24 - (((m_position - 2) & 3) * 8)));
        break;
      case CMD_READ_ARRAY:
        switch (m_position) {
          case 1: m_address = (u32)in << 17; break;
          case 2: m_address |= (u32)in << 9; break;
          case 3: m_address |= (u32)(in & 3) << 7; break;
          case 4: m_address |= in & 0x7F; break;
          default: break;
        }
        if (m_position > 1) {
          out = m_data[m_address & (SIZE_BYTES - 1)];
          if (m_position >= 9) m_address = (m_address & ~0x1FFu) | ((m_address + 1) & 0x1FF);
        }
        break;
      case CMD_READ_STATUS: out = m_status; break;
      case CMD_READ_ID:
        out = (m_position == 1) ? (u8)(CARD_ID >> 8) : (u8)((m_position & 1) ? CARD_ID : (CARD_ID >> 8));
        break;
      case CMD_SECTOR_ERASE:
        if (m_position == 1) m_address = (u32)in << 17;
        if (m_position == 2) m_address |= (u32)in << 9;
        break;
      case CMD_SET_INTERRUPT:
        if (m_position == 1) m_interrupt_switch = in;
        break;
      case CMD_PAGE_PROGRAM:
        switch (m_position) {
          case 1: m_address = (u32)in << 17; break;
          case 2: m_address |= (u32)in << 9; break;
          case 3: m_address |= (u32)(in & 3) << 7; break;
          case 4: m_address |= in & 0x7F; break;
          default: m_program[(m_position - 5) & 0x7F] = in; break;
        }
        break;
      default: break;
    }
  }
  m_position++;
  return out;
}

void MemoryCard::DoState(StateBuffer& s) {
  s.Marker("Memcard");
  s.Do(m_position);
  s.Do(m_command);
  s.Do(m_address);
  s.Do(m_status);
  s.Do(m_interrupt_switch);
  s.Do(m_interrupt);
  s.Do(m_pending_delay);
  s.Do(m_program);
}

void MemoryCard::CommandDone() {
  m_status |= STATUS_READY;
  m_status &= ~STATUS_BUSY;
  m_interrupt = true;
}

bool MemoryCard::TakeInterrupt() {
  if (!m_interrupt || !m_interrupt_switch) return false;
  m_interrupt = false;
  return true;
}

s64 MemoryCard::TakePendingDelay() {
  s64 d = m_pending_delay;
  m_pending_delay = 0;
  return d;
}
