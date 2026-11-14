#include "v2_storage.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace mv2 {
namespace {
constexpr u32 kWalMagic = 0x3357564d; // MVW3
constexpr u64 kSegmentMagic = 0x3254474553564dULL; // MVSEG2

[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }

void write_all(int fd, const void* data, std::size_t size) {
  const char* p = static_cast<const char*>(data);
  while (size) {
    const ssize_t n = ::write(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) fail("short persistent write");
    p += n; size -= static_cast<std::size_t>(n);
  }
}

void read_all(int fd, void* data, std::size_t size, off_t offset) {
  char* p = static_cast<char*>(data);
  while (size) {
    const ssize_t n = ::pread(fd, p, size, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) fail("truncated persistent file");
    p += n; offset += n; size -= static_cast<std::size_t>(n);
  }
}

void sync_file(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0 || ::fsync(fd) != 0) { if (fd >= 0) ::close(fd); fail("cannot sync file"); }
  ::close(fd);
}

void sync_directory(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0 || ::fsync(fd) != 0) { if (fd >= 0) ::close(fd); fail("cannot sync directory"); }
  ::close(fd);
}

template<class T> void put(std::string& out, T value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
template<class T> T get(const std::string& in, std::size_t& offset) {
  if (offset + sizeof(T) > in.size()) fail("truncated manifest");
  T value{}; std::memcpy(&value, in.data() + offset, sizeof(value)); offset += sizeof(value); return value;
}
}

u32 crc32c(const void* data, std::size_t size) {
  const auto* p = static_cast<const unsigned char*>(data);
  u32 crc = 0xffffffffu;
  while (size--) { crc ^= *p++; for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u))); }
  return ~crc;
}

WalV3::WalV3(std::string path) : path_(std::move(path)) {
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd_ < 0) fail("cannot open WAL");
}
WalV3::~WalV3() { if (fd_ >= 0) ::close(fd_); }
u64 WalV3::size() const { struct stat st{}; if (::fstat(fd_, &st) != 0) fail("cannot stat WAL"); return static_cast<u64>(st.st_size); }
u64 WalV3::append(u64 seq, const std::string& payload) {
  if (payload.size() > 0xffffffffu) fail("WAL payload too large");
  std::string header; put<u32>(header, kWalMagic); put<std::uint16_t>(header, 3); put<std::uint16_t>(header, 0); put<u32>(header, static_cast<u32>(payload.size())); put<u64>(header, seq); put<u32>(header, 0);
  std::string check = header + payload; const u32 crc = crc32c(check.data(), check.size()); std::memcpy(header.data() + 20, &crc, sizeof(crc));
  const u64 offset = size(); write_all(fd_, header.data(), header.size()); write_all(fd_, payload.data(), payload.size());
  if (::fdatasync(fd_) != 0) fail("cannot sync WAL"); return offset;
}
void WalV3::replay(u64 offset, const std::function<void(u64, const std::string&)>& apply) const {
  const u64 end = size(); u64 cursor = offset;
  while (cursor < end) {
    if (end - cursor < 24) break;
    unsigned char header[24]; read_all(fd_, header, sizeof(header), static_cast<off_t>(cursor));
    u32 magic = 0, length = 0, stored_crc = 0; std::uint16_t version = 0, flags = 0; u64 seq = 0;
    std::memcpy(&magic, header, 4); std::memcpy(&version, header + 4, 2); std::memcpy(&flags, header + 6, 2); std::memcpy(&length, header + 8, 4); std::memcpy(&seq, header + 12, 8); std::memcpy(&stored_crc, header + 20, 4); (void)flags;
    if (magic != kWalMagic || version != 3 || length > (1u << 30)) fail("invalid WAL header");
    if (end - cursor - 24 < length) break;
    std::string payload(length, '\0'); if (length) read_all(fd_, payload.data(), length, static_cast<off_t>(cursor + 24));
    std::string check(reinterpret_cast<char*>(header), sizeof(header)); std::memset(check.data() + 20, 0, 4); check += payload;
    if (crc32c(check.data(), check.size()) != stored_crc) fail("WAL CRC mismatch");
    apply(seq, payload); cursor += 24 + length;
  }
  if (cursor < end && ::ftruncate(fd_, static_cast<off_t>(cursor)) != 0) fail("cannot truncate WAL tail");
}

