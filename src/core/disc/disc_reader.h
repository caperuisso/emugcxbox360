// emugcxbox360 - disc image readers (raw ISO/GCM, WIA, RVZ)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>

#include "core/common.h"

class DiscReader {
 public:
  virtual ~DiscReader() = default;
  // Size of the uncompressed disc.
  virtual u64 Size() const = 0;
  // Reads uncompressed disc bytes; false on I/O or decoding errors.
  virtual bool Read(u64 offset, void* dst, u64 len) = 0;
  virtual const char* FormatName() const = 0;
};

// Picks the right reader from the file's magic number. On failure returns null
// and, if error is given, a human readable reason.
std::unique_ptr<DiscReader> OpenDiscImage(const std::string& path, std::string* error = nullptr);

// True for the file extensions handled by OpenDiscImage (.iso .gcm .rvz .wia).
bool IsDiscImagePath(const std::string& path);

// Implemented in wia_reader.cpp
std::unique_ptr<DiscReader> OpenWIAOrRVZ(FILE* file, std::string* error);
