/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "cuvs_snapshot_format.h"
#include "lib/compress/zstd_1_3_8/ob_zstd_wrapper.h"
// The existing hash header contains an unused local; keep strict checks enabled
// for this codec without changing the shared hash implementation.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "lib/hash_func/murmur_hash.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <algorithm>
#include <cstdio>
#include <vector>

namespace oceanbase {
namespace common {
namespace obvsag {

// OBCUVS04 stores the complete opaque hipVS file in independent lossless
// blocks. Sizes are bounded before allocation/decompression. No GPU data,
// graph edge, FP32 bit or library-specific field is interpreted here.
// Each block: uint32 raw bytes, uint32 stored bytes, uint64 checksum, payload.
// stored == raw means uncompressed; otherwise the payload is a Zstd frame.
struct CuvsSnapshotCodec {
  enum : uint32_t { BLOCK_BYTES = 1024 * 1024 };
  struct Stats {
    uint64_t raw_bytes = 0, stored_bytes = 0, compressed_blocks = 0, raw_blocks = 0;
  };
  using Zstd = zstd_1_3_8::ObZstdWrapper;

  static bool encode(FILE *file, uint64_t bytes, std::ostream &out, Stats &stats)
  {
    stats = Stats{};
    if (!file || !out.good() || ::fflush(file) != 0 || ::fseek(file, 0, SEEK_SET) != 0) { return false; }
    // uint64 storage guarantees the alignment required by murmurhash.
    std::vector<uint64_t> raw_words(BLOCK_BYTES / sizeof(uint64_t));
    char *raw = reinterpret_cast<char *>(raw_words.data());
    std::vector<char> packed(Zstd::compress_bound(BLOCK_BYTES));
    zstd_1_3_8::OB_ZSTD_customMem mem{nullptr, nullptr, nullptr};
    uint64_t block = 0;
    while (bytes > 0) {
      const uint32_t raw_size = static_cast<uint32_t>(std::min<uint64_t>(bytes, BLOCK_BYTES));
      if (::fread(raw, 1, raw_size, file) != raw_size) { return false; }
      size_t packed_size = 0;
      if (Zstd::compress(mem, raw, raw_size, packed.data(), packed.size(), packed_size) != 0) { return false; }
      const bool compressed = packed_size < raw_size;
      const uint32_t stored_size = compressed ? static_cast<uint32_t>(packed_size) : raw_size;
      const uint64_t checksum = murmurhash(raw, raw_size, block++);
      if (stored_size == 0 ||
          !cuvs_snapshot_write(out, &raw_size, sizeof(raw_size)) ||
          !cuvs_snapshot_write(out, &stored_size, sizeof(stored_size)) ||
          !cuvs_snapshot_write(out, &checksum, sizeof(checksum)) ||
          !cuvs_snapshot_write(out, compressed ? packed.data() : raw, stored_size)) { return false; }
      stats.raw_bytes += raw_size;
      stats.stored_bytes += sizeof(raw_size) + sizeof(stored_size) + sizeof(checksum) + stored_size;
      if (compressed) { ++stats.compressed_blocks; } else { ++stats.raw_blocks; }
      bytes -= raw_size;
    }
    return true;
  }

  static bool decode(std::istream &in, uint64_t bytes, FILE *file, Stats &stats)
  {
    stats = Stats{};
    if (!file || !in.good()) { return false; }
    std::vector<uint64_t> raw_words(BLOCK_BYTES / sizeof(uint64_t));
    char *raw = reinterpret_cast<char *>(raw_words.data());
    std::vector<char> packed(BLOCK_BYTES);
    zstd_1_3_8::OB_ZSTD_customMem mem{nullptr, nullptr, nullptr};
    uint64_t block = 0;
    while (bytes > 0) {
      uint32_t raw_size = 0, stored_size = 0;
      uint64_t checksum = 0;
      if (!cuvs_snapshot_read(in, &raw_size, sizeof(raw_size)) ||
          !cuvs_snapshot_read(in, &stored_size, sizeof(stored_size)) ||
          !cuvs_snapshot_read(in, &checksum, sizeof(checksum)) ||
          raw_size != std::min<uint64_t>(bytes, BLOCK_BYTES) || stored_size == 0 || stored_size > raw_size) { return false; }
      const bool compressed = stored_size < raw_size;
      if (!cuvs_snapshot_read(in, compressed ? packed.data() : raw, stored_size)) { return false; }
      if (compressed) {
        size_t decoded_size = 0;
        if (Zstd::decompress(mem, packed.data(), stored_size, raw, raw_size, decoded_size) != 0 ||
            decoded_size != raw_size) { return false; }
      }
      if (murmurhash(raw, raw_size, block++) != checksum || ::fwrite(raw, 1, raw_size, file) != raw_size) { return false; }
      stats.raw_bytes += raw_size;
      stats.stored_bytes += sizeof(raw_size) + sizeof(stored_size) + sizeof(checksum) + stored_size;
      if (compressed) { ++stats.compressed_blocks; } else { ++stats.raw_blocks; }
      bytes -= raw_size;
    }
    return ::fflush(file) == 0;
  }
};

} // namespace obvsag
} // namespace common
} // namespace oceanbase
