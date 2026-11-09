// Tests the actual adaptor wire-format helpers; opaque hipVS bytes are a fixture.
#include "lib/vector/cuvs_snapshot_format.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace oceanbase::common::obvsag;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::abort(); } } while (0)

template <typename T> static void append(std::string &out, const T &value)
{
  out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

// Encode the historical V2 scalar layout independently of the production writer.
static std::string legacy_header(int version, uint64_t graph_bytes, int32_t dim, int32_t reserved, int64_t rows)
{
  std::string out = version == 2 ? "OBCUVS02" : (version == 3 ? "OBCUVS03" : "OBCUVS04");
  append(out, graph_bytes); append(out, dim); append(out, reserved); append(out, rows);
  CHECK(out.size() == 32);
  return out;
}

class LimitedOutput : public std::streambuf {
public:
  explicit LimitedOutput(size_t limit) : left_(limit) {}
protected:
  std::streamsize xsputn(const char *, std::streamsize n) override
  {
    const size_t count = std::min(left_, static_cast<size_t>(n));
    left_ -= count;
    return static_cast<std::streamsize>(count);
  }
private:
  size_t left_;
};

static bool read_prefix(std::istream &in, CuvsSnapshotHeader &header, int dim)
{
  char magic[8];
  return bool(in.read(magic, sizeof(magic))) &&
         header.read_body(in, CuvsSnapshotHeader::version_of(magic), dim);
}

int main()
{
  constexpr int dim = 3;
  const std::vector<int64_t> ids = {INT64_MIN, -17, 0, 101, INT64_MAX};
  std::vector<float> vectors(ids.size() * dim);
  for (size_t i = 0; i < vectors.size(); ++i) {
    const uint32_t bits = 0x80000000u | static_cast<uint32_t>(i * 13);
    std::memcpy(&vectors[i], &bits, sizeof(bits));
  }
  const std::string graph("opaque\0hipVS\xff\0payload", 21);
  for (const int version : {2, 3}) {
    const CuvsSnapshotHeader header{version, graph.size(), dim, 0, static_cast<int64_t>(ids.size())};
    std::string expected = legacy_header(version, graph.size(), dim, 0, ids.size());
    expected.append(reinterpret_cast<const char *>(ids.data()), ids.size() * sizeof(int64_t));
    if (version == 2) expected.append(reinterpret_cast<const char *>(vectors.data()), vectors.size() * sizeof(float));
    const size_t graph_offset = expected.size();
    expected += graph;

    std::ostringstream out(std::ios::binary);
    CHECK(header.write(out));
    CHECK(header.write_rows(out, ids.data(), version == 2 ? vectors.data() : nullptr));
    CHECK(cuvs_snapshot_write(out, graph.data(), graph.size()));
    CHECK(out.str() == expected);
    // V3 contains no second vector region; the full opaque payload is unchanged.
    CHECK(expected.size() == 32 + ids.size() * sizeof(int64_t) + graph.size() +
                              (version == 2 ? vectors.size() * sizeof(float) : 0));
    std::istringstream in(expected + "NEXT", std::ios::binary);
    CuvsSnapshotHeader read;
    CHECK(read_prefix(in, read, dim));
    CHECK(read.version == version && read.rows == header.rows && read.graph_bytes == graph.size());
    std::vector<int64_t> actual_ids(ids.size());
    std::vector<float> actual_vectors(vectors.size(), 42.0f);
    CHECK(read.read_rows(in, actual_ids.data(), actual_vectors.data()));
    CHECK(actual_ids == ids && in.tellg() == static_cast<std::streamoff>(graph_offset));
    if (version == 2) {
      CHECK(std::memcmp(actual_vectors.data(), vectors.data(), vectors.size() * sizeof(float)) == 0);
    } else {
      for (float v : actual_vectors) CHECK(v == 42.0f); // restored later through the bridge
    }
    std::string actual_graph(graph.size(), '\0');
    CHECK(cuvs_snapshot_read(in, &actual_graph[0], actual_graph.size()) && actual_graph == graph);
    char next[4]; CHECK(bool(in.read(next, 4)) && std::memcmp(next, "NEXT", 4) == 0);

    for (size_t length = 0; length < expected.size(); ++length) {
      std::istringstream truncated(expected.substr(0, length), std::ios::binary);
      CuvsSnapshotHeader broken;
      const bool accepted = read_prefix(truncated, broken, dim) &&
          broken.read_rows(truncated, actual_ids.data(), actual_vectors.data()) &&
          cuvs_snapshot_read(truncated, &actual_graph[0], actual_graph.size());
      CHECK(!accepted);
      LimitedOutput limit(length);
      std::ostream short_out(&limit);
      CHECK(!(header.write(short_out) && header.write_rows(short_out, ids.data(), vectors.data()) &&
              cuvs_snapshot_write(short_out, graph.data(), graph.size())));
    }
    std::istringstream mismatch(expected, std::ios::binary);
    CHECK(!read_prefix(mismatch, read, dim + 1));
    CHECK(!header.write_rows(out, nullptr, vectors.data()));
    if (version == 2) CHECK(!header.write_rows(out, ids.data(), nullptr));
  }
  for (const char *magic : {"OBCUVS01", "OBCUVS05", "NOT-CUVS"}) CHECK(CuvsSnapshotHeader::version_of(magic) == 0);
  CHECK(CuvsSnapshotHeader::version_of("OBCUVS04") == 4);
  for (const CuvsSnapshotHeader invalid : {
      CuvsSnapshotHeader{1, 20, 3, 0, 5}, CuvsSnapshotHeader{3, 0, 3, 0, 5},
      CuvsSnapshotHeader{3, UINT64_MAX, 3, 0, 5}, CuvsSnapshotHeader{3, 20, -1, 0, 5},
      CuvsSnapshotHeader{3, 20, 0, 0, 5}, CuvsSnapshotHeader{3, 20, 3, 1, 5},
      CuvsSnapshotHeader{3, 20, 3, 0, 0}, CuvsSnapshotHeader{3, 20, 3, 0, -1},
      CuvsSnapshotHeader{3, 20, 3, 0, INT64_MAX},
      CuvsSnapshotHeader{3, 20, INT32_MAX, 0, UINT32_MAX}}) {
    CHECK(!invalid.valid(invalid.dim));
    std::ostringstream out;
    CHECK(!invalid.write(out) && out.str().empty());
    std::istringstream in(legacy_header(invalid.version, invalid.graph_bytes, invalid.dim,
                                     invalid.reserved, invalid.rows).substr(8));
    CuvsSnapshotHeader read;
    CHECK(!read.read_body(in, invalid.version, invalid.dim));
  }
  std::puts("PASS snapshot format: V2 compatibility, V3 exact layout, IDs/FP32/opaque graph bytes, next-object boundary, corrupt headers and every truncation/short-write offset");
}
