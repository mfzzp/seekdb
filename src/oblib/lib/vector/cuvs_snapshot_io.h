/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <ostream>
#include <vector>
#ifdef __linux__
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase {
namespace common {
namespace obvsag {

// hipVS has finished writing and closed its descriptor before this is called.
// The private staging file must not be truncated while the mapping is live.
// Its bytes still go through the caller's durable output stream in full.
inline bool cuvs_copy_snapshot(FILE *file, uint64_t bytes, std::ostream &out,
                               bool &used_mapping, bool try_mapping = true)
{
  used_mapping = false;
  if (file == nullptr || !out.good() || ::fflush(file) != 0 || ::fseek(file, 0, SEEK_SET) != 0) {
    return false;
  }
  if (bytes == 0) { return true; }
#ifdef __linux__
  struct stat status;
  if (try_mapping && bytes <= std::numeric_limits<size_t>::max() &&
      ::fstat(::fileno(file), &status) == 0 && status.st_size >= 0 &&
      static_cast<uint64_t>(status.st_size) >= bytes) {
    // RAII also unmaps if an exception-enabled output stream throws.
    struct Mapping {
      void *data_;
      size_t size_;
      ~Mapping() { if (data_ != MAP_FAILED) { ::munmap(data_, size_); } }
    } mapping{::mmap(nullptr, static_cast<size_t>(bytes), PROT_READ, MAP_PRIVATE,
                     ::fileno(file), 0), static_cast<size_t>(bytes)};
    if (mapping.data_ != MAP_FAILED) {
      used_mapping = true;
      const char *data = static_cast<const char *>(mapping.data_);
      while (bytes > 0 && out.good()) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(bytes, 64UL * 1024 * 1024));
        out.write(data, static_cast<std::streamsize>(count));
        data += count;
        bytes -= count;
      }
      // Do not retry after an output error: a partial prefix may already be durable.
      return out.good();
    }
  }
#else
  (void)try_mapping;
#endif
  // Unsupported/failed mappings keep the previous bounded, buffered path.
  std::vector<char> chunk(static_cast<size_t>(std::min<uint64_t>(bytes, 1024 * 1024)));
  while (bytes > 0) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(bytes, chunk.size()));
    if (::fread(chunk.data(), 1, count, file) != count) { return false; }
    out.write(chunk.data(), static_cast<std::streamsize>(count));
    if (!out.good()) { return false; }
    bytes -= count;
  }
  return true;
}

} // namespace obvsag
} // namespace common
} // namespace oceanbase
