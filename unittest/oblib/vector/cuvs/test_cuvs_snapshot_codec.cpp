#include "lib/vector/cuvs_snapshot_codec.h"
#include "lib/vector/cuvs_snapshot_io.h"
#include <chrono>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace oceanbase::common::obvsag;
using Codec = CuvsSnapshotCodec;
struct FileCloser { void operator()(FILE *file) const { std::fclose(file); } };
using File = std::unique_ptr<FILE, FileCloser>;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::abort(); } } while (0)
static File temp_file()
{
  FILE *raw = nullptr;
#ifdef _WIN32
  CHECK(::tmpfile_s(&raw) == 0);
#else
  raw = std::tmpfile();
#endif
  File file(raw); CHECK(file); return file;
}
static uint32_t next(uint32_t &state) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
static std::string noise(size_t size)
{
  std::string out(size, '\0'); uint32_t state = 7;
  for (size_t pos = 0; pos < size; ++pos) out[pos] = static_cast<char>(next(state));
  return out;
}
static File source_file(const std::string &data)
{
  File f = temp_file(); CHECK(std::fwrite(data.data(), 1, data.size(), f.get()) == data.size());
  return f;
}
static std::string encode(const std::string &raw, Codec::Stats &stats)
{
  File f = source_file(raw);
  std::ostringstream out(std::ios::binary);
  CHECK(Codec::encode(f.get(), raw.size(), out, stats));
  CHECK(stats.raw_bytes == raw.size() && stats.stored_bytes == out.str().size());
  return out.str();
}
static bool decode(const std::string &packed, const std::string &expected)
{
  std::istringstream in(packed + "NEXT", std::ios::binary);
  File out = temp_file(); Codec::Stats stats;
  if (!Codec::decode(in, expected.size(), out.get(), stats)) return false;
  CHECK(stats.raw_bytes == expected.size() && stats.stored_bytes == packed.size());
  CHECK(in.tellg() == static_cast<std::streamoff>(packed.size()));
  char next_bytes[4]; CHECK(bool(in.read(next_bytes, 4)) && std::memcmp(next_bytes, "NEXT", 4) == 0);
  std::rewind(out.get());
  std::string result(expected.size(), '\0');
  CHECK(std::fread(&result[0], 1, result.size(), out.get()) == result.size());
  CHECK(result == expected && std::fgetc(out.get()) == EOF);
  return true;
}
static void rejected(const std::string &packed, uint64_t raw_size)
{
  std::istringstream in(packed, std::ios::binary); File out = temp_file(); Codec::Stats stats;
  CHECK(!Codec::decode(in, raw_size, out.get(), stats));
}
class ShortOutput : public std::streambuf {
public:
  explicit ShortOutput(size_t left, bool throws = false) : left_(left), throws_(throws) {}
protected:
  std::streamsize xsputn(const char *, std::streamsize count) override
  {
    if (throws_) throw std::runtime_error("injected write exception");
    const size_t n = std::min(left_, static_cast<size_t>(count)); left_ -= n; return n;
  }
private:
  size_t left_; bool throws_;
};

