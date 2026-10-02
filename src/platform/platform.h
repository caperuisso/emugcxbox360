// emugcxbox360 - platform abstraction between the portable core and a frontend
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "core/common.h"

// GameCube pad buttons (same bit layout as libogc PAD_BUTTON_*)
enum PadButton : u16 {
  PAD_LEFT = 0x0001,
  PAD_RIGHT = 0x0002,
  PAD_DOWN = 0x0004,
  PAD_UP = 0x0008,
  PAD_Z = 0x0010,
  PAD_R = 0x0020,
  PAD_L = 0x0040,
  PAD_A = 0x0100,
  PAD_B = 0x0200,
  PAD_X = 0x0400,
  PAD_Y = 0x0800,
  PAD_START = 0x1000,
};

struct PadState {
  bool connected = false;
  u16 buttons = 0;
  u8 stick_x = 0x80, stick_y = 0x80;
  u8 cstick_x = 0x80, cstick_y = 0x80;
  u8 trigger_l = 0, trigger_r = 0;
};

// Implemented by each frontend (SDL on Linux, libxenon on Xbox 360).
class Host {
 public:
  virtual ~Host() = default;
  virtual void Log(const char* msg) = 0;
  // argb: width*height pixels, 0xAARRGGBB
  virtual void PresentFrame(const u32* argb, int width, int height) = 0;
  virtual void PollPad(int port, PadState& out) = 0;
  // Interleaved stereo s16 samples (host endianness)
  virtual void PushAudio(const s16* samples, int frames, int rate) { (void)samples; (void)frames; (void)rate; }
  // Parallel work for the renderer: runs fn(task, tasks, ctx) for every task
  // in [0, tasks) and returns when all are done. Default: sequential.
  virtual int ParallelWorkers() { return 1; }
  virtual void RunParallel(int tasks, void (*fn)(int task, int tasks, void* ctx), void* ctx) {
    for (int i = 0; i < tasks; i++) fn(i, tasks, ctx);
  }
  // Raw memory card image for slot 0 (A) / 1 (B); empty string = no card.
  virtual std::string MemcardPath(int slot) { (void)slot; return std::string(); }
};

extern Host* g_host;
