// emugcxbox360 - minimal headless host used to compare builds
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Runs a game for N fields without a memory card and prints a hash of every
// presented frame, of the audio and of guest RAM. Built natively and with the
// Xbox 360 compiler (run under qemu-ppc), the two logs must be identical:
// that checks the big-endian code paths without the console.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/memory.h"
#include "core/system.h"
#include "platform/platform.h"
#include "xenos/sim.h"

namespace {

u64 Fnv(const void* data, size_t len, u64 h = 1469598103934665603ull) {
  const u8* p = (const u8*)data;
  for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

class HeadlessHost : public Host {
 public:
  u64 audio_hash = 1469598103934665603ull;
  u64 frame_hash = 0;
  int presented = 0;
  void Log(const char* msg) override { (void)msg; }
  void PresentFrame(const u32* argb, int w, int h) override {
    presented++;
    u64 v = 0;  // computed on pixel values, so it does not depend on host endianness
    for (int i = 0; i < w * h; i++) v = (v ^ argb[i]) * 1099511628211ull;
    frame_hash = v;
    last = argb;
    lw = w, lh = h;
    if (!dump_path.empty()) {
      FILE* f = fopen(dump_path.c_str(), "wb");
      if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; i++) {
          u8 rgb[3] = {(u8)(argb[i] >> 16), (u8)(argb[i] >> 8), (u8)argb[i]};
          fwrite(rgb, 1, 3, f);
        }
        fclose(f);
      }
    }
  }
  void PollPad(int port, PadState& out) override {
    out.connected = port == 0;
    if (port == 0 && frame >= 600 && frame % 120 < 8) out.buttons = PAD_START;  // leave the title screen
  }
  void PushAudio(const s16* samples, int frames, int rate) override {
    for (int i = 0; i < frames * 2; i++) {
      u16 s = (u16)samples[i];
      u8 b[2] = {(u8)s, (u8)(s >> 8)};
      audio_hash = Fnv(b, 2, audio_hash);
    }
    (void)rate;
  }
  std::string MemcardPath(int slot) override { (void)slot; return std::string(); }

  const u32* last = nullptr;
  int lw = 0, lh = 0;
  int frame = 0;
  std::string dump_path;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s GAME FIELDS [LAST.ppm]\n", argv[0]);
    return 1;
  }
  HeadlessHost host;
  const char* gpu = getenv("EMUGC_GPU");
  if (gpu && !strcmp(gpu, "sim")) XenosSim::Install();  // GPU path through the Xenos simulator
  int fields = atoi(argv[2]);
  if (!System::Init(&host) || !System::Boot(argv[1])) {
    printf("boot failed\n");
    return 1;
  }
  for (host.frame = 0; host.frame < fields; host.frame++) {
    if (argc > 3 && host.frame == fields - 1) host.dump_path = argv[3];
    System::RunFrame();
    if (host.frame % 30 == 29)
      printf("field %d: presented %d frame %016llx audio %016llx ram %016llx\n", host.frame + 1, host.presented,
             (unsigned long long)host.frame_hash, (unsigned long long)host.audio_hash,
             (unsigned long long)Fnv(Mem::g_mem1, Mem::MEM1_SIZE));
    fflush(stdout);
  }
  System::Shutdown();
  return 0;
}
