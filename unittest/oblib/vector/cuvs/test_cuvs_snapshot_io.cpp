#include "lib/vector/cuvs_snapshot_io.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#ifdef __linux__
#include <cerrno>
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::abort(); } } while (0)
using oceanbase::common::obvsag::cuvs_copy_snapshot;

#ifdef __linux__
// Only this test executable wraps mmap; production calls the system API directly.
static bool fail_mapping = false;
static int mappings = 0;
extern "C" void *__real_mmap(void *, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void *, size_t);
extern "C" void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t offset)
{
  if (fail_mapping) { errno = ENOMEM; return MAP_FAILED; }
  void *result = __real_mmap(p, n, prot, flags, fd, offset);
  if (result != MAP_FAILED) { ++mappings; }
  return result;
}
extern "C" int __wrap_munmap(void *p, size_t n)
{
  const int result = __real_munmap(p, n);
  if (result == 0) { --mappings; }
  return result;
}
#endif

struct File {
  FILE *file = nullptr;
  explicit File(bool memory) {
#ifdef __linux__
    if (memory) {
      const int fd = static_cast<int>(syscall(SYS_memfd_create, "cuvs-snapshot-test", MFD_CLOEXEC));
      CHECK(fd >= 0);
      file = fdopen(fd, "w+b");
      if (!file) { close(fd); }
    }
#else
    (void)memory;
#endif
    if (!file) { file = std::tmpfile(); }
    CHECK(file);
  }
  ~File() { CHECK(std::fclose(file) == 0); }
};

// A short write must stop the transfer, including on exception-enabled streams.
struct LimitedSink : std::streambuf {
  size_t remaining;
  std::string accepted;
  explicit LimitedSink(size_t n) : remaining(n) {}
  std::streamsize xsputn(const char *p, std::streamsize n) override {
    const size_t count = std::min<size_t>(n, remaining);
    accepted.append(p, count);
    remaining -= count;
    return static_cast<std::streamsize>(count);
  }
};

static void correctness()
{
  bool mapped = true;
  std::ostringstream invalid;
  CHECK(!cuvs_copy_snapshot(nullptr, 1, invalid, mapped) && !mapped);
  for (bool memory : {false, true}) {
    for (size_t bytes : {size_t(0), size_t(1), size_t(4095), size_t(4096), size_t(4097),
                         size_t(1024 * 1024 + 37), size_t(64 * 1024 * 1024 + 37)}) {
      File source(memory);
      std::string expected(bytes, '\0');
      for (size_t i = 0; i < bytes; ++i) { expected[i] = static_cast<char>((i * 73 + i / 8191) % 256); }
      CHECK(std::fwrite(expected.data(), 1, bytes, source.file) == bytes);
      for (bool try_mapping : {false, true}) {
        std::ostringstream out;
        out << "prefix";
        CHECK(cuvs_copy_snapshot(source.file, bytes, out, mapped, try_mapping));
#ifdef __linux__
        CHECK(mapped == (try_mapping && bytes > 0));
        CHECK(mappings == 0);
#else
        CHECK(!mapped);
#endif
        out << "suffix";
        const std::string actual = out.str();
        CHECK(actual.size() == bytes + 12 && actual.compare(0, 6, "prefix") == 0);
        CHECK(actual.compare(6, bytes, expected) == 0 && actual.compare(6 + bytes, 6, "suffix") == 0);
        // A header claiming more than the complete file must fail, not read past EOF.
        std::ostringstream truncated;
        CHECK(!cuvs_copy_snapshot(source.file, bytes + 1, truncated, mapped, try_mapping));
        CHECK(!mapped);
      }
    }
    File source(memory);
    const std::string expected(2 * 1024 * 1024 + 19, 'X');
    CHECK(std::fwrite(expected.data(), 1, expected.size(), source.file) == expected.size());
    for (bool try_mapping : {false, true}) {
      for (bool throwing : {false, true}) {
        LimitedSink sink(1024 * 1024 + 3);
        std::ostream out(&sink);
        if (throwing) { out.exceptions(std::ios::badbit | std::ios::failbit); }
        bool caught = false;
        try {
          CHECK(!cuvs_copy_snapshot(source.file, expected.size(), out, mapped, try_mapping));
        } catch (const std::ios_base::failure &) { caught = true; }
        CHECK(caught == throwing && sink.accepted == expected.substr(0, 1024 * 1024 + 3));
#ifdef __linux__
        CHECK(mappings == 0);
#endif
      }
      // Copying an explicit prefix must not append the rest of the file.
      std::ostringstream prefix;
      CHECK(cuvs_copy_snapshot(source.file, 5, prefix, mapped, try_mapping));
      CHECK(prefix.str() == expected.substr(0, 5));
    }
#ifdef __linux__
    fail_mapping = true;
    std::ostringstream fallback;
    CHECK(cuvs_copy_snapshot(source.file, expected.size(), fallback, mapped));
    CHECK(!mapped && fallback.str() == expected && mappings == 0);
    fail_mapping = false;
#endif
    std::ostringstream failed;
    failed.setstate(std::ios::badbit);
    CHECK(!cuvs_copy_snapshot(source.file, 1, failed, mapped) && !mapped);
  }
  std::puts("PASS snapshot bytes, chunk/page boundaries, memfd/tmpfile, failed mmap fallback, short input/output and exception cleanup");
}

// A bounded copy sink models the existing stream's memcpy, with no disk writes
// or GPU work. Timings are solely staging-transfer costs, never SQL build times.
struct CopySink : std::streambuf {
  std::vector<char> buffer{std::vector<char>(2 * 1024 * 1024 - 1)};
  size_t total = 0;
  volatile unsigned checksum = 0;
  std::streamsize xsputn(const char *p, std::streamsize n) override {
    const auto count = n;
    while (n > 0) {
      const size_t step = std::min<size_t>(n, buffer.size());
      std::memcpy(buffer.data(), p, step);
      checksum += static_cast<unsigned char>(buffer[0]);
      p += step; n -= step; total += step;
    }
    return count;
  }
};

static void benchmark()
{
  std::puts("MiB,sample,mode,milliseconds");
  for (size_t mb : {64, 256, 512}) {
    const size_t bytes = mb * 1024 * 1024 + 37;
    File source(true);
    std::vector<char> input(1024 * 1024, 41);
    for (size_t left = bytes; left;) {
      const size_t step = std::min(left, input.size());
      CHECK(std::fwrite(input.data(), 1, step, source.file) == step);
      left -= step;
    }
    CHECK(std::fflush(source.file) == 0);
    // One warmup pair, then seven paired samples with alternating order.
    for (int sample = -1; sample < 7; ++sample) {
      for (int k = 0; k < 2; ++k) {
        const bool try_mapping = (k + sample + 2) % 2;
        CopySink sink;
        std::ostream out(&sink);
        bool mapped = false;
        const auto start = std::chrono::steady_clock::now();
        CHECK(cuvs_copy_snapshot(source.file, bytes, out, mapped, try_mapping));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        CHECK(sink.total == bytes && sink.checksum > 0);
        if (sample >= 0) { std::printf("%zu,%d,%s,%.4f\n", mb, sample, mapped ? "mapped" : "buffered", ms); }
      }
    }
  }
}

int main(int argc, char **argv)
{
  if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) { benchmark(); }
  else { correctness(); }
}
