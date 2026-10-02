// emugcxbox360 - Xbox 360 frontend (libxenon, boots from XeLL)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <dirent.h>

#include <algorithm>
#include <malloc.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <console/console.h>
#include <diskio/ata.h>
#include <input/input.h>
#include <libfat/fat.h>
#include <ppc/cache.h>
#include <ppc/timebase.h>
#include <time/time.h>
#include <usb/usbmain.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_sound/sound.h>
#include <xenos/xenos.h>
int bdev_enum(int handle, const char** name);
}

#include "core/memory.h"
#include "core/system.h"

namespace {

// ---- Renderer workers on the Xenon's secondary hardware threads ----
// Hardware threads 1..5 each run a spin loop waiting for jobs. Untested on
// hardware so far: disabled unless chosen in the game picker.
struct XenonJob {
  volatile u32 generation;
  volatile int tasks;
  volatile int next;
  volatile int done;
  void (*fn)(int, int, void*);
  void* ctx;
};
XenonJob g_job;
int g_worker_count = 1;  // including the main thread

void RunJobTasks() {
  for (;;) {
    int t = __sync_fetch_and_add(&g_job.next, 1);
    if (t >= g_job.tasks) break;
    g_job.fn(t, g_job.tasks, g_job.ctx);
    __sync_fetch_and_add(&g_job.done, 1);
  }
}

void WorkerLoop() {
  u32 seen = 0;
  for (;;) {
    while (g_job.generation == seen) asm volatile("or 1,1,1");  // low priority spin
    seen = g_job.generation;
    __sync_synchronize();
    RunJobTasks();
  }
}

bool StartWorkers() {
  xenon_thread_startup();
  const int stack_size = 256 * 1024;
  for (int t = 1; t <= 5; t++) {
    u8* stack = (u8*)memalign(128, stack_size);
    if (!stack || xenon_run_thread_task(t, stack + stack_size - 256, (void*)WorkerLoop) != 0) break;
    g_worker_count = t + 1;
  }
  printf("Renderer threads: %d\n", g_worker_count);
  return g_worker_count > 1;
}

// ---- Code integrity diagnostics ----
// Keeps a copy of the program's code and, at each boot checkpoint, reports
// whether anything has overwritten it (and stops so the screen can be read).
extern "C" u32 low_hole_start[], low_hole_end[], _text_end[];  // unused low memory, code
u32* g_text_copy = nullptr;
u32 g_text_words = 0;

void SnapshotText() {
  g_text_words = (u32)(_text_end - low_hole_end);
  g_text_copy = (u32*)malloc(g_text_words * 4);
  if (g_text_copy) memcpy(g_text_copy, low_hole_end, g_text_words * 4);
}

// Compares the code with the startup copy. Overwritten words are reported and
// restored (with the caches flushed) so the emulator keeps running; the report
// tells which operation preceded the damage.
u32 g_repairs = 0;

// The hole below the code is never used: report whatever lands there.
void WatchHole(const char* where) {
  static u32 reports = 0;
  for (u32* w = low_hole_start; w < low_hole_end; w++) {
    if (LIKELY(*w == 0)) continue;
    if (reports++ < 16)
      printf("[guard] low memory %p written after '%s' (value %08x)\n", (void*)w, where, (unsigned)*w);
    *w = 0;
  }
}

bool GuardText(const char* where) {
  WatchHole(where);
  if (!g_text_copy) return true;
  bool ok = true;
  for (u32 i = 0; i < g_text_words; i++) {
    if (LIKELY(low_hole_end[i] == g_text_copy[i])) continue;
    u32 first = i, count = 0;
    u32 bad = low_hole_end[i];
    for (; i < g_text_words; i++) {
      if (low_hole_end[i] == g_text_copy[i]) {
        if (i - first > 64) break;  // end of this damaged run
        continue;
      }
      count++;
      u32* w = &low_hole_end[i];
      *w = g_text_copy[i];
      asm volatile("dcbst 0,%0; sync; icbi 0,%0; sync; isync" ::"r"(w) : "memory");
    }
    if (g_repairs++ < 16)
      printf("[guard] code at %p overwritten after '%s' (%08x -> %08x, %u words): repaired\n",
             (void*)&low_hole_end[first], where, (unsigned)g_text_copy[first], (unsigned)bad, (unsigned)count);
    ok = false;
  }
  return ok;
}

void CheckText(const char* where) {
  if (GuardText(where)) printf("[check] %s: code OK\n", where);
}

// Layout of the Xenos scan-out registers (see libxenon console.c).
struct ATIInfo {
  uint32_t unknown1[4];
  uint32_t base;
  uint32_t unknown2[8];
  uint32_t width;
  uint32_t height;
} __attribute__((__packed__));

class XenonHost : public Host {
 public:
  uint32_t* fb = nullptr;
  int fb_w = 0, fb_h = 0;          // visible size
  int fb_pitch_w = 0;              // width rounded up to 32-pixel tiles
  std::vector<int> x_map;
  int map_src_w = 0, map_dst_w = 0;