static void tests()
{
  const size_t block = Codec::BLOCK_BYTES;
  for (size_t size : {size_t(0), size_t(1), size_t(127), size_t(65537), block - 1, block, block + 1, 2 * block + 17}) {
    for (bool random : {false, true}) {
      const std::string raw = random ? noise(size) : std::string(size, '\0');
      Codec::Stats stats;
      const std::string packed = encode(raw, stats);
      CHECK(decode(packed, raw));
      if (size > 65536 && !random) CHECK(stats.compressed_blocks > 0 && packed.size() < raw.size() / 10);
      if (size > 65536 && random) CHECK(stats.raw_blocks > 0);
      if (size > 0) {
        for (size_t at : {size_t(0), size_t(4), size_t(8), size_t(15), packed.size() - 1}) {
          rejected(packed.substr(0, at), size);
          std::string corrupt = packed; corrupt[at] ^= 1; rejected(corrupt, size);
        }
        rejected(packed, size + 1);
      }
    }
  }
  Codec::Stats stats;
  const std::string raw = std::string(block, 'x') + noise(block) + std::string(71, 'y');
  const std::string packed = encode(raw, stats);
  CHECK(stats.compressed_blocks == 2 && stats.raw_blocks == 1 && decode(packed, raw));
  // Exercise the full envelope/payload boundary with the production helpers.
  // V2/V3 keep their original bytes; only V4 routes the opaque payload to Zstd.
  for (int version : {2, 3, 4}) {
    const int64_t ids[] = {INT64_MIN, 57, INT64_MAX};
    const float vectors[] = {-0.0f, 1.25f, -2.5f, 7.0f, 3.0f, 0.0f};
    CuvsSnapshotHeader header{version, raw.size(), 2, 0, 3};
    std::ostringstream envelope(std::ios::binary);
    CHECK(header.write(envelope) && header.write_rows(envelope, ids, vectors));
    File f = source_file(raw);
    bool mapped = false;
    CHECK(version == 4 ? Codec::encode(f.get(), raw.size(), envelope, stats)
                       : cuvs_copy_snapshot(f.get(), raw.size(), envelope, mapped, false));
    std::istringstream in(envelope.str() + "NEXT", std::ios::binary);
    char magic[8]; CHECK(bool(in.read(magic, 8)));
    CuvsSnapshotHeader read;
    CHECK(read.read_body(in, CuvsSnapshotHeader::version_of(magic), 2) && read.version == version);
    int64_t restored_ids[3] = {}; float restored_vectors[6] = {};
    CHECK(read.read_rows(in, restored_ids, restored_vectors));
    CHECK(std::memcmp(ids, restored_ids, sizeof(ids)) == 0);
    if (version == 2) CHECK(std::memcmp(vectors, restored_vectors, sizeof(vectors)) == 0);
    File decoded = temp_file();
    if (version == 4) {
      CHECK(Codec::decode(in, read.graph_bytes, decoded.get(), stats));
      std::rewind(decoded.get()); std::string restored(raw.size(), '\0');
      CHECK(std::fread(&restored[0], 1, restored.size(), decoded.get()) == restored.size() && restored == raw);
    } else {
      std::string restored(raw.size(), '\0');
      CHECK(cuvs_snapshot_read(in, &restored[0], restored.size()) && restored == raw);
    }
    char next_bytes[4]; CHECK(bool(in.read(next_bytes, 4)) && std::memcmp(next_bytes, "NEXT", 4) == 0);
  }
  for (uint32_t invalid : {0u, uint32_t(block + 1), UINT32_MAX}) {
    std::string corrupt = packed; std::memcpy(&corrupt[0], &invalid, 4); rejected(corrupt, raw.size());
    corrupt = packed; std::memcpy(&corrupt[4], &invalid, 4); rejected(corrupt, raw.size());
  }
  // Block ordinal is part of the checksum: moving identical raw blocks fails.
  const std::string repeated = std::string(2 * block, 'q');
  std::string reordered = encode(repeated, stats);
  uint32_t stored = 0; std::memcpy(&stored, reordered.data() + 4, 4);
  const size_t first = 16 + stored;
  CHECK(reordered.size() == first * 2);
  rejected(reordered.substr(first) + reordered.substr(0, first), repeated.size());
  // Every truncated position of a short frame must fail.
  const std::string short_raw(91, 'a');
  const std::string short_frame = encode(short_raw, stats);
  for (size_t at = 0; at < short_frame.size(); ++at) rejected(short_frame.substr(0, at), short_raw.size());
  for (size_t limit : {size_t(0), size_t(4), size_t(8), size_t(16), packed.size() - 1}) {
    File f = source_file(raw); ShortOutput buf(limit); std::ostream out(&buf);
    CHECK(!Codec::encode(f.get(), raw.size(), out, stats));
  }
  File short_source = source_file(short_raw); std::ostringstream out;
  CHECK(!Codec::encode(short_source.get(), short_raw.size() + 1, out, stats));
  CHECK(!Codec::encode(nullptr, 1, out, stats));
  {
    File f = source_file(short_raw); ShortOutput buf(0, true); std::ostream throwing(&buf);
    throwing.exceptions(std::ios::badbit); bool caught = false;
    try { Codec::encode(f.get(), short_raw.size(), throwing, stats); } catch (const std::exception &) { caught = true; }
    CHECK(caught);
  }
#ifdef __linux__
  File full(std::fopen("/dev/full", "wb")); CHECK(full);
  std::istringstream in(short_frame);
  CHECK(!Codec::decode(in, short_raw.size(), full.get(), stats));
#endif
  std::puts("PASS real bundled Zstd: byte equality, mixed raw/compressed blocks, boundaries, bounded headers, checksum, reorder, truncation, failed I/O and exception cleanup");
}

static void benchmark()
{
  using Clock = std::chrono::steady_clock;
  for (const std::string kind : {"graph_ids", "fp32_uniform", "fp32_integer", "random_bytes"}) {
    std::string raw(32 * 1024 * 1024, '\0'); uint32_t state = 17;
    for (size_t pos = 0; pos < raw.size(); pos += 4) {
      const uint32_t bits = next(state);
      if (kind == "fp32_uniform" || kind == "fp32_integer") {
        const float value = kind == "fp32_uniform" ? static_cast<float>(bits >> 8) / 8388608.0f - 1.0f
                                                   : static_cast<float>(bits % 256);
        std::memcpy(&raw[pos], &value, 4);
      } else {
        const uint32_t value = kind == "graph_ids" ? bits % 250000 : bits;
        std::memcpy(&raw[pos], &value, 4);
      }
    }
    double encode_ms = 0, decode_ms = 0; uint64_t stored = 0;
    // Synthetic byte/CPU cost probe, not hipVS or SQL performance measurement.
    for (int sample = 0; sample < 3; ++sample) {
      File f = source_file(raw); Codec::Stats stats; std::ostringstream out(std::ios::binary);
      auto start = Clock::now(); CHECK(Codec::encode(f.get(), raw.size(), out, stats));
      encode_ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      stored = stats.stored_bytes;
      std::istringstream in(out.str(), std::ios::binary); File decoded = temp_file();
      start = Clock::now(); CHECK(Codec::decode(in, raw.size(), decoded.get(), stats));
      decode_ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      std::rewind(decoded.get()); std::string restored(raw.size(), '\0');
      CHECK(std::fread(&restored[0], 1, restored.size(), decoded.get()) == restored.size() && restored == raw);
    }
    std::printf("kind=%s raw_bytes=%zu stored_bytes=%llu encode_ms=%.3f decode_ms=%.3f samples=3\n",
                kind.c_str(), raw.size(), static_cast<unsigned long long>(stored), encode_ms / 3, decode_ms / 3);
  }
}

int main(int argc, char **argv)
{
  if (argc == 2 && std::strcmp(argv[1], "--bench") == 0) benchmark(); else tests();
}
