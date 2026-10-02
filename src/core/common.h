// emugcxbox360 - GameCube emulator for Xbox 360 (libxenon)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <cstdio>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using f32 = float;
using f64 = double;

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define HOST_BIG_ENDIAN 1
#else
#define HOST_BIG_ENDIAN 0
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

inline u16 Swap16(u16 v) { return __builtin_bswap16(v); }
inline u32 Swap32(u32 v) { return __builtin_bswap32(v); }
inline u64 Swap64(u64 v) { return __builtin_bswap64(v); }

// Guest memory is big-endian. On the Xbox 360 (big-endian host) these are no-ops.
inline u16 BE16(u16 v) { return HOST_BIG_ENDIAN ? v : Swap16(v); }
inline u32 BE32(u32 v) { return HOST_BIG_ENDIAN ? v : Swap32(v); }
inline u64 BE64(u64 v) { return HOST_BIG_ENDIAN ? v : Swap64(v); }

inline u16 LoadBE16(const u8* p) { u16 v; memcpy(&v, p, 2); return BE16(v); }
inline u32 LoadBE32(const u8* p) { u32 v; memcpy(&v, p, 4); return BE32(v); }
inline u64 LoadBE64(const u8* p) { u64 v; memcpy(&v, p, 8); return BE64(v); }
inline void StoreBE16(u8* p, u16 v) { v = BE16(v); memcpy(p, &v, 2); }
inline void StoreBE32(u8* p, u32 v) { v = BE32(v); memcpy(p, &v, 4); }
inline void StoreBE64(u8* p, u64 v) { v = BE64(v); memcpy(p, &v, 8); }

// The register barriers keep integer <-> float conversions in integer
// registers: otherwise GCC fuses BitCast<float>(LoadBE32(p)) into lfs/stfs,
// which raise an alignment exception on the Xenon when p is not aligned.
template <typename To, typename From>
inline To BitCast(const From& f) {
  static_assert(sizeof(To) == sizeof(From), "size mismatch");
  To t;
#if HOST_BIG_ENDIAN
  if constexpr (std::is_integral<From>::value) {
    From v = f;
    __asm__("" : "+r"(v));
    memcpy(&t, &v, sizeof(To));
  } else {
    memcpy(&t, &f, sizeof(To));
  }
  if constexpr (std::is_integral<To>::value) __asm__("" : "+r"(t));
#else
  memcpy(&t, &f, sizeof(To));
#endif
  return t;
}

// Timing constants (GameCube)
constexpr u32 CPU_CLOCK = 486000000;  // Gekko core clock
constexpr u32 BUS_CLOCK = 162000000;  // Flipper bus clock
constexpr u32 TB_DIVIDER = 12;        // timebase ticks = bus/4 = cpu/12

class StateBuffer;  // save states (core/state.h)

// Boot progress markers (used by frontends for diagnostics; no-op by default).
void BootCheckpoint(const char* where);

void LogPrint(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) LogPrint(__VA_ARGS__)