  void Attach() {
    volatile ATIInfo* ai = (volatile ATIInfo*)0xEC806100ULL;
    fb = (uint32_t*)(long)(ai->base | 0x80000000);
    fb_w = (int)ai->width;
    fb_h = (int)ai->height;
    fb_pitch_w = ((fb_w + 31) >> 5) << 5;
  }

  // Xenos framebuffers are tiled in 32x32 blocks (formula from libxenon console.c).
  inline void PutPixel(int x, int y, uint32_t bgrx) {
    int idx = (((y >> 5) * 32 * fb_pitch_w + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2) +
                (((x & 31) >> 2) << 3) + (((y & 31) >> 1) << 6)) ^
               ((y & 8) << 2));
    fb[idx] = bgrx;
  }

  void Log(const char* msg) override { printf("%s", msg); }
  void Checkpoint(const char* where) override { CheckText(where); }
  void CodeGuard(const char* where) override { GuardText(where); }

  int ParallelWorkers() override { return g_worker_count; }
  void RunParallel(int tasks, void (*fn)(int, int, void*), void* ctx) override {
    g_job.fn = fn;
    g_job.ctx = ctx;
    g_job.tasks = tasks;
    g_job.next = 0;
    g_job.done = 0;
    __sync_synchronize();
    g_job.generation++;
    RunJobTasks();
    while (g_job.done < tasks) asm volatile("or 1,1,1");
    __sync_synchronize();
  }

  // ---- Performance overlay (tiny 3x5 font, drawn over the picture) ----
  char overlay[32] = "";

  void DrawOverlay() {
    static const char kChars[] = "0123456789.% FPSD";
    static const u16 kGlyphs[] = {  // 3x5 bitmaps, row-major, 15 bits
        0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF,
        0x0002, 0x52A5, 0x0000, 0x79E4, 0x7BE4, 0x79CF, 0x6B6E};
    const int scale = 3, ox = 16, oy = 16;
    int len = (int)strlen(overlay);
    for (int y = oy - 4; y < oy + 5 * scale + 4; y++)
      for (int x = ox - 4; x < ox + len * 4 * scale + 4; x++) PutPixel(x, y, 0);
    for (int c = 0; c < len; c++) {
      const char* pos = strchr(kChars, overlay[c]);
      if (!pos) continue;
      u16 g = kGlyphs[pos - kChars];
      for (int r = 0; r < 5; r++)
        for (int col = 0; col < 3; col++) {
          if (!(g & (1 << (14 - (r * 3 + col))))) continue;
          for (int dy = 0; dy < scale; dy++)
            for (int dx = 0; dx < scale; dx++)
              PutPixel(ox + (c * 4 + col) * scale + dx, oy + r * scale + dy, 0xFFFFFF00);
        }
    }
  }

  std::string memcard_dir;  // e.g. "uda0:/gc/"
  std::string MemcardPath(int slot) override {
    if (slot != 0 || memcard_dir.empty()) return std::string();
    return memcard_dir + "memcard_a.raw";
  }

  void PresentFrame(const u32* argb, int w, int h) override {
    if (!fb || w <= 0 || h <= 0) return;
    // Fit the image to the screen height, keeping the 4:3 aspect, centred.
    int dst_h = fb_h;
    int dst_w = dst_h * 4 / 3;
    if (dst_w > fb_w) {
      dst_w = fb_w;
      dst_h = dst_w * 3 / 4;
    }
    int off_x = (fb_w - dst_w) / 2, off_y = (fb_h - dst_h) / 2;
    if (map_src_w != w || map_dst_w != dst_w) {
      x_map.resize(dst_w);
      for (int x = 0; x < dst_w; x++) x_map[x] = x * w / dst_w;
      map_src_w = w;
      map_dst_w = dst_w;
    }
    for (int y = 0; y < dst_h; y++) {
      const u32* row = argb + (size_t)(y * h / dst_h) * w;
      for (int x = 0; x < dst_w; x++) {
        u32 p = row[x_map[x]];
        // ARGB -> the B,G,R,x byte order console.c uses
        uint32_t bgrx = ((p & 0xFF) << 24) | (((p >> 8) & 0xFF) << 16) | (((p >> 16) & 0xFF) << 8);
        PutPixel(off_x + x, off_y + y, bgrx);
      }
    }
    if (overlay[0]) DrawOverlay();
    memdcbst(fb, fb_pitch_w * (((fb_h + 31) >> 5) << 5) * 4);
  }

