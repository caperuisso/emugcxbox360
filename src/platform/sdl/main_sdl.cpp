// emugcxbox360 - SDL2 frontend for Linux/PC (development and testing host)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <SDL.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/gekko/block_cache.h"
#include "core/gekko/cpu.h"
#include "core/hle.h"
#include "core/video/video.h"
#include "core/system.h"

namespace {

// Scripted input: "frame:buttons:duration,..." e.g. "1000:start:10,1200:a+down:5"
struct ScriptedPress {
  long frame, duration;
  u16 buttons;
  u8 stick_x = 0x80, stick_y = 0x80;
};

// Buttons: a b x y start z l r up down left right; main stick: sleft sright sup sdown
u16 ParseButtons(const std::string& s, u8* sx = nullptr, u8* sy = nullptr) {
  u16 b = 0;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t end = s.find('+', pos);
    std::string name = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (name == "a") b |= PAD_A;
    else if (name == "b") b |= PAD_B;
    else if (name == "x") b |= PAD_X;
    else if (name == "y") b |= PAD_Y;
    else if (name == "start") b |= PAD_START;
    else if (name == "z") b |= PAD_Z;
    else if (name == "l") b |= PAD_L;
    else if (name == "r") b |= PAD_R;
    else if (name == "up") b |= PAD_UP;
    else if (name == "down") b |= PAD_DOWN;
    else if (name == "left") b |= PAD_LEFT;
    else if (name == "right") b |= PAD_RIGHT;
    else if (name == "sleft" && sx) *sx = 0x10;
    else if (name == "sright" && sx) *sx = 0xF0;
    else if (name == "sup" && sy) *sy = 0xF0;
    else if (name == "sdown" && sy) *sy = 0x10;
    else fprintf(stderr, "unknown button '%s'\n", name.c_str());
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return b;
}

std::vector<ScriptedPress> ParseScript(const std::string& spec) {
  std::vector<ScriptedPress> out;
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t end = spec.find(',', pos);
    std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
    if (c1 != std::string::npos) {
      ScriptedPress p;
      p.frame = atol(item.substr(0, c1).c_str());
      p.buttons = ParseButtons(item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1),
                               &p.stick_x, &p.stick_y);
      p.duration = c2 == std::string::npos ? 5 : atol(item.substr(c2 + 1).c_str());
      out.push_back(p);
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return out;
}

class SDLHost : public Host {
 public:
  std::vector<ScriptedPress> script;
  std::string state_path;
  bool request_save = false, request_load = false;
  long current_frame = 0;
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

  std::string memcard_path;  // empty: default location; "none": no card
  std::string MemcardPath(int slot) override {
    if (slot != 0 || memcard_path == "none") return std::string();
    if (!memcard_path.empty()) return memcard_path;
    const char* home = getenv("HOME");
    std::string dir = std::string(home ? home : ".") + "/.emugcxbox360";
    mkdir(dir.c_str(), 0755);
    return dir + "/memcard_a.raw";
  }

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
    for (const ScriptedPress& p : script)
      if (current_frame >= p.frame && current_frame < p.frame + p.duration) {
        out.buttons |= p.buttons;
        if (p.stick_x != 0x80) out.stick_x = p.stick_x;
        if (p.stick_y != 0x80) out.stick_y = p.stick_y;
      }
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

  FILE* wav = nullptr;
  u32 wav_bytes = 0;
  int wav_rate = 0;

  void WriteWavHeader() {
    u8 h[44];
    memcpy(h, "RIFF", 4);
    u32 v = 36 + wav_bytes;
    memcpy(h + 4, &v, 4);  // little-endian host
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    u16 s = 1; memcpy(h + 20, &s, 2);   // PCM
    s = 2; memcpy(h + 22, &s, 2);       // stereo
    v = (u32)wav_rate; memcpy(h + 24, &v, 4);
    v = (u32)wav_rate * 4; memcpy(h + 28, &v, 4);
    s = 4; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &wav_bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
  }

  void PushAudio(const s16* samples, int frames, int rate) override {
    if (wav) {
      if (!wav_rate) wav_rate = rate;
      fwrite(samples, 4, (size_t)frames, wav);
      wav_bytes += (u32)frames * 4;
    }
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
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F1) request_save = true;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F3) request_load = true;
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
          "usage: %s [options] <game.dol|game.iso|game.gcm|game.rvz>\n"
          "  --headless        no window (for tests)\n"
          "  --frames N        quit after N fields\n"
          "  --dump FILE.ppm   save the last frame on exit\n"
          "  --cpi N           cycles charged per instruction (default 1)\n"
          "  --unthrottled     do not limit speed to 60 fields/s\n"
          "  --scale N         window scale (default 1)\n"
          "  --regs            print CPU registers on exit (debugging)\n"
          "  --interpreter     disable the block cache (plain interpreter)\n"
          "  --osreport ADDR   log calls to the guest OSReport at ADDR (debugging)\n"
          "  --stats           print GPU statistics every 60 fields\n"
          "  --input SPEC      scripted pad input, e.g. 1000:start:10,1300:a:5\n"
          "  --wav FILE        record the audio output to a WAV file\n"
          "  --load-state FILE load a save state right after booting\n"
          "  --save-state N:FILE  save a state after N fields\n"
          "  (keys: F1 save state, F3 load state)\n"
          "  --memcard FILE    memory card image for slot A (default ~/.emugcxbox360/memcard_a.raw, 'none' = no card)\n"
          "  --dump-every N    with --dump, also save a frame every N fields (name_FRAME.ppm)\n",
          argv0);
}

}  // namespace

