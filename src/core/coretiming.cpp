// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/coretiming.h"
#include "core/prof.h"

#include <vector>

#include "core/gekko/cpu.h"
#include "core/state.h"

namespace CoreTiming {

u64 g_slice_end = 0;

namespace {
constexpr s64 MAX_SLICE = 20000;

struct EventType {
  const char* name;
  Callback cb;
};

struct Event {
  u64 time;
  int type;
  u64 userdata;
};

std::vector<EventType> s_types;
std::vector<Event> s_events;  // kept sorted by time, earliest first
}  // namespace

void Init() {
  s_events.clear();
  g_slice_end = 0;
}

void DoState(StateBuffer& s) {
  s.Marker("CoreTiming");
  s.Do(s_events);
  s.Do(g_slice_end);
}

int RegisterEvent(const char* name, Callback cb) {
  s_types.push_back({name, cb});
  return (int)s_types.size() - 1;
}

u64 GetTicks() { return cpu.cycles; }

void ScheduleEvent(int type, s64 cycles_into_future, u64 userdata) {
  if (cycles_into_future < 0) cycles_into_future = 0;
  Event ev{cpu.cycles + (u64)cycles_into_future, type, userdata};
  auto it = s_events.begin();
  while (it != s_events.end() && it->time <= ev.time) ++it;
  s_events.insert(it, ev);
  // Cut the current CPU slice short if this event is due earlier.
  if (ev.time < g_slice_end) g_slice_end = ev.time;
}

void RemoveEvent(int type) {
  for (auto it = s_events.begin(); it != s_events.end();) {
    if (it->type == type)
      it = s_events.erase(it);
    else
      ++it;
  }
}

void Advance() {
  u64 target = cpu.cycles + MAX_SLICE;
  if (!s_events.empty() && s_events.front().time < target) target = s_events.front().time;
  g_slice_end = target;
  {
    Prof::Scope prof(Prof::CPU);
    CPU::Run();
  }

  while (!s_events.empty() && s_events.front().time <= cpu.cycles) {
    Event ev = s_events.front();
    s_events.erase(s_events.begin());
    s_types[ev.type].cb(ev.userdata, (s64)(cpu.cycles - ev.time));
  }
}

}  // namespace CoreTiming
