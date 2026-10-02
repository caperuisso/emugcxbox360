// SPDX-License-Identifier: GPL-2.0-or-later
// Reader for Dolphin's WIA and RVZ compressed disc images (GameCube discs).
// Format reference: docs/WiaAndRvz.md in the Dolphin repository.
// Supported compression: NONE and Zstandard (Dolphin's RVZ default).
#include <algorithm>
#include <vector>

#include "core/disc/disc_reader.h"
#include "zstd/zstd.h"

namespace {

constexpr u32 COMPRESSION_NONE = 0;
constexpr u32 COMPRESSION_ZSTD = 5;
constexpr u32 DISC_TYPE_GAMECUBE = 1;
constexpr u32 SECTOR_SIZE = 0x8000;

// Lagged Fibonacci generator that recreates the pseudorandom padding of GC/Wii
// discs (f = xor, j = 32, k = 521), as stored by RVZ packing.
class JunkGenerator {
 public:
  static constexpr u32 K = 521, J = 32, SEED_WORDS = 17, BUFFER_BYTES = K * 4;

  void SetSeed(const u8* seed) {
    for (u32 i = 0; i < SEED_WORDS; i++) m_buf[i] = LoadBE32(seed + i * 4);
    for (u32 i = SEED_WORDS; i < K; i++)
      m_buf[i] = (m_buf[i - 17] << 23) ^ (m_buf[i - 16] >> 9) ^ m_buf[i - 1];
    for (int i = 0; i < 4; i++) Advance();
    m_pos = 0;
  }

  void Skip(u64 bytes) {
    m_pos += bytes;
    while (m_pos >= BUFFER_BYTES) {
      Advance();
      m_pos -= BUFFER_BYTES;
    }
  }

  void Generate(u8* out, u64 count) {
    static const u8 kShift[4] = {24, 18, 8, 0};  // 18, not 16: quirk of the original generator
    for (u64 k = 0; k < count; k++) {
      out[k] = (u8)(m_buf[m_pos >> 2] >> kShift[m_pos & 3]);
      if (++m_pos == BUFFER_BYTES) {
        Advance();
        m_pos = 0;
      }
    }
  }

 private:
  void Advance() {
    for (u32 i = 0; i < J; i++) m_buf[i] ^= m_buf[i + K - J];
    for (u32 i = J; i < K; i++) m_buf[i] ^= m_buf[i - J];
  }

  u32 m_buf[K];
  u64 m_pos = 0;
};

class WIAReader : public DiscReader {
 public:
  ~WIAReader() override {
    if (m_dctx) ZSTD_freeDCtx(m_dctx);
    if (m_file) fclose(m_file);
  }