int main(int argc, char** argv) {
  SDLHost host;
  std::string path, dump;
  long max_frames = -1, dump_every = 0, save_state_at = -1;
  std::string load_state, save_state_path;
  int scale = 1;
  bool throttle = true, dump_regs = false, stats = false;
  std::vector<u32> osreport_addrs;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--headless") host.headless = true;
    else if (a == "--frames" && i + 1 < argc) max_frames = atol(argv[++i]);
    else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
    else if (a == "--cpi" && i + 1 < argc) CPU::g_cycles_per_instruction = (u32)atoi(argv[++i]);
    else if (a == "--unthrottled") throttle = false;
    else if (a == "--scale" && i + 1 < argc) scale = atoi(argv[++i]);
    else if (a == "--regs") dump_regs = true;
    else if (a == "--interpreter") BlockCache::g_enabled = false;
    else if (a == "--stats") stats = true;
    else if (a == "--input" && i + 1 < argc) host.script = ParseScript(argv[++i]);
    else if (a == "--memcard" && i + 1 < argc) host.memcard_path = argv[++i];
    else if (a == "--load-state" && i + 1 < argc) load_state = argv[++i];
    else if (a == "--save-state" && i + 1 < argc) {
      std::string spec = argv[++i];
      size_t c = spec.find(':');
      save_state_at = atol(spec.substr(0, c).c_str());
      save_state_path = c == std::string::npos ? "state.st" : spec.substr(c + 1);
    }
    else if (a == "--wav" && i + 1 < argc) {
      host.wav = fopen(argv[++i], "wb");
      static const u8 placeholder[44] = {};
      if (host.wav) fwrite(placeholder, 1, sizeof(placeholder), host.wav);  // header written at exit
    }
    else if (a == "--dump-every" && i + 1 < argc) dump_every = atol(argv[++i]);
    else if (a == "--osreport" && i + 1 < argc) osreport_addrs.push_back((u32)strtoul(argv[++i], nullptr, 0));
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
  for (u32 addr : osreport_addrs) HLE::Patch(addr, HLE::OSReport, "OSReport");
  if (!load_state.empty() && !System::LoadState(load_state)) {
    host.Close();
    return 1;
  }
  host.state_path = host.MemcardPath(0).empty() ? std::string("quick.st")
                                                : host.MemcardPath(0).substr(0, host.MemcardPath(0).rfind('/') + 1) + "quick.st";

  using clock = std::chrono::steady_clock;
  auto next = clock::now();
  const auto field = std::chrono::microseconds(16683);
  auto fps_start = clock::now();
  long frames = 0, fps_frames = 0;
  while (host.HandleEvents()) {
    host.current_frame = frames;
    System::RunFrame();
    frames++;
    if (dump_every > 0 && !dump.empty() && frames % dump_every == 0) {
      std::string name = dump;
      size_t dot = name.rfind('.');
      name = name.substr(0, dot) + "_" + std::to_string(frames) + (dot == std::string::npos ? "" : name.substr(dot));
      host.DumpPPM(name.c_str());
    }
    fps_frames++;
    if (frames == save_state_at) System::SaveState(save_state_path);
    if (host.request_save) System::SaveState(host.state_path);
    if (host.request_load) System::LoadState(host.state_path);
    host.request_save = host.request_load = false;
    if (stats && frames % 60 == 0) {
      const Video::Stats& s = Video::g_stats;
      printf("[stats] field %ld: prims=%u verts=%u tris=%u pixels=%u efb_copies=%u xfb_copies=%u pc=%08x\n", frames,
             s.primitives, s.vertices, s.triangles, s.pixels, s.efb_copies, s.xfb_copies, cpu.pc);
      memset(&Video::g_stats, 0, sizeof(Video::g_stats));
    }
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
  if (dump_regs) {
    printf("lr=%08x ctr=%08x msr=%08x cr=%08x srr0=%08x srr1=%08x\n", LR, CTR, cpu.msr, cpu.cr,
           cpu.spr[SPR_SRR0], cpu.spr[SPR_SRR1]);
    for (int r = 0; r < 32; r++) printf("r%-2d=%08x%s", r, cpu.gpr[r], (r % 8 == 7) ? "\n" : " ");
  }
  if (!dump.empty()) {
    if (host.DumpPPM(dump.c_str()))
      printf("Saved last frame to %s\n", dump.c_str());
    else
      printf("No frame to save\n");
  }
  if (host.wav) {
    host.WriteWavHeader();
    fclose(host.wav);
    printf("Recorded %u bytes of audio at %d Hz\n", host.wav_bytes, host.wav_rate);
  }
  System::Shutdown();
  host.Close();
  return 0;
}