VectorSegmentWriter::VectorSegmentWriter(std::string path, u64 segment_id, u64 generation, u32 dim) : path_(std::move(path)), segment_id_(segment_id), generation_(generation), dim_(dim) {
  if (!dim_) fail("invalid segment dimension"); stride_ = (dim_ * sizeof(float) + 63u) & ~63u; fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd_ < 0) fail("cannot create segment");
  std::string header(64, '\0'); std::memcpy(header.data(), &kSegmentMagic, 8); const u32 version = 2; std::memcpy(header.data()+8, &version, 4); std::memcpy(header.data()+12, &dim_, 4); std::memcpy(header.data()+16, &stride_, 4); std::memcpy(header.data()+24, &segment_id_, 8); std::memcpy(header.data()+32, &generation_, 8); write_all(fd_, header.data(), header.size());
}
VectorSegmentWriter::~VectorSegmentWriter() { if (fd_ >= 0) { try { finish(); } catch (...) { ::close(fd_); } } }
VectorLocation VectorSegmentWriter::append(VectorId id, const std::vector<float>& value) {
  if (value.size() != dim_) fail("vector dimension mismatch"); const u64 record = 64 + rows_ * (16 + stride_); const u32 bytes = dim_ * sizeof(float); const u32 zero = 0; write_all(fd_, &id.value, 8); write_all(fd_, &bytes, 4); write_all(fd_, &zero, 4); write_all(fd_, value.data(), bytes); if (stride_ > bytes) { std::string padding(stride_ - bytes, '\0'); write_all(fd_, padding.data(), padding.size()); } ++rows_; return {segment_id_, generation_, record + 16, bytes};
}
void VectorSegmentWriter::finish() { if (fd_ < 0) return; if (::pwrite(fd_, &rows_, 8, 40) != 8) fail("cannot finalize segment"); if (::fsync(fd_) != 0) fail("cannot sync segment"); ::close(fd_); fd_ = -1; }

VectorSegmentReader::VectorSegmentReader(std::string path) {
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC); if (fd_ < 0) fail("cannot open segment"); unsigned char header[64]; read_all(fd_, header, sizeof(header), 0); u64 magic = 0; std::memcpy(&magic, header, 8); if (magic != kSegmentMagic) fail("invalid segment"); std::memcpy(&dim_, header+12, 4); std::memcpy(&stride_, header+16, 4); std::memcpy(&segment_id_, header+24, 8); std::memcpy(&generation_, header+32, 8);
}
VectorSegmentReader::~VectorSegmentReader() { if (fd_ >= 0) ::close(fd_); }
std::vector<float> VectorSegmentReader::read(const VectorLocation& location, const ReadView& view) const {
  if (location.segment_id != segment_id_ || location.generation > view.version_generation || location.length != dim_ * sizeof(float)) fail("invalid vector location"); std::vector<float> value(dim_); read_all(fd_, value.data(), location.length, static_cast<off_t>(location.offset)); return value;
}

void ManifestStore::publish(const Manifest& manifest) const {
  const std::string path = directory_ + "/MANIFEST.v2", temp = path + ".tmp"; const int fd = ::open(temp.c_str(), O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644); if (fd < 0) fail("cannot create manifest"); std::string out; put<u32>(out, 0x32564d4d); put<u32>(out, manifest.format); put<u64>(out, manifest.generation); put<u64>(out, manifest.visible_seq); put<u64>(out, manifest.durable_seq); put<u64>(out, manifest.wal_offset); put<u64>(out, manifest.next_row_id); put<u64>(out, manifest.next_vector_id); put<u32>(out, static_cast<u32>(manifest.vector_segments.size())); for (const auto& name : manifest.vector_segments) { put<u32>(out, static_cast<u32>(name.size())); out += name; } write_all(fd, out.data(), out.size()); if (::fsync(fd) != 0) { ::close(fd); fail("cannot sync manifest"); } ::close(fd); if (::rename(temp.c_str(), path.c_str()) != 0) fail("cannot publish manifest"); sync_directory(directory_);
}
std::optional<Manifest> ManifestStore::load() const {
  std::ifstream file(directory_ + "/MANIFEST.v2", std::ios::binary); if (!file) return std::nullopt; const std::string data((std::istreambuf_iterator<char>(file)), {}); std::size_t p = 0; Manifest m; if (get<u32>(data,p) != 0x32564d4d) fail("invalid manifest"); m.format=get<u32>(data,p); m.generation=get<u64>(data,p); m.visible_seq=get<u64>(data,p); m.durable_seq=get<u64>(data,p); m.wal_offset=get<u64>(data,p); m.next_row_id=get<u64>(data,p); m.next_vector_id=get<u64>(data,p); const u32 count=get<u32>(data,p); if (count > 100000) fail("invalid manifest"); for (u32 i=0;i<count;++i) { const u32 len=get<u32>(data,p); if (len > 4096 || p+len > data.size()) fail("invalid manifest"); m.vector_segments.emplace_back(data.data()+p,len); p += len; } if (p != data.size()) fail("invalid manifest"); return m;
}
} // namespace mv2
