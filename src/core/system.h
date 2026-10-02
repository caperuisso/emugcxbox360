// emugcxbox360 - top level emulator control used by the frontends
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "platform/platform.h"

namespace System {

bool Init(Host* host);
void Shutdown();

// Resets the machine and boots a .dol / .iso / .gcm file.
bool Boot(const std::string& path);

// Emulates until the VI finishes the next field (~1/60 s of guest time).
void RunFrame();

}  // namespace System
