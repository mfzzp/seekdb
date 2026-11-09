/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <cstdint>
#include <cstring>
#include <istream>
#include <ostream>
#include <limits>

namespace oceanbase {
namespace common {
namespace obvsag {

inline bool cuvs_snapshot_write(std::ostream &out, const void *data, size_t bytes)
{
  const char *p = static_cast<const char *>(data);
  constexpr size_t CHUNK = 64UL * 1024 * 1024;
  while (bytes > 0 && out.good()) {
    const size_t sz = bytes < CHUNK ? bytes : CHUNK;
    out.write(p, static_cast<std::streamsize>(sz));
    p += sz; bytes -= sz;
  }
  return out.good();
}

inline bool cuvs_snapshot_read(std::istream &in, void *data, size_t bytes)
{
  char *p = static_cast<char *>(data);
  constexpr size_t CHUNK = 64UL * 1024 * 1024;
  while (bytes > 0) {
    const size_t sz = bytes < CHUNK ? bytes : CHUNK;
    in.read(p, static_cast<std::streamsize>(sz));
    if (in.gcount() != static_cast<std::streamsize>(sz)) { return false; }
    p += sz; bytes -= sz;
  }
  return true;
}

// V2: header, IDs, raw vectors, complete hipVS snapshot (including vectors).
// V3: header, IDs, complete hipVS snapshot. Its dataset restores the raw buffer.
// V4: same as V3, with the opaque snapshot losslessly encoded in bounded blocks.
struct CuvsSnapshotHeader {
  static constexpr size_t MAGIC_SIZE = 8;
  int version = 3;
  uint64_t graph_bytes = 0;
  int32_t dim = 0;
  int32_t reserved = 0;
  int64_t rows = 0;

  static int version_of(const char *magic)
  {
    if (std::memcmp(magic, "OBCUVS02", MAGIC_SIZE) == 0) { return 2; }
    if (std::memcmp(magic, "OBCUVS03", MAGIC_SIZE) == 0) { return 3; }
    if (std::memcmp(magic, "OBCUVS04", MAGIC_SIZE) == 0) { return 4; }
    return 0;
  }
  bool valid(int expected_dim) const
  {
    return (version == 2 || version == 3 || version == 4) && dim > 0 && dim == expected_dim &&
           reserved == 0 && rows > 0 && static_cast<uint64_t>(rows) <= UINT32_MAX &&
           static_cast<uint64_t>(rows) <= std::numeric_limits<size_t>::max() / sizeof(float) / dim &&
           static_cast<uint64_t>(rows) <= std::numeric_limits<size_t>::max() / sizeof(int64_t) &&
           graph_bytes > 0 && graph_bytes <= static_cast<uint64_t>(INT64_MAX);
  }
  // The format sniffer already consumed the eight magic bytes.
  bool read_body(std::istream &in, int input_version, int expected_dim)
  {
    version = input_version;
    return bool(in.read(reinterpret_cast<char *>(&graph_bytes), sizeof(graph_bytes))) &&
           bool(in.read(reinterpret_cast<char *>(&dim), sizeof(dim))) &&
           bool(in.read(reinterpret_cast<char *>(&reserved), sizeof(reserved))) &&
           bool(in.read(reinterpret_cast<char *>(&rows), sizeof(rows))) && valid(expected_dim);
  }
  bool write(std::ostream &out) const
  {
    if (!valid(dim)) { return false; }
    out.write(version == 2 ? "OBCUVS02" : (version == 3 ? "OBCUVS03" : "OBCUVS04"), MAGIC_SIZE);
    out.write(reinterpret_cast<const char *>(&graph_bytes), sizeof(graph_bytes));
    out.write(reinterpret_cast<const char *>(&dim), sizeof(dim));
    out.write(reinterpret_cast<const char *>(&reserved), sizeof(reserved));
    out.write(reinterpret_cast<const char *>(&rows), sizeof(rows));
    return out.good();
  }
  bool write_rows(std::ostream &out, const int64_t *ids, const float *vectors = nullptr) const
  {
    return valid(dim) && ids != nullptr && (version != 2 || vectors != nullptr) &&
           cuvs_snapshot_write(out, ids, static_cast<size_t>(rows) * sizeof(int64_t)) &&
           (version != 2 || cuvs_snapshot_write(out, vectors, static_cast<size_t>(rows) * dim * sizeof(float)));
  }
  bool read_rows(std::istream &in, int64_t *ids, float *vectors) const
  {
    return valid(dim) && ids != nullptr && (version != 2 || vectors != nullptr) &&
           cuvs_snapshot_read(in, ids, static_cast<size_t>(rows) * sizeof(int64_t)) &&
           (version != 2 || cuvs_snapshot_read(in, vectors, static_cast<size_t>(rows) * dim * sizeof(float)));
  }
};

} // namespace obvsag
} // namespace common
} // namespace oceanbase