  // Audio: the Xenon DAC plays 48 kHz stereo little-endian s16. GameCube audio
  // (32 or 48 kHz) is linearly resampled; when the buffer is full samples are
  // dropped rather than stalling emulation.
  bool sound_ready = false;
  u32 resample_pos = 0;  // 16.16 fixed point position in the input stream
  s16 last_l = 0, last_r = 0;
  std::vector<u8> sound_out;

  void PushAudio(const s16* samples, int frames, int rate) override {
    if (!sound_ready) {
      xenon_sound_init();
      sound_ready = true;
    }
    if (frames <= 0 || rate <= 0) return;
    u32 step = (u32)(((u64)rate << 16) / 48000);
    sound_out.clear();
    while ((resample_pos >> 16) < (u32)frames) {
      u32 idx = resample_pos >> 16, frac = resample_pos & 0xFFFF;
      s16 l0 = idx ? samples[(idx - 1) * 2] : last_l, r0 = idx ? samples[(idx - 1) * 2 + 1] : last_r;
      s16 l1 = samples[idx * 2], r1 = samples[idx * 2 + 1];
      s16 l = (s16)(l0 + (((s32)(l1 - l0) * (s32)frac) >> 16));
      s16 r = (s16)(r0 + (((s32)(r1 - r0) * (s32)frac) >> 16));
      u8 bytes[4] = {(u8)l, (u8)(l >> 8), (u8)r, (u8)(r >> 8)};  // little-endian
      sound_out.insert(sound_out.end(), bytes, bytes + 4);
      resample_pos += step;
    }
    resample_pos -= (u32)frames << 16;
    last_l = samples[(frames - 1) * 2];
    last_r = samples[(frames - 1) * 2 + 1];
    int len = (int)sound_out.size();
    if (len && xenon_sound_get_free() >= len) xenon_sound_submit(sound_out.data(), len);
  }

  void PollPad(int port, PadState& out) override {
    struct controller_data_s c;
    memset(&c, 0, sizeof(c));
    if (!get_controller_data(&c, port)) return;
    out.connected = true;
    auto stick = [](int v) {
      int r = 128 + v / 256;
      return (u8)(r < 0 ? 0 : (r > 255 ? 255 : r));
    };
    // Xbox 360 -> GameCube: A=A, X=B, B=X, Y=Y, RB=Z, Back+Start reserved
    if (c.a) out.buttons |= PAD_A;
    if (c.x) out.buttons |= PAD_B;
    if (c.b) out.buttons |= PAD_X;
    if (c.y) out.buttons |= PAD_Y;
    if (c.start) out.buttons |= PAD_START;
    if (c.rb) out.buttons |= PAD_Z;
    if (c.up) out.buttons |= PAD_UP;
    if (c.down) out.buttons |= PAD_DOWN;
    if (c.left) out.buttons |= PAD_LEFT;
    if (c.right) out.buttons |= PAD_RIGHT;
    out.stick_x = stick(c.s1_x);
    out.stick_y = stick(c.s1_y);
    out.cstick_x = stick(c.s2_x);
    out.cstick_y = stick(c.s2_y);
    out.trigger_l = c.lt;
    out.trigger_r = c.rt;
    if (c.lt > 240) out.buttons |= PAD_L;
    if (c.rt > 240) out.buttons |= PAD_R;
  }
};

bool HasGameExtension(const std::string& name) {
  std::string l = name;
  std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return (char)tolower(ch); });
  auto ends = [&](const char* s) {
    size_t n = strlen(s);
    return l.size() >= n && l.compare(l.size() - n, n, s) == 0;
  };
  return ends(".dol") || ends(".iso") || ends(".gcm") || ends(".rvz") || ends(".wia");
}

