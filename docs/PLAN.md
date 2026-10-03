# Working plan (performance on the Xbox 360)

Status legend: [x] done, [~] in progress, [ ] to do. Every step is verified on
the PC (reference images, qemu-ppc big-endian run) before going to the console.

## GPU rendering (Xenos)
- [x] Run-time Xenos microcode builder (`src/xenos/ucode.h`), checked against XDK shaders
- [x] GPU display path (textured quad) — confirmed working on hardware
- [x] Core backend interface (`src/core/video/gpu_backend.h`)
- [ ] Core hooks: triangles, EFB clears, lazy EFB read-back for copies/peeks
- [ ] GX -> Xenos translation (`src/xenos/gx_draw.*`): TEV pixel shaders, vertex shader, constants, render states
- [ ] Xenos microcode simulator + simulated backend on the PC, compared to the software renderer
- [ ] Real Xenos backend (libxenon Xe), EFB in EDRAM, resolves for EFB copies
- [ ] XFB copies presented directly from the GPU (no YUV round trip)

## CPU
- [ ] PPC -> PPC JIT (lockstep-verified against the interpreter under qemu-ppc)
- [ ] Faster memory access paths (fast RAM path, fewer BitCast round trips)

## Other
- [ ] Audio/DSP and frame pacing costs on the console
