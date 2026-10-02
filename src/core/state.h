// emugcxbox360 - save states: a simple binary serializer shared by every module
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <type_traits>
#include <vector>

#include "core/common.h"

class StateBuffer {
 public:
  enum class Mode { Write, Read };
  explicit StateBuffer(Mode mode) : m_mode(mode) {}

  bool IsReading() const { return m_mode == Mode::Read; }
  bool Ok() const { return m_ok; }
  std::vector<u8>& Data() { return m_data; }

  void DoBytes(void* p, size_t n) {
    if (m_mode == Mode::Write) {
      const u8* b = (const u8*)p;
      m_data.insert(m_data.end(), b, b + n);
    } else {
      if (m_pos + n > m_data.size()) {
        m_ok = false;
        memset(p, 0, n);
        return;
      }
      memcpy(p, m_data.data() + m_pos, n);
      m_pos += n;
    }
  }

  template <typename T>
  void Do(T& v) {
    static_assert(std::is_trivially_copyable<T>::value, "use a specialised Do");
    DoBytes(&v, sizeof(T));
  }

  template <typename T>
  void Do(std::vector<T>& v) {
    static_assert(std::is_trivially_copyable<T>::value, "vector element must be POD");
    u32 n = (u32)v.size();
    Do(n);
    if (IsReading()) v.resize(n);
    if (n) DoBytes(v.data(), n * sizeof(T));
  }

  void Do(std::string& s) {
    u32 n = (u32)s.size();
    Do(n);
    if (IsReading()) s.resize(n);
    if (n) DoBytes(&s[0], n);
  }

  // Section marker: catches modules reading back a different layout.
  void Marker(const char* name) {
    u32 tag = 0;
    for (const char* c = name; *c; c++) tag = tag * 31 + (u8)*c;
    u32 v = tag;
    Do(v);
    if (IsReading() && v != tag) {
      if (m_ok) LOG("State: section '%s' does not match\n", name);
      m_ok = false;
    }
  }

 private:
  Mode m_mode;
  std::vector<u8> m_data;
  size_t m_pos = 0;
  bool m_ok = true;
};
