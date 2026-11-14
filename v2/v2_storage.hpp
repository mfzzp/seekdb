#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mv2 {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct ReadView {
  u64 snapshot_seq = 0;
  u64 version_generation = 0;
};

struct VectorId { u64 value = 0; };
struct VectorLocation {
  u64 segment_id = 0;
  u64 generation = 0;
  u64 offset = 0;
  u32 length = 0;
};

struct Manifest {
  u32 format = 2;
  u64 generation = 0;
  u64 visible_seq = 0;
  u64 durable_seq = 0;
  u64 wal_offset = 0;
  u64 next_row_id = 1;
  u64 next_vector_id = 1;
  std::vector<std::string> vector_segments;
};

u32 crc32c(const void *data, std::size_t size);

class WalV3 {
 public:
  explicit WalV3(std::string path);
  ~WalV3();
  WalV3(const WalV3&) = delete;
  WalV3& operator=(const WalV3&) = delete;
  u64 append(u64 commit_seq, const std::string& payload);
  u64 size() const;
  void replay(u64 offset, const std::function<void(u64, const std::string&)>& apply) const;
 private:
  std::string path_;
  int fd_ = -1;
};

class VectorSegmentWriter {
 public:
  VectorSegmentWriter(std::string path, u64 segment_id, u64 generation, u32 dim);
  ~VectorSegmentWriter();
  VectorSegmentWriter(const VectorSegmentWriter&) = delete;
  VectorSegmentWriter& operator=(const VectorSegmentWriter&) = delete;
  VectorLocation append(VectorId id, const std::vector<float>& value);
  void finish();
 private:
  std::string path_;
  int fd_ = -1;
  u64 segment_id_ = 0, generation_ = 0, rows_ = 0;
  u32 dim_ = 0, stride_ = 0;
};

class VectorSegmentReader {
 public:
  explicit VectorSegmentReader(std::string path);
  ~VectorSegmentReader();
  VectorSegmentReader(const VectorSegmentReader&) = delete;
  VectorSegmentReader& operator=(const VectorSegmentReader&) = delete;
  std::vector<float> read(const VectorLocation&, const ReadView&) const;
 private:
  int fd_ = -1;
  u64 segment_id_ = 0, generation_ = 0;
  u32 dim_ = 0, stride_ = 0;
};

class ManifestStore {
 public:
  explicit ManifestStore(std::string directory) : directory_(std::move(directory)) {}
  void publish(const Manifest&) const;
  std::optional<Manifest> load() const;
 private:
  std::string directory_;
};

} // namespace mv2
