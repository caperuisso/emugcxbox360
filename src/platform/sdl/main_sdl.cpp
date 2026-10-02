// emugcxbox360 - SDL2 frontend for Linux/PC (development and testing host)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/gekko/cpu.h"
#include "core/system.h"

namespace {

class SDLHost : public Host {
 public:
  bool headless = false;
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Texture* texture = nullptr;
  int tex_w = 0, tex_h = 0;
  SDL_GameController* pad = nullptr;
  SDL_AudioDeviceID audio = 0;
  int audio_rate = 0;
  std::vector<u32> last_frame;
  int last_w = 0, last_h = 0;
  u64 frames_presented = 0;

  bool Open(int scale) {
    if (headless) return true;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
      fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
      return false;
    }
    window = SDL_CreateWindow("emugcxbox360", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640 * scale,
                              480 * scale, SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!window || !renderer) {
      fprintf(stderr, "SDL window: %s\n", SDL_GetError());
      return false;
    }
    SDL_RenderSetLogicalSize(renderer, 640, 480);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
      if (SDL_IsGameController(i) && (pad = SDL_GameControllerOpen(i))) break;
    return true;
  }

  void Close() {
    if (pad) SDL_GameControllerClose(pad);
    if (audio) SDL_CloseAudioDevice(audio);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (!headless) SDL_Quit();
  }

  void Log(const char* msg) override { fputs(msg, stdout); fflush(stdout); }

  void PresentFrame(const u32* argb, int w, int h) override {
    frames_presented++;
    last_frame.assign(argb, argb + (size_t)w * h);
    last_w = w;
    last_h = h;
    if (headless) return;
    if (!texture || tex_w != w || tex_h != h) {
      if (texture) SDL_DestroyTexture(texture);
      texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
      tex_w = w;
      tex_h = h;
    }
    SDL_UpdateTexture(texture, nullptr, argb, w * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
  }

  void PollPad(int port, PadState& out) override {
    if (port != 0) return;  // only port 1 is wired for now
    out.connected = true;
    if (headless) return;
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    auto key = [&](SDL_Scancode s) { return k[s] != 0; };
    if (key(SDL_SCANCODE_X)) out.buttons |= PAD_A;
    if (key(SDL_SCANCODE_Z)) out.buttons |= PAD_B;
    if (key(SDL_SCANCODE_C)) out.buttons |= PAD_X;
    if (key(SDL_SCANCODE_S)) out.buttons |= PAD_Y;
    if (key(SDL_SCANCODE_RETURN)) out.buttons |= PAD_START;
    if (key(SDL_SCANCODE_D)) out.buttons |= PAD_Z;
    if (key(SDL_SCANCODE_Q)) { out.buttons |= PAD_L; out.trigger_l = 0xFF; }
    if (key(SDL_SCANCODE_W)) { out.buttons |= PAD_R; out.trigger_r = 0xFF; }
    if (key(SDL_SCANCODE_T)) out.buttons |= PAD_UP;
    if (key(SDL_SCANCODE_G)) out.buttons |= PAD_DOWN;
    if (key(SDL_SCANCODE_F)) out.buttons |= PAD_LEFT;
    if (key(SDL_SCANCODE_H)) out.buttons |= PAD_RIGHT;
    if (key(SDL_SCANCODE_LEFT)) out.stick_x = 0x20;
    if (key(SDL_SCANCODE_RIGHT)) out.stick_x = 0xE0;
    if (key(SDL_SCANCODE_UP)) out.stick_y = 0xE0;
    if (key(SDL_SCANCODE_DOWN)) out.stick_y = 0x20;
    if (key(SDL_SCANCODE_J)) out.cstick_x = 0x20;
    if (key(SDL_SCANCODE_L)) out.cstick_x = 0xE0;
    if (key(SDL_SCANCODE_I)) out.cstick_y = 0xE0;
    if (key(SDL_SCANCODE_K)) out.cstick_y = 0x20;

    if (pad) {
      auto b = [&](SDL_GameControllerButton btn) { return SDL_GameControllerGetButton(pad, btn) != 0; };
      auto ax = [&](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(pad, a); };
      auto to_u8 = [](int v, bool invert) {
        int r = 128 + (invert ? -v : v) / 256;
        return (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
      };
      // Xbox layout -> GameCube: A=A, X=B, B=X, Y=Y, RB=Z
      if (b(SDL_CONTROLLER_BUTTON_A)) out.buttons |= PAD_A;
      if (b(SDL_CONTROLLER_BUTTON_X)) out.buttons |= PAD_B;
      if (b(SDL_CONTROLLER_BUTTON_B)) out.buttons |= PAD_X;
      if (b(SDL_CONTROLLER_BUTTON_Y)) out.buttons |= PAD_Y;
      if (b(SDL_CONTROLLER_BUTTON_START)) out.buttons |= PAD_START;
      if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) out.buttons |= PAD_Z;
      if (b(SDL_CONTROLLER_BUTTON_DPAD_UP)) out.buttons |= PAD_UP;
      if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) out.buttons |= PAD_DOWN;
      if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) out.buttons |= PAD_LEFT;
      if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) out.buttons |= PAD_RIGHT;
      int lx = ax(SDL_CONTROLLER_AXIS_LEFTX), ly = ax(SDL_CONTROLLER_AXIS_LEFTY);
      int rx = ax(SDL_CONTROLLER_AXIS_RIGHTX), ry = ax(SDL_CONTROLLER_AXIS_RIGHTY);
      if (abs(lx) > 6000) out.stick_x = to_u8(lx, false);
      if (abs(ly) > 6000) out.stick_y = to_u8(ly, true);
      if (abs(rx) > 6000) out.cstick_x = to_u8(rx, false);
      if (abs(ry) > 6000) out.cstick_y = to_u8(ry, true);
      int lt = ax(SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 128, rt = ax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 128;
      if (lt > 8) out.trigger_l = (u8)lt;
      if (rt > 8) out.trigger_r = (u8)rt;
      if (lt > 240) out.buttons |= PAD_L;
      if (rt > 240) out.buttons |= PAD_R;
    }
  }

  void PushAudio(const s16* samples, int frames, int rate) override {
    if (headless) return;
    if (!audio || rate != audio_rate) {
      if (audio) SDL_CloseAudioDevice(audio);
      SDL_AudioSpec want = {};
      want.freq = rate;
      want.format = AUDIO_S16SYS;
      want.channels = 2;
      want.samples = 1024;
      audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
      audio_rate = rate;
      if (audio) SDL_PauseAudioDevice(audio, 0);
    }
    // Drop audio rather than build latency when emulation runs fast.
    if (audio && SDL_GetQueuedAudioSize(audio) < (Uint32)rate)
      SDL_QueueAudio(audio, samples, (Uint32)frames * 4);
  }

  bool HandleEvents() {
    if (headless) return true;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) return false;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
      if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
    }
    return true;
  }

  bool DumpPPM(const char* path) const {
    FILE* f = fopen(path, "wb");
    if (!f || last_w == 0) {
      if (f) fclose(f);
      return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", last_w, last_h);
    for (u32 px : last_frame) {
      u8 rgb[3] = {(u8)(px >> 16), (u8)(px >> 8), (u8)px};
      fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
  }
};

void Usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s [options] <game.dol|game.iso|game.gcm>\n"
          "  --headless        no window (for tests)\n"
          "  --frames N        quit after N fields\n"
          "  --dump FILE.ppm   save the last frame on exit\n"
          "  --cpi N           cycles charged per instruction (default 1)\n"
          "  --unthrottled     do not limit speed to 60 fields/s\n"
          "  --scale N         window scale (default 1)\n",
          argv0);
}

}  // namespace

