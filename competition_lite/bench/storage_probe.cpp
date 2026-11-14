// Standalone non-destructive file benchmark, excluded from database runtime.
// storage_probe ROOT seq MIB DIRECT JOBS | ROOT commit ROWS DIM BATCH
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;
static double seconds(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}
static void check(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}
struct File {
  std::string path;
  int fd;
  File(const std::string& root, const std::string& name, bool direct = false)
      : path(root + "/" + name + "-" + std::to_string(getpid()) + ".bin") {
    fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | (direct ? O_DIRECT : 0), 0600);
    check(fd >= 0, "create benchmark file");
  }
  ~File() { close(fd); unlink(path.c_str()); }
};
struct Buffer {
  char* p = nullptr;
  explicit Buffer(size_t n) {
    check(posix_memalign(reinterpret_cast<void**>(&p), 4096, n) == 0, "aligned allocation");
    uint32_t state = 1739;
    for (size_t i = 0; i < n; ++i) {
      state ^= state << 13; state ^= state >> 17; state ^= state << 5;
      p[i] = static_cast<char>(state);
    }
  }
  ~Buffer() { free(p); }
};
static void put(int fd, const char* data, size_t n, off_t off) {
  while (n) {
    ssize_t z = pwrite(fd, data, n, off);
    if (z < 0 && errno == EINTR) continue;
    check(z > 0, "write");
    data += z; n -= z; off += z;
  }
}
int main(int argc, char** argv) try {
  if (argc < 6) throw std::runtime_error("ROOT seq MIB DIRECT JOBS | ROOT commit ROWS DIM BATCH");
  const std::string root = std::filesystem::canonical(argv[1]).string();
  if (root.find("/zzp/") == std::string::npos)
    throw std::runtime_error("benchmark root must be inside zzp");
  const std::string mode = argv[2];
  if (mode == "seq") {
    const size_t bytes = std::stoull(argv[3]) * 1024 * 1024, block = 1024 * 1024;
    const bool direct = std::stoi(argv[4]);
    const int jobs = std::stoi(argv[5]);
    if (jobs < 1 || jobs > 16 || bytes % (block * jobs)) throw std::runtime_error("invalid size/jobs");
    File file(root, "seq", direct);
    Buffer data(block);
    std::vector<std::thread> workers;
    std::vector<std::exception_ptr> errors(jobs);
    auto start = Clock::now();
    for (int j = 0; j < jobs; ++j) workers.emplace_back([&, j] {
      try {
        for (size_t off = bytes / jobs * j; off < bytes / jobs * (j + 1); off += block)
          put(file.fd, data.p, block, off);
      } catch (...) { errors[j] = std::current_exception(); }
    });
    for (auto& t : workers) t.join();
    for (auto& e : errors) if (e) std::rethrow_exception(e);
    const double submit_s = seconds(start);
    check(fdatasync(file.fd) == 0, "fdatasync");
    const double total_s = seconds(start);
    std::cout << "{\"mode\":\"seq\",\"direct\":" << direct << ",\"jobs\":" << jobs
              << ",\"bytes\":" << bytes << ",\"submit_s\":" << submit_s
              << ",\"durable_s\":" << total_s << ",\"MB_s\":" << bytes / total_s / 1e6 << "}\n";
  } else if (mode == "commit") {
    const size_t rows = std::stoull(argv[3]), dim = std::stoull(argv[4]), batch = std::stoull(argv[5]);
    if (!rows || !dim || !batch) throw std::runtime_error("zero input");
    File vectors(root, "vectors"), wal(root, "wal");
    Buffer data(std::max(batch * dim * 4, batch * 128 + 128));
    std::vector<double> latency;
    off_t voff = 0, woff = 0;
    auto start = Clock::now();
    for (size_t i = 0; i < rows; i += batch) {
      const size_t n = std::min(batch, rows - i), vb = n * dim * 4, wb = n * 128 + 128;
      auto cstart = Clock::now();
      put(vectors.fd, data.p, vb, voff); voff += vb;
      check(fdatasync(vectors.fd) == 0, "sync vectors");
      put(wal.fd, data.p, wb, woff); woff += wb;
      check(fdatasync(wal.fd) == 0, "sync wal");
      latency.push_back(seconds(cstart));
    }
    const double total = seconds(start);
    std::sort(latency.begin(), latency.end());
    std::cout << "{\"mode\":\"commit\",\"rows\":" << rows << ",\"dim\":" << dim
              << ",\"batch\":" << batch << ",\"commits\":" << latency.size()
              << ",\"bytes\":" << voff + woff << ",\"seconds\":" << total
              << ",\"rows_s\":" << rows / total << ",\"mean_commit_ms\":" << total * 1e3 / latency.size()
              << ",\"p50_commit_ms\":" << latency[latency.size() / 2] * 1e3
              << ",\"p95_commit_ms\":" << latency[std::min(latency.size() - 1, latency.size() * 95 / 100)] * 1e3 << "}\n";
  } else throw std::runtime_error("unknown mode");
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
