#include "lib/vector/cuvs_row_batch.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <limits>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::abort(); } } while (0)
using oceanbase::common::obvsag::CuvsRowBatch;

int main()
{
  CHECK(CuvsRowBatch::capacity(0) == 0);
  CHECK(CuvsRowBatch::capacity(-1) == 0);
  CHECK(CuvsRowBatch::capacity(std::numeric_limits<int64_t>::max()) == 0);
  CHECK(CuvsRowBatch::capacity(262145) == 0);
  for (int dim : {1, 3, 4, 128, 256, 1024, 4097, 262144}) {
    const int capacity = CuvsRowBatch::capacity(dim);
    CHECK(capacity > 0 && capacity <= 256 && size_t(capacity) * dim * sizeof(float) <= 1024 * 1024);
    std::vector<float> storage(size_t(capacity) * dim + 2, -5.0f);
    std::vector<int64_t> ids(capacity + 2, -5);
    // Arbitrary bytes include signed zero, NaNs and infinity. No float arithmetic
    // or reinterpretation is permitted when buffering the original FP32 input.
    std::vector<unsigned char> input(size_t(dim) * sizeof(float) + 1);
    CuvsRowBatch batch(storage.data() + 1, ids.data() + 1, dim);
    CHECK(batch.count() == 0 && !batch.full());
    CHECK(!batch.append(nullptr, dim * sizeof(float), 0));
    CHECK(!batch.append(input.data(), dim * sizeof(float) - 1, 0));
    CHECK(!batch.append(input.data(), dim * sizeof(float) + 1, 0));
    for (int round = 0; round < 3; ++round) {
      for (int row = 0; row < capacity; ++row) {
        for (size_t i = 1; i < input.size(); ++i) input[i] = static_cast<unsigned char>(i * 17 + row + round);
        const int64_t id = (INT64_C(1) << 40) - row * 97;
        CHECK(batch.append(input.data() + 1, input.size() - 1, -id));
        CHECK(std::memcmp(storage.data() + 1 + size_t(row) * dim, input.data() + 1, input.size() - 1) == 0);
        std::memset(input.data(), 0, input.size());
        CHECK(reinterpret_cast<const unsigned char *>(storage.data() + 1 + size_t(row) * dim)[0] ==
              static_cast<unsigned char>(17 + row + round));
        CHECK(ids[row + 1] == -id && batch.count() == row + 1);
      }
      CHECK(batch.full());
      CHECK(!batch.append(input.data() + 1, input.size() - 1, 1));
      CHECK(storage.front() == -5 && storage.back() == -5 && ids.front() == -5 && ids.back() == -5);
      batch.reset();
      CHECK(batch.count() == 0 && !batch.full());
    }
    const int tail = capacity > 1 ? capacity - 1 : 1;
    for (int row = 0; row < tail; ++row) { CHECK(batch.append(input.data() + 1, input.size() - 1, row)); }
    CHECK(batch.count() == tail && batch.full() == (tail == capacity));
    batch.reset();
    CHECK(batch.count() == 0);
  }
  std::puts("PASS dense batches: byte-exact unaligned input, bounded memory, 64-bit IDs, shapes, full/partial reuse and invalid inputs");
}
