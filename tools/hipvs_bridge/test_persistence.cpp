#include "seekdb_cuvs_bridge.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <set>

int main(int argc, char **argv)
{
  if (argc != 3) return 2;
  const long count = 10000;
  const long dimension = 32;
  std::vector<float> vectors(count * dimension);
  for (long row = 0; row < count; ++row) {
    for (long column = 0; column < dimension; ++column) {
      vectors[row * dimension + column] = std::sin(float(row * 31 + column * 17));
    }
  }
  void *index = nullptr;
  if (std::strcmp(argv[1], "write") == 0) {
    index = seekdb_cuvs_build(vectors.data(), count, dimension);
    if (!index || seekdb_cuvs_serialize(index, argv[2]) != 0) return 3;
  } else {
    void *invalid = seekdb_cuvs_deserialize(argv[2], count - 1, dimension);
    if (invalid) { seekdb_cuvs_free(invalid); return 6; }
    invalid = seekdb_cuvs_deserialize(argv[2], count, dimension + 1);
    if (invalid) { seekdb_cuvs_free(invalid); return 7; }
    index = seekdb_cuvs_deserialize(argv[2], count, dimension);
    if (!index) return 4;
  }
  // Exercise growing/shrinking scratch space, different queries and Top-100
  // after both build and reload. Real GPU execution is required for this test.
  for (const long topk : {10L, 100L, 200L, 10L}) {
    for (const long nq : {1L, 3L}) {
      const long start_row = topk + nq;
      std::vector<unsigned int> neighbors(nq * topk + 2, UINT32_MAX);
      std::vector<float> distances(nq * topk + 2, -1.0f);
      if (seekdb_cuvs_search(index, vectors.data() + start_row * dimension, nq, topk,
                            neighbors.data() + 1, distances.data() + 1) != 0) {
        seekdb_cuvs_free(index); return 8;
      }
      if (neighbors.front() != UINT32_MAX || neighbors.back() != UINT32_MAX ||
          distances.front() != -1.0f || distances.back() != -1.0f) {
        seekdb_cuvs_free(index); return 9;
      }
      for (long row = 0; row < nq; ++row) {
        std::set<unsigned int> distinct;
        bool found_self = false;
        for (long k = 0; k < topk; ++k) {
          const auto offset = 1 + row * topk + k;
          if (neighbors[offset] >= count || !std::isfinite(distances[offset]) ||
              !distinct.insert(neighbors[offset]).second) {
            seekdb_cuvs_free(index); return 10;
          }
          if (neighbors[offset] == static_cast<unsigned>(start_row + row) &&
              std::fabs(distances[offset]) <= 0.0001f) { found_self = true; }
        }
        if (!found_self) {
          seekdb_cuvs_free(index); return 11;
        }
      }
    }
  }
  unsigned int ids[10];
  float distances[10];
  int result = seekdb_cuvs_search(index, vectors.data(), 1, 10, ids, distances);
  seekdb_cuvs_free(index);
  if (result != 0 || ids[0] != 0 || std::fabs(distances[0]) > 0.0001f) return 5;
  std::printf("%s: n=%ld top1=%u distance=%g\n", argv[1], count, ids[0], distances[0]);
  return 0;
}
