// Standalone diagnostic: full FP32 inputs, exact CPU truth; NOT SQL score.
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
using Clock = std::chrono::steady_clock;
static void check(bool ok, const char* s) {
  if (!ok) throw std::runtime_error(s);
}
static double ms(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
static double distance(const float* a, const float* b, long dim) {
  double s = 0;
  for (long d = 0; d < dim; ++d) {
    double x = double(a[d]) - b[d];
    s += x * x;
  }
  return s;
}
template <class T>
T symbol(void* lib, const char* name) {
  auto p = dlsym(lib, name);
  check(p != nullptr, name);
  return reinterpret_cast<T>(p);
}
int main(int argc, char** argv) {
  if (argc != 7) {
    std::fprintf(stderr,
                 "usage: %s BRIDGE ROWS DIM QUERIES "
                 "uniform|gaussian|sphere|anisotropic|file:PREFIX NEW_SNAPSHOT\n",
                 argv[0]);
    return 2;
  }
  try {
    long n = std::stol(argv[2]), dim = std::stol(argv[3]), nq = std::stol(argv[4]);
    std::string kind = argv[5];
    check(n >= 100 && n <= 1000000 && dim >= 1 && dim <= 1024 && nq >= 1 && nq <= 4096,
          "bad dimensions");
    check(kind == "uniform" || kind == "gaussian" || kind == "sphere" || kind == "anisotropic" ||
              kind.rfind("file:", 0) == 0,
          "unknown distribution");
    std::vector<float> data(n * dim), queries(nq * dim);
    std::mt19937 rng(371912);
    std::normal_distribution<float> gaussian(0, 1);
    std::uniform_real_distribution<float> uniform(-1, 1);
    auto fill = [&](std::vector<float>& rows) {
      for (size_t i = 0; i < rows.size(); ++i)
        rows[i] = kind == "uniform" ? uniform(rng) : gaussian(rng);
    };
    if (kind.rfind("file:", 0) == 0) {
      const std::string prefix = kind.substr(5);
      std::ifstream base(prefix + ".f32", std::ios::binary),
          query(prefix + ".queries.f32", std::ios::binary);
      check(bool(base.read(reinterpret_cast<char*>(data.data()), data.size() * sizeof(float))),
            "read base file");
      check(
          bool(query.read(reinterpret_cast<char*>(queries.data()), queries.size() * sizeof(float))),
          "read query file");
    } else {
      fill(data);
      fill(queries);
    }
    if (kind == "sphere") {
      for (auto* v : {&data, &queries})
        for (size_t r = 0; r < v->size() / dim; ++r) {
          double norm = 0;
          for (long d = 0; d < dim; ++d) norm += double((*v)[r * dim + d]) * (*v)[r * dim + d];
          for (long d = 0; d < dim; ++d) (*v)[r * dim + d] /= std::sqrt(norm);
        }
    }
    if (kind == "anisotropic") {
      for (auto* v : {&data, &queries})
        for (size_t i = 0; i < v->size(); ++i) (*v)[i] *= std::exp(-3.0 * (i % dim) / dim);
    }
    std::vector<unsigned> truth(nq * 100);
#pragma omp parallel for num_threads(8)
    for (long q = 0; q < nq; ++q) {
      std::vector<std::pair<double, unsigned>> ranked(n);
      for (long r = 0; r < n; ++r)
        ranked[r] = {distance(queries.data() + q * dim, data.data() + r * dim, dim), (unsigned)r};
      std::partial_sort(ranked.begin(), ranked.begin() + 100, ranked.end());
      for (long k = 0; k < 100; ++k) truth[q * 100 + k] = ranked[k].second;
    }
    if (const char* prefix = std::getenv("STUDY_EXPORT_PREFIX")) {
      std::ofstream b(std::string(prefix) + ".f32", std::ios::binary),
          q(std::string(prefix) + ".queries.f32", std::ios::binary),
          t(std::string(prefix) + ".truth.u32", std::ios::binary);
      check(bool(b.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(float))),
            "write base file");
      check(bool(q.write(reinterpret_cast<const char*>(queries.data()),
                         queries.size() * sizeof(float))),
            "write query file");
      check(bool(t.write(reinterpret_cast<const char*>(truth.data()),
                         truth.size() * sizeof(unsigned))),
            "write truth file");
      if (std::getenv("STUDY_EXPORT_ONLY")) return 0;
    }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      std::fprintf(stderr, "%s\n", dlerror());
      return 1;
    }
    auto build = symbol<void* (*)(const float*, long, long)>(lib, "seekdb_cuvs_build");
    auto search = symbol<int (*)(void*, const float*, long, long, unsigned*, float*)>(
        lib, "seekdb_cuvs_search");
    auto release = symbol<void (*)(void*)>(lib, "seekdb_cuvs_free");
    auto save = symbol<int (*)(void*, const char*)>(lib, "seekdb_cuvs_serialize");
    auto load = symbol<void* (*)(const char*, long, long)>(lib, "seekdb_cuvs_deserialize");
    auto export_data = reinterpret_cast<int (*)(void*, float*, long, long)>(
        dlsym(lib, "seekdb_cuvs_export_vectors"));
    auto start = Clock::now();
    void* index = build(data.data(), n, dim);
    check(index, "build failed");
    double build_ms = ms(start);
    start = Clock::now();
    check(save(index, argv[6]) == 0, "save failed");
    double save_ms = ms(start), load_ms = 0;
    for (int phase = 0; phase < 2; ++phase) {
      if (phase) {
        release(index);
        start = Clock::now();
        index = load(argv[6], n, dim);
        check(index, "load failed");
        load_ms = ms(start);
      }
      if (export_data) {
        std::vector<float> restored(data.size());
        check(export_data(index, restored.data(), n, dim) == 0, "export failed");
        check(memcmp(data.data(), restored.data(), data.size() * sizeof(float)) == 0,
              "FP32 changed");
      }
      for (long topk : {10, 100}) {
        long hits = 0;
        double total = 0;
        std::vector<double> latencies;
        std::vector<unsigned> ids(topk + 2);
        std::vector<float> dst(topk + 2);
        for (long q = 0; q < nq; ++q) {
          std::fill(ids.begin(), ids.end(), UINT32_MAX);
          std::fill(dst.begin(), dst.end(), -1);
          start = Clock::now();
          check(
              search(index, queries.data() + q * dim, 1, topk, ids.data() + 1, dst.data() + 1) == 0,
              "search failed");
          double latency = ms(start);
          total += latency;
          latencies.push_back(latency);
          check(ids.front() == UINT32_MAX && ids.back() == UINT32_MAX && dst.front() == -1 &&
                    dst.back() == -1,
                "output overrun");
          std::set<unsigned> unique;
          for (long k = 1; k <= topk; ++k) {
            check(ids[k] < (unsigned)n && unique.insert(ids[k]).second, "bad or duplicate id");
            double exact = distance(queries.data() + q * dim, data.data() + ids[k] * dim, dim);
            check(std::isfinite(dst[k]) && std::abs(dst[k] - exact) < .001 * (1 + exact),
                  "distance mismatch");
            hits += std::find(truth.begin() + q * 100, truth.begin() + q * 100 + topk, ids[k]) !=
                    truth.begin() + q * 100 + topk;
          }
        }
        std::sort(latencies.begin(), latencies.end());
        std::printf(
            "n=%ld dim=%ld nq=%ld kind=%s phase=%d topk=%ld recall=%.9f build_ms=%.3f save_ms=%.3f "
            "load_ms=%.3f search_ms=%.3f bridge_qps=%.3f p50_ms=%.3f p95_ms=%.3f max_ms=%.3f "
            "verified_ids_distances=1\n",
            n, dim, nq, kind.c_str(), phase, topk, double(hits) / (nq * topk), build_ms, save_ms,
            load_ms, total, nq * 1000 / total, latencies[nq / 2],
            latencies[std::min(nq - 1, nq * 95 / 100)], latencies.back());
        std::fflush(stdout);
      }
    }
    release(index);
    dlclose(lib);
    check(std::remove(argv[6]) == 0, "snapshot cleanup failed");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
}