  bool Open(FILE* f, std::string* error) {
    m_file = f;
    u8 head[0x48];
    if (!ReadFile(0, head, sizeof(head))) return Fail(error, "truncated header");
    m_rvz = memcmp(head, "RVZ\x01", 4) == 0;
    u32 disc_size = LoadBE32(head + 0x0C);
    m_iso_size = LoadBE64(head + 0x24);
    if (disc_size < 0xDC) return Fail(error, "unsupported WIA/RVZ header");

    std::vector<u8> disc(disc_size);
    if (!ReadFile(0x48, disc.data(), disc_size)) return Fail(error, "truncated disc header");
    u32 disc_type = LoadBE32(&disc[0x00]);
    m_compression = LoadBE32(&disc[0x04]);
    m_chunk_size = LoadBE32(&disc[0x0C]);
    memcpy(m_dhead, &disc[0x10], sizeof(m_dhead));
    u32 n_raw = LoadBE32(&disc[0xB4]);
    u64 raw_off = LoadBE64(&disc[0xB8]);
    u32 raw_size = LoadBE32(&disc[0xC0]);
    u32 n_groups = LoadBE32(&disc[0xC4]);
    u64 group_off = LoadBE64(&disc[0xC8]);
    u32 group_size = LoadBE32(&disc[0xD0]);

    if (disc_type != DISC_TYPE_GAMECUBE) return Fail(error, "only GameCube discs are supported (this is a Wii disc)");
    if (m_compression != COMPRESSION_NONE && m_compression != COMPRESSION_ZSTD)
      return Fail(error,
                  "unsupported compression method (only zstd or none). Convert it with: "
                  "dolphin-tool convert -f rvz -c zstd -i in.rvz -o out.rvz, or to ISO");
    if (m_chunk_size < SECTOR_SIZE) return Fail(error, "invalid chunk size");
    if (m_compression == COMPRESSION_ZSTD && !(m_dctx = ZSTD_createDCtx())) return Fail(error, "out of memory");

    BootCheckpoint("RVZ header read");
    // Raw data table: { u64 offset, u64 size, u32 group_index, u32 n_groups }
    std::vector<u8> raw(n_raw * 24);
    if (!ReadTable(raw_off, raw_size, raw)) return Fail(error, "corrupt raw data table");
    BootCheckpoint("RVZ raw table decompressed");
    for (u32 i = 0; i < n_raw; i++) {
      const u8* r = &raw[i * 24];
      RawData rd;
      u64 off = LoadBE64(r), size = LoadBE64(r + 8);
      // The first entry starts at 0x80; round down to a sector boundary (see spec).
      rd.offset = off & ~(u64)(SECTOR_SIZE - 1);
      rd.size = size + (off - rd.offset);
      rd.group_index = LoadBE32(r + 16);
      rd.n_groups = LoadBE32(r + 20);
      m_raw.push_back(rd);
    }

    // Group table: WIA { u32 off4, u32 size }, RVZ adds { u32 packed_size }
    u32 entry = m_rvz ? 12 : 8;
    std::vector<u8> groups(n_groups * entry);
    BootCheckpoint("RVZ before group table");
    if (!ReadTable(group_off, group_size, groups)) return Fail(error, "corrupt group table");
    BootCheckpoint("RVZ group table decompressed");
    for (u32 i = 0; i < n_groups; i++) {
      const u8* g = &groups[i * entry];
      Group gr;
      gr.file_offset = (u64)LoadBE32(g) * 4;
      u32 size = LoadBE32(g + 4);
      if (m_rvz) {
        gr.compressed = (size & 0x80000000) != 0;
        gr.size = size & 0x7FFFFFFF;
        gr.packed_size = LoadBE32(g + 8);
      } else {
        gr.compressed = m_compression != COMPRESSION_NONE;
        gr.size = size;
        gr.packed_size = 0;
      }
      m_groups.push_back(gr);
    }
    for (const RawData& rd : m_raw)
      if ((u64)rd.group_index + rd.n_groups > m_groups.size()) return Fail(error, "group index out of range");
    return true;
  }

  u64 Size() const override { return m_iso_size; }
  const char* FormatName() const override { return m_rvz ? "RVZ" : "WIA"; }

  bool Read(u64 offset, void* dst_void, u64 len) override {
    u8* dst = (u8*)dst_void;
    if (offset + len > m_iso_size) return false;
    while (len) {
      if (offset < sizeof(m_dhead)) {
        u64 n = std::min<u64>(len, sizeof(m_dhead) - offset);
        memcpy(dst, m_dhead + offset, n);
        offset += n, dst += n, len -= n;
        continue;
      }
      const RawData* rd = FindRawData(offset);
      if (!rd) {  // not covered by any entry: the disc has zeroes there
        memset(dst, 0, 1);
        offset++, dst++, len--;
        continue;
      }
      u64 chunk = (offset - rd->offset) / m_chunk_size;
      u64 chunk_start = rd->offset + chunk * m_chunk_size;
      u64 chunk_len = std::min<u64>(m_chunk_size, rd->offset + rd->size - chunk_start);
      if (chunk >= rd->n_groups || !LoadGroup(rd->group_index + (u32)chunk, chunk_start, chunk_len)) return false;
      u64 in_chunk = offset - chunk_start;
      u64 n = std::min<u64>(len, chunk_len - in_chunk);
      memcpy(dst, m_cache.data() + in_chunk, n);
      offset += n, dst += n, len -= n;
    }
    return true;
  }

 private:
  struct RawData {
    u64 offset, size;
    u32 group_index, n_groups;
  };
  struct Group {
    u64 file_offset;
    u32 size;
    bool compressed;
    u32 packed_size;
  };

  bool Fail(std::string* error, const char* msg) {
    if (error) *error = msg;
    return false;
  }

