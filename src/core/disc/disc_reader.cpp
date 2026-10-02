// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/disc/disc_reader.h"

#include <algorithm>
#include <cstdio>

namespace {

class ISOReader : public DiscReader {
 public:
  explicit ISOReader(FILE* f) : m_file(f) {
    fseek(m_file, 0, SEEK_END);
    m_size = (u64)ftell(m_file);
  }
  ~ISOReader() override { fclose(m_file); }

  u64 Size() const override { return m_size; }
  const char* FormatName() const override { return "ISO"; }

  bool Read(u64 offset, void* dst, u64 len) override {
    if (offset + len > m_size) return false;
    if (fseek(m_file, (long)offset, SEEK_SET) != 0) return false;
    return fread(dst, 1, len, m_file) == len;
  }

 private:
  FILE* m_file;
  u64 m_size = 0;
};

void SetError(std::string* error, const std::string& msg) {
  if (error) *error = msg;
}

}  // namespace

bool IsDiscImagePath(const std::string& path) {
  std::string l = path;
  std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)tolower(c); });
  for (const char* ext : {".iso", ".gcm", ".rvz", ".wia"}) {
    size_t n = strlen(ext);
    if (l.size() >= n && l.compare(l.size() - n, n, ext) == 0) return true;
  }
  return false;
}

std::unique_ptr<DiscReader> OpenDiscImage(const std::string& path, std::string* error) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    SetError(error, "cannot open file");
    return nullptr;
  }
  u8 magic[4] = {};
  size_t got = fread(magic, 1, 4, f);
  fseek(f, 0, SEEK_SET);
  if (got == 4 && (memcmp(magic, "WIA\x01", 4) == 0 || memcmp(magic, "RVZ\x01", 4) == 0))
    return OpenWIAOrRVZ(f, error);  // takes ownership of f
  return std::make_unique<ISOReader>(f);
}
