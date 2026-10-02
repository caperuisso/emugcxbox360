// Builds the GPU backend's shaders on the PC and writes them for
// tools/xenos_disasm.py --raw (big-endian dwords, as in GPU memory).
#include <cstdio>
#include <string>

#include "xenos/shaders.h"

static void Write(const char* path, const std::vector<uint32_t>& code) {
  FILE* f = fopen(path, "wb");
  for (uint32_t w : code) {
    unsigned char b[4] = {(unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8), (unsigned char)w};
    fwrite(b, 1, 4, f);
  }
  fclose(f);
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  Write((dir + "/blit_vs.bin").c_str(), Xenos::BlitVertexShader().code);
  Write((dir + "/blit_ps.bin").c_str(), Xenos::BlitPixelShader().code);
  printf("vs program_control %08x, ps program_control %08x\n", Xenos::BlitVertexShader().program_control,
         Xenos::BlitPixelShader().program_control);
  return 0;
}
