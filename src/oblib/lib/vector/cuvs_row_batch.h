/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace oceanbase {
namespace common {
namespace obvsag {

// Caller-owned storage: LOB/row scratch may be reused immediately after append.
class CuvsRowBatch {
public:
  static int capacity(int64_t dim)
  {
    if (dim <= 0 || static_cast<uint64_t>(dim) > MAX_BYTES / sizeof(float)) { return 0; }
    const size_t rows = MAX_BYTES / (static_cast<size_t>(dim) * sizeof(float));
    return static_cast<int>(rows < 256 ? rows : 256);
  }
  CuvsRowBatch(float *vectors, int64_t *ids, int64_t dim)
      : vectors_(vectors), ids_(ids), dim_(dim), capacity_(capacity(dim)), count_(0) {}
  bool append(const void *data, size_t bytes, int64_t id)
  {
    if (!data || !vectors_ || !ids_ || capacity_ == 0 || count_ == capacity_ ||
        bytes != static_cast<size_t>(dim_) * sizeof(float)) { return false; }
    std::memcpy(vectors_ + static_cast<size_t>(count_) * dim_, data, bytes);
    ids_[count_++] = id;
    return true;
  }
  int count() const { return count_; }
  bool full() const { return capacity_ > 0 && count_ == capacity_; }
  void reset() { count_ = 0; }
private:
  static constexpr size_t MAX_BYTES = 1024 * 1024;
  float *vectors_;
  int64_t *ids_;
  int64_t dim_;
  int capacity_;
  int count_;
};

} // namespace obvsag
} // namespace common
} // namespace oceanbase
