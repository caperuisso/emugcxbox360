// emugcxbox360 - executable loading and HLE replacement for the IPL boot
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

#include "core/common.h"

namespace Boot {

// Loads a DOL image into memory; returns the entry point or 0 on error.
u32 LoadDOL(const std::vector<u8>& dol);

// Boots a .dol directly, or a .iso/.gcm through the HLE apploader.
bool BootFile(const std::string& path);

}  // namespace Boot
