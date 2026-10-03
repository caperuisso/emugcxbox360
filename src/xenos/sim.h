// emugcxbox360 - Xenos shader microcode interpreter and simulated GPU backend
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Executes the microcode produced by the GX translation on the PC, with the
// instruction semantics documented in Xenia's ucode.h, so the GPU path can be
// compared with the software renderer without an Xbox 360.
#pragma once

#include "core/video/gpu_backend.h"

namespace XenosSim {

// Installs a simulated Xenos backend as Video::g_gpu.
void Install();

}  // namespace XenosSim
