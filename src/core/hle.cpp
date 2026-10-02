// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/hle.h"

#include <algorithm>
#include <string>
#include <vector>

#include "core/gekko/cpu.h"
#include "core/memory.h"

namespace HLE {

namespace {
struct Hook {
  u32 addr;
  u32 original;
  Handler handler;
  const char* name;
};
std::vector<Hook> s_hooks;

std::string ReadGuestString(u32 addr, size_t max = 1024) {
  std::string s;
  u32 pa;
  while (s.size() < max && Mem::TranslateData(addr, pa)) {
    u8* p = Mem::PhysPtr(pa, 1);
    if (!p || !*p) break;
    s += (char)*p;
    addr++;
  }
  return s;
}
}  // namespace

void Clear() { s_hooks.clear(); }

bool Patch(u32 addr, Handler handler, const char* name) {
  u8* p = Mem::PhysPtr(addr & 0x01FFFFFF, 4);
  if (!p) return false;
  Hook h{addr, LoadBE32(p), handler, name};
  StoreBE32(p, (1u << 26) | (u32)s_hooks.size());
  s_hooks.push_back(h);
  LOG("HLE: hooked %s at %08x\n", name, addr);
  return true;
}

void Execute(u32 inst) {
  u32 index = inst & 0x03FFFFFF;
  if (index >= s_hooks.size()) {
    LOG("HLE: bad hook index %u at %08x\n", index, cpu.pc);
    return;
  }
  s_hooks[index].handler();
  cpu.npc = LR;  // behave like the function returned
}

void OSReport() {
  std::string fmt = ReadGuestString(cpu.gpr[3]);
  std::string out;
  u32 gpr = 4, fpr = 1;
  auto next32 = [&]() -> u32 { return gpr <= 10 ? cpu.gpr[gpr++] : 0; };
  auto next64 = [&]() -> u64 {
    if (!(gpr & 1)) gpr++;  // 64-bit values use an odd/even register pair
    u64 hi = next32(), lo = next32();
    return (hi << 32) | lo;
  };
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    // Copy the conversion spec, e.g. "%-08lx", and format it on the host.
    size_t start = i++;
    while (i < fmt.size() && strchr("-+ #0123456789.", fmt[i])) i++;
    int longs = 0;
    while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h')) longs += fmt[i++] == 'l';
    if (i >= fmt.size()) break;
    char conv = fmt[i];
    std::string spec = fmt.substr(start, i - start);
    spec.erase(std::remove_if(spec.begin(), spec.end(), [](char c) { return c == 'l' || c == 'h'; }),
               spec.end());
    char buf[512];
    switch (conv) {
      case 'd': case 'i':
        if (longs >= 2) snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)next64());
        else snprintf(buf, sizeof(buf), (spec + "d").c_str(), (int)next32());
        break;
      case 'u': case 'x': case 'X': case 'o':
        if (longs >= 2) snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)next64());
        else snprintf(buf, sizeof(buf), (spec + conv).c_str(), next32());
        break;
      case 'p': snprintf(buf, sizeof(buf), "0x%08x", next32()); break;
      case 'c': snprintf(buf, sizeof(buf), (spec + "c").c_str(), (int)next32()); break;
      case 's': snprintf(buf, sizeof(buf), (spec + "s").c_str(), ReadGuestString(next32()).c_str()); break;
      case 'f': case 'e': case 'g': case 'F': case 'E': case 'G':
        snprintf(buf, sizeof(buf), (spec + conv).c_str(), fpr <= 8 ? cpu.fpr[fpr++].d0() : 0.0);
        break;
      case '%': snprintf(buf, sizeof(buf), "%%"); break;
      default: snprintf(buf, sizeof(buf), "%s%c", spec.c_str(), conv); break;
    }
    out += buf;
  }
  LOG("[OSReport] %s%s", out.c_str(), (!out.empty() && out.back() == '\n') ? "" : "\n");
}

}  // namespace HLE
