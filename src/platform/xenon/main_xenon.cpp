// emugcxbox360 - Xbox 360 frontend (libxenon, boots from XeLL)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <dirent.h>

#include <algorithm>
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
#include <time/time.h>
#include <usb/usbmain.h>
#include <xenon_soc/xenon_power.h>
#include <xenos/xenos.h>
int bdev_enum(int handle, const char** name);
}

#include "core/system.h"

namespace {

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
    memdcbst(fb, fb_pitch_w * (((fb_h + 31) >> 5) << 5) * 4);
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
      printf("emugcxbox360 - choose a game (A: start, Guide: quit)\n\n");
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

int main() {
  RunGlobalConstructors();
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
  std::string game = PickGame();
  if (game.empty()) return 0;

  host.Attach();
  if (!System::Init(&host) || !System::Boot(game)) {
    printf("Failed to boot %s\n", game.c_str());
    for (;;) mdelay(1000);
  }
  console_set_colors(0, 0);
  console_clrscr();  // black screen: no leftover text around the emulated picture
  console_close();   // the emulated picture owns the framebuffer from now on

  for (;;) {
    usb_do_poll();
    System::RunFrame();
    struct controller_data_s c;
    memset(&c, 0, sizeof(c));
    get_controller_data(&c, 0);
    if (c.logo) break;  // Guide button: back to XeLL
  }
  System::Shutdown();
  return 0;
}
