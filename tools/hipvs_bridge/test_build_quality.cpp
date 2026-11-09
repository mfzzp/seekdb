// Real hipVS integration check; link against the production bridge, never doubles.
// Small synthetic fixtures diagnose build/reload quality. They are not the
// official datasets and their timings/Recall cannot establish a contest score.
#include "seekdb_cuvs_bridge.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Index = std::unique_ptr<void, decltype(&seekdb_cuvs_free)>;
using Clock = std::chrono::steady_clock;
static void require(bool condition, const char *message)
{
  if (!condition) { throw std::runtime_error(message); }
}
static float random_value(uint32_t &state)
{
  state ^= state << 13; state ^= state >> 17; state ^= state << 5;
  return static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
}
static double distance(const float *a, const float *b, long dim)
{
  double result = 0;
  for (long d = 0; d < dim; ++d) {
    const double delta = static_cast<double>(a[d]) - b[d];
    result += delta * delta;
  }
  return result;
}

static void fixture(long n, long dim, bool clustered, const std::string &path)
{
  constexpr long probes = 64;
  uint32_t state = 0x3217a8d9u;
  std::vector<float> vectors(static_cast<size_t>(n) * dim), queries(probes * dim);
  std::vector<float> centers(32 * dim);
  for (auto &v : centers) { v = random_value(state); }
  for (long row = 0; row < n; ++row) {
    for (long d = 0; d < dim; ++d) {
      vectors[row * dim + d] = clustered ? centers[(row % 32) * dim + d] + 0.2f * random_value(state)
                                        : random_value(state);
    }
  }
  for (long q = 0; q < probes; ++q) {
    for (long d = 0; d < dim; ++d) {
      // Include both nearby probes and independent inputs, not just self-hits.
      queries[q * dim + d] = q % 2 == 0 ? vectors[((q * 173 + 77) % n) * dim + d] + 0.01f * random_value(state)
                                        : random_value(state);
    }
  }
  // Ground truth always scans every original FP32 row, with double accumulation.
  std::vector<std::vector<unsigned>> truth(probes);
  for (long q = 0; q < probes; ++q) {
    std::vector<std::pair<double, unsigned>> ranked;
    ranked.reserve(n);
    for (long row = 0; row < n; ++row) {
      ranked.emplace_back(distance(queries.data() + q * dim, vectors.data() + row * dim, dim),
                          static_cast<unsigned>(row));
    }
    std::partial_sort(ranked.begin(), ranked.begin() + 100, ranked.end());
    for (long k = 0; k < 100; ++k) { truth[q].push_back(ranked[k].second); }
  }
  auto begin = Clock::now();
  Index index(seekdb_cuvs_build(vectors.data(), n, dim), seekdb_cuvs_free);
  require(index != nullptr, "build failed");
  const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
  begin = Clock::now();
  require(seekdb_cuvs_serialize(index.get(), path.c_str()) == 0, "serialize failed");
  const double serialize_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
  for (int phase = 0; phase < 2; ++phase) {
    if (phase == 1) {
      index.reset();
      index.reset(seekdb_cuvs_deserialize(path.c_str(), n, dim));
      require(index != nullptr, "reload failed");
    }
    for (long topk : {10L, 100L}) {
      size_t hits = 0;
      begin = Clock::now();
      for (long q = 0; q < probes; ++q) {
        std::vector<unsigned> ids(topk + 2, UINT32_MAX);
        std::vector<float> distances(topk + 2, -1.0f);
        require(seekdb_cuvs_search(index.get(), queries.data() + q * dim, 1, topk,
                                  ids.data() + 1, distances.data() + 1) == 0, "search failed");
        require(ids.front() == UINT32_MAX && ids.back() == UINT32_MAX &&
                distances.front() == -1 && distances.back() == -1, "output overrun");
        std::set<unsigned> distinct;
        for (long k = 1; k <= topk; ++k) {
          require(ids[k] < static_cast<unsigned>(n) && distinct.insert(ids[k]).second,
                  "invalid or duplicate result ID");
          const double exact = distance(queries.data() + q * dim, vectors.data() + ids[k] * dim, dim);
          require(std::isfinite(distances[k]) && std::fabs(distances[k] - exact) <= 0.001 * (1 + exact),
                  "result ID and L2 distance disagree");
          hits += std::find(truth[q].begin(), truth[q].begin() + topk, ids[k]) != truth[q].begin() + topk;
        }
      }
      const double search_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
      const double recall = static_cast<double>(hits) / (probes * topk);
      std::printf("n=%ld dim=%ld clustered=%d phase=%s topk=%ld recall=%.6f build_ms=%.3f serialize_ms=%.3f search_and_check_ms=%.3f\n",
                  n, dim, clustered, phase == 0 ? "built" : "reloaded", topk, recall, build_ms, serialize_ms, search_ms);
      std::fflush(stdout);
      // Diagnostic fixture threshold only; official paired Recall gates still apply.
      require(recall >= 0.95, "fixture Recall below 0.95");
    }
  }
  index.reset();
  require(std::remove(path.c_str()) == 0, "failed to remove completed fixture snapshot");
}

int main(int argc, char **argv)
{
  if (argc != 2) { std::fprintf(stderr, "usage: %s /path/to/new-snapshot-prefix\n", argv[0]); return 2; }
  try {
    const std::string prefix(argv[1]);
    for (long n : {256L, 512L, 513L}) { fixture(n, 33, false, prefix + "-" + std::to_string(n) + ".bin"); }
    fixture(8193, 128, false, prefix + "-uniform.bin");
    fixture(8193, 256, true, prefix + "-clustered.bin");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  std::puts("PASS synthetic GPU build/reload quality; official paired Recall/performance still required");
}
