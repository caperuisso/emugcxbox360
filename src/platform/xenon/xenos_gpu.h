// emugcxbox360 - Xenos GPU output for the Xbox 360 frontend
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace XenosGpu {

// Sets up libxenon's Xe device on the scan-out framebuffer. Call after the
// text console has been closed.
bool Init();

// Shows an ARGB image (0xAARRGGBB) scaled to the screen height with a 4:3
// aspect, by drawing it as a textured quad on the GPU.
void Present(const uint32_t* argb, int width, int height, const char* overlay);

// Text drawn over the picture (performance overlay).
void SetOverlay(const char* text);

// Renders GX draws on the GPU (Video::g_gpu). Requires Init().
bool InstallBackend();

}  // namespace XenosGpu