void ScanDir(const std::string& dir, std::vector<std::string>& out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (struct dirent* de = readdir(d)) {
    if (de->d_name[0] == '.') continue;
    if (!(de->d_type & DT_DIR) && HasGameExtension(de->d_name)) out.push_back(dir + de->d_name);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
}

// Simple file picker drawn with the libxenon text console.
bool g_use_threads = false;

std::string PickGame() {
  std::vector<std::string> games;
  const char* name = nullptr;
  for (int h = bdev_enum(-1, &name); h >= 0; h = bdev_enum(h, &name)) {
    std::string root = std::string(name) + ":/";
    ScanDir(root, games);
    ScanDir(root + "gc/", games);
  }
  if (games.empty()) {
    printf("\nNo .dol/.iso/.gcm/.rvz found. Put games in the root or in /gc/ of a USB drive.\n");
    return "";
  }

  int sel = 0, shown = -1;
  int repeat = 0;
  for (;;) {
    if (sel != shown) {
      console_clrscr();
      printf("emugcxbox360 - choose a game (A: start, X: multi-thread renderer %s, Guide: quit)\n\n",
             g_use_threads ? "ON" : "OFF");
      int first = std::max(0, sel - 10);
      for (int i = first; i < (int)games.size() && i < first + 20; i++)
        printf("%s %s\n", i == sel ? ">" : " ", games[i].c_str());
      shown = sel;
    }
    usb_do_poll();
    struct controller_data_s c;
    memset(&c, 0, sizeof(c));
    get_controller_data(&c, 0);
    if (c.logo) return "";
    if (c.a) return games[sel];
    static bool x_was_down = false;
    if (c.x && !x_was_down) {
      g_use_threads = !g_use_threads;
      shown = -1;  // redraw
    }
    x_was_down = c.x;
    bool down = c.down || c.s1_y < -20000, up = c.up || c.s1_y > 20000;
    if ((down || up) && repeat-- <= 0) {
      sel = std::clamp(sel + (down ? 1 : -1), 0, (int)games.size() - 1);
      repeat = 8;
    } else if (!down && !up) {
      repeat = 0;
    }
    mdelay(16);
  }
}

}  // namespace

// libxenon's startup only runs the legacy .ctors list; run .init_array ourselves
// (see app.lds) so C++ global constructors work like on the PC build.
extern "C" {
typedef void (*InitFunc)();
extern InitFunc __init_array_start[];
extern InitFunc __init_array_end[];
}

static void RunGlobalConstructors() {
  for (InitFunc* f = __init_array_start; f != __init_array_end; ++f) (*f)();
}

// XeLL leaves the Ethernet controller running: every received packet is still
// DMAed into XeLL's old receive buffers, in low memory where our code now
// lives (it corrupted the code around 0x8003bf00). Stop RX/TX first thing
// (same as libxenon's enet_quiesce, without pulling in lwIP).
void StopXeLLNetwork() {
  *(volatile u32*)0xEA001400 = 0;  // TX queue control
  *(volatile u32*)0xEA001410 = 0;  // RX control
  asm volatile("sync; eieio" ::: "memory");
}

int main() {
  StopXeLLNetwork();
  RunGlobalConstructors();
  SnapshotText();
  xenos_init(VIDEO_MODE_AUTO);
  console_init();
  xenon_make_it_faster(XENON_SPEED_FULL);
  usb_init();
  usb_do_poll();
  xenon_ata_init();
  xenon_atapi_init();
  fatInitDefault();

  printf("emugcxbox360 - GameCube emulator (milestone 1: interpreter, XFB only)\n");

  static XenonHost host;
  CheckText("libxenon init");
  std::string game = PickGame();
  CheckText("game picker");
  if (game.empty()) return 0;

  if (g_use_threads) StartWorkers();
  host.Attach();
  host.memcard_dir = game.substr(0, game.rfind('/') + 1);  // card lives next to the game
  if (!System::Init(&host) || !System::Boot(game)) {
    printf("Failed to boot %s\n", game.c_str());
    for (;;) mdelay(1000);
  }
  console_set_colors(0, 0);
  console_clrscr();  // black screen: no leftover text around the emulated picture
  console_close();   // the emulated picture owns the framebuffer from now on

  u64 stats_start = mftb();
  int stats_fields = 0;
  for (;;) {
    usb_do_poll();
    System::RunFrame();
    GuardText("frame");
    stats_fields++;
    u64 now = mftb();
    if (tb_diff_msec(now, stats_start) >= 1000) {
      // Emulated speed relative to the console's field rate (50 Hz PAL, 60 Hz NTSC).
      unsigned ms = tb_diff_msec(now, stats_start);
      int field_rate = Mem::PhysRead32(0xCC) == 1 ? 50 : 60;
      int fps10 = (int)(stats_fields * 10000u / ms);
      int speed = fps10 * 10 / field_rate;
      snprintf(host.overlay, sizeof(host.overlay), "FPS %d.%d SPD %d%%", fps10 / 10, fps10 % 10, speed);
      printf("[perf] %s\n", host.overlay);  // also on the UART
      stats_start = now;
      stats_fields = 0;
    }
    struct controller_data_s c;
    memset(&c, 0, sizeof(c));
    get_controller_data(&c, 0);
    if (c.logo) break;  // Guide button: back to XeLL
  }
  System::Shutdown();
  return 0;
}
