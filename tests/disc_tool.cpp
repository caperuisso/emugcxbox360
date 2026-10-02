// emugcxbox360 - disc image inspection: decodes a whole image (ISO/GCM/WIA/RVZ)
// and prints its CRC32 and SHA-1, to compare with Redump; optionally writes an ISO.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <algorithm>
#include <vector>

#include "core/disc/disc_reader.h"

namespace {

u32 g_crc_table[256];

void InitCRC() {
  for (u32 i = 0; i < 256; i++) {
    u32 c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc_table[i] = c;
  }
}

u32 UpdateCRC(u32 crc, const u8* p, size_t n) {
  crc = ~crc;
  while (n--) crc = g_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

class SHA1 {
 public:
  void Update(const u8* p, size_t n) {
    m_total += n;
    while (n) {
      size_t take = std::min(n, 64 - m_fill);
      memcpy(m_block + m_fill, p, take);
      m_fill += take, p += take, n -= take;
      if (m_fill == 64) {
        Transform(m_block);
        m_fill = 0;
      }
    }
  }
  void Final(u8 out[20]) {
    u64 bits = m_total * 8;
    u8 pad = 0x80;
    Update(&pad, 1);
    u8 zero = 0;
    while (m_fill != 56) Update(&zero, 1);
    u8 len[8];
    for (int i = 0; i < 8; i++) len[i] = (u8)(bits >> (56 - 8 * i));
    Update(len, 8);
    for (int i = 0; i < 5; i++) StoreBE32(out + i * 4, m_h[i]);
  }

 private:
  static u32 Rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
  void Transform(const u8* b) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = LoadBE32(b + i * 4);
    for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = m_h[0], bb = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4];
    for (int i = 0; i < 80; i++) {
      u32 f, k;
      if (i < 20) f = (bb & c) | (~bb & d), k = 0x5A827999;
      else if (i < 40) f = bb ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDC;
      else f = bb ^ c ^ d, k = 0xCA62C1D6;
      u32 t = Rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = Rol(bb, 30), bb = a, a = t;
    }
    m_h[0] += a, m_h[1] += bb, m_h[2] += c, m_h[3] += d, m_h[4] += e;
  }
  u32 m_h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  u8 m_block[64];
  size_t m_fill = 0;
  u64 m_total = 0;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <image.iso|.gcm|.rvz|.wia> [out.iso]\n", argv[0]);
    return 1;
  }
  std::string error;
  auto disc = OpenDiscImage(argv[1], &error);
  if (!disc) {
    fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  u8 header[0x40];
  disc->Read(0, header, sizeof(header));
  printf("format: %s\ngame:   %.6s  %.32s\nsize:   %llu bytes\n", disc->FormatName(), (const char*)header,
         (const char*)header + 0x20, (unsigned long long)disc->Size());

  FILE* out = argc > 2 ? fopen(argv[2], "wb") : nullptr;
  InitCRC();
  SHA1 sha;
  u32 crc = 0;
  std::vector<u8> buf(4 << 20);
  for (u64 off = 0; off < disc->Size(); off += buf.size()) {
    u64 n = std::min<u64>(buf.size(), disc->Size() - off);
    if (!disc->Read(off, buf.data(), n)) {
      fprintf(stderr, "error: read failed at offset 0x%llx\n", (unsigned long long)off);
      return 1;
    }
    crc = UpdateCRC(crc, buf.data(), n);
    sha.Update(buf.data(), n);
    if (out) fwrite(buf.data(), 1, n, out);
  }
  if (out) fclose(out);
  u8 digest[20];
  sha.Final(digest);
  printf("crc32:  %08x\nsha1:   ", crc);
  for (u8 b : digest) printf("%02x", b);
  printf("\n");
  return 0;
}