int main(int argc, char** argv) {
  SDLHost host;
  std::string path, dump;
  long max_frames = -1;
  int scale = 1;
  bool throttle = true;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--headless") host.headless = true;
    else if (a == "--frames" && i + 1 < argc) max_frames = atol(argv[++i]);
    else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
    else if (a == "--cpi" && i + 1 < argc) CPU::g_cycles_per_instruction = (u32)atoi(argv[++i]);
    else if (a == "--unthrottled") throttle = false;
    else if (a == "--scale" && i + 1 < argc) scale = atoi(argv[++i]);
    else if (a[0] == '-') { Usage(argv[0]); return 1; }
    else path = a;
  }
  if (path.empty()) {
    Usage(argv[0]);
    return 1;
  }
  if (host.headless) throttle = false;

  if (!host.Open(scale) || !System::Init(&host)) return 1;
  if (!System::Boot(path)) {
    fprintf(stderr, "Failed to boot %s\n", path.c_str());
    host.Close();
    return 1;
  }

  using clock = std::chrono::steady_clock;
  auto next = clock::now();
  const auto field = std::chrono::microseconds(16683);
  auto fps_start = clock::now();
  long frames = 0, fps_frames = 0;
  while (host.HandleEvents()) {
    System::RunFrame();
    frames++;
    fps_frames++;
    if (max_frames >= 0 && frames >= max_frames) break;
    auto now = clock::now();
    if (throttle) {
      next += field;
      if (next > now) std::this_thread::sleep_for(next - now);
      else if (now - next > std::chrono::milliseconds(100)) next = now;
    }
    if (now - fps_start >= std::chrono::seconds(1) && host.window) {
      char title[128];
      snprintf(title, sizeof(title), "emugcxbox360 - %ld fields/s - pc %08x", fps_frames, cpu.pc);
      SDL_SetWindowTitle(host.window, title);
      fps_frames = 0;
      fps_start = now;
    }
  }

  printf("Ran %ld fields, %llu presented, pc=%08x\n", frames, (unsigned long long)host.frames_presented, cpu.pc);
  if (!dump.empty()) {
    if (host.DumpPPM(dump.c_str()))
      printf("Saved last frame to %s\n", dump.c_str());
    else
      printf("No frame to save\n");
  }
  System::Shutdown();
  host.Close();
  return 0;
}
