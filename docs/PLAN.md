# Working plan (performance on the Xbox 360)

Status legend: [x] done, [~] in progress, [ ] to do. Every step is verified on
the PC (reference images, qemu-ppc big-endian runs) before going to the console.

## GPU rendering (Xenos)
- [x] Run-time Xenos microcode builder (`src/xenos/ucode.h`), checked against XDK shaders
- [x] GPU display path (textured quad) — confirmed working on hardware
- [x] Core backend interface (`src/core/video/gpu_backend.h`) and hooks (triangles, clears, lazy EFB read-back)
- [x] GX -> Xenos translation (`src/xenos/gx_draw.*`): TEV pixel shaders (lerp/compare, swaps, konst,
      alpha test, fog, indirect texturing), vertex shader, constants, render states, half-pixel offset
- [x] Xenos microcode simulator + simulated backend (`--gpu-sim`): mean difference to the software
      renderer 0.02-2.6 per channel on the WW title/intro (edges and the missing copy filter)
- [x] Real Xenos backend (libxenon Xe): EFB in EDRAM, texture cache, vertex ring, read-back of
      color and depth (rectangles only), restore after presents — untested on hardware
- [x] XFB copies presented straight from the GPU (no YUV round trip)
- [ ] Mipmaps / LOD bias on the GPU path (level 0 only for now)
- [ ] EFB copies to textures kept on the GPU (avoid the read-back)
- [ ] Vertex transform on the GPU (currently CPU, cheap so far)

## CPU (PowerPC JIT, `src/core/jit/`)
- [x] v1-v2: blocks as host code, inline integer ALU (re-emitted guest instructions)
- [x] v3-v6: inline loads/stores with a RAM fast path, FP arithmetic, psq_l/psq_st (float GQR)
- [x] v7-v8: carries, CR moves, FP moves/merges/compares, mtlr/mtctr (no block split)
- [x] v9: generated dispatcher chaining blocks
- [x] v10-v14: GPR, CR and FPR caching in host registers, block linking, more instructions inline
      (98%+ of executed instructions native)
- [x] v16: fixed the BAT index in the memory fast path and the dispatcher (rotate by 17, not 15):
      every load/store and every unlinked block exit used to fall back to C. Interpreted
      instructions over 30 WW fields: 32M -> 2.0M (left: idle loops, mtmsr, mftb, MMIO)
- [x] `EMUGC_JIT_PROF=1`: counts the instructions still interpreted (with the address region of
      memory slow paths), printed at exit
- Each version verified bit-identical to the interpreter over 300-1500 WW fields under qemu-ppc.
- [ ] Quantized psq types (u8/s16 with scale), remaining interpreted instructions

## Other
- [ ] Save states are host-endian (a PC state does not load on the 360 / PPC build)
- [x] VI line event only on interesting lines; 200k-cycle slices (identical output)
- [x] On-screen profiler (C/G/A/V shares) for hardware measurements
- [x] GPU texture cache capped by memory (EFB copies made a new 1.3 MB texture per frame)
- [x] Rectangle read-backs flush only the tile rows they read (peeks)
- [ ] Audio/DSP and frame pacing costs on the console
- [ ] Measure on hardware: FPS with JIT on/off (Start), GPU rendering on/off (B)