  bool ReadFile(u64 offset, void* dst, u64 len) {
    if (fseek(m_file, (long)offset, SEEK_SET) != 0) return false;
    return fread(dst, 1, len, m_file) == len;
  }

  bool Decompress(const std::vector<u8>& src, u8* dst, u64 dst_len) {
    if (m_compression == COMPRESSION_NONE) {
      if (src.size() < dst_len) return false;
      memcpy(dst, src.data(), dst_len);
      return true;
    }
    size_t got = ZSTD_decompressDCtx(m_dctx, dst, dst_len, src.data(), src.size());
    return !ZSTD_isError(got) && got == dst_len;
  }

  // Tables are stored compressed with the image's method.
  bool ReadTable(u64 file_offset, u32 stored_size, std::vector<u8>& out) {
    m_comp.resize(stored_size);
    if (!ReadFile(file_offset, m_comp.data(), stored_size)) return false;
    return Decompress(m_comp, out.data(), out.size());
  }

  const RawData* FindRawData(u64 offset) const {
    for (const RawData& rd : m_raw)
      if (offset >= rd.offset && offset < rd.offset + rd.size) return &rd;
    return nullptr;
  }

  bool LoadGroup(u32 index, u64 disc_offset, u64 len) {
    if (index == m_cached_group) return true;
    m_cached_group = ~0u;
    const Group& g = m_groups[index];
    m_cache.assign(len, 0);
    if (g.size == 0) {  // all zeroes
      m_cached_group = index;
      return true;
    }
    m_comp.resize(g.size);
    if (!ReadFile(g.file_offset, m_comp.data(), g.size)) return false;

    u64 stage_len = g.packed_size ? g.packed_size : len;
    u8* stage;
    if (g.packed_size) {
      m_packed.resize(stage_len);
      stage = m_packed.data();
    } else {
      stage = m_cache.data();
    }
    if (g.compressed) {
      if (!Decompress(m_comp, stage, stage_len)) return false;
    } else {
      if (m_comp.size() < stage_len) return false;
      memcpy(stage, m_comp.data(), stage_len);
    }
    if (g.packed_size && !Unpack(m_packed.data(), g.packed_size, disc_offset, len)) return false;
    m_cached_group = index;
    return true;
  }

  // RVZ packing: runs of literal bytes, or runs of padding regenerated from a seed.
  bool Unpack(const u8* p, u64 packed_len, u64 disc_offset, u64 len) {
    u64 pos = 0, out = 0;
    while (out < len) {
      if (pos + 4 > packed_len) return false;
      u32 size = LoadBE32(p + pos);
      pos += 4;
      bool junk = size & 0x80000000;
      size &= 0x7FFFFFFF;
      if (out + size > len) return false;
      if (junk) {
        if (pos + JunkGenerator::SEED_WORDS * 4 > packed_len) return false;
        m_junk.SetSeed(p + pos);
        pos += JunkGenerator::SEED_WORDS * 4;
        m_junk.Skip((disc_offset + out) % SECTOR_SIZE);
        m_junk.Generate(m_cache.data() + out, size);
      } else {
        if (pos + size > packed_len) return false;
        memcpy(m_cache.data() + out, p + pos, size);
        pos += size;
      }
      out += size;
    }
    return true;
  }

  FILE* m_file = nullptr;
  bool m_rvz = false;
  u32 m_compression = 0;
  u32 m_chunk_size = 0;
  u64 m_iso_size = 0;
  u8 m_dhead[0x80];
  std::vector<RawData> m_raw;
  std::vector<Group> m_groups;
  ZSTD_DCtx* m_dctx = nullptr;
  JunkGenerator m_junk;

  u32 m_cached_group = ~0u;
  std::vector<u8> m_cache;   // decoded bytes of the cached group
  std::vector<u8> m_comp;    // compressed bytes read from the file
  std::vector<u8> m_packed;  // decompressed but still RVZ-packed bytes
};

}  // namespace

std::unique_ptr<DiscReader> OpenWIAOrRVZ(FILE* file, std::string* error) {
  auto reader = std::make_unique<WIAReader>();
  if (!reader->Open(file, error)) return nullptr;  // destructor closes the file
  return reader;
}
