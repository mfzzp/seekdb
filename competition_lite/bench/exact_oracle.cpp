// Independent brute-force truth over original float32 data; no engine includes.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char** argv) try {
  if (argc != 6) throw std::runtime_error("VECTOR_FILE PREP_DIR ROWS DIM OUTPUT");
  const size_t n = std::stoull(argv[3]), d = std::stoull(argv[4]);
  const std::string root = argv[2];
  std::vector<float> vectors(n * d), query(d);
  std::ifstream sizes(root + "/masks.u8", std::ios::binary | std::ios::ate);
  const auto mask_bytes = sizes.tellg();
  if (!n || mask_bytes <= 0 || size_t(mask_bytes) % n) throw std::runtime_error("mask length");
  const size_t mask_count = size_t(mask_bytes) / n;
  std::vector<unsigned char> masks(n * mask_count);
  std::ifstream input(argv[1], std::ios::binary), mf(root + "/masks.u8", std::ios::binary);
  if (!input.read(reinterpret_cast<char*>(vectors.data()), vectors.size() * sizeof(float)) ||
      !mf.read(reinterpret_cast<char*>(masks.data()), masks.size())) throw std::runtime_error("input length");
  std::ifstream queries(root + "/queries.f32", std::ios::binary), cases(root + "/cases.tsv");
  std::ofstream out(argv[5]); out << std::setprecision(17);
  int id, mask; size_t k, count = 0;
  while (cases >> id >> mask >> k) {
    if (mask < 0 || size_t(mask) >= mask_count || !k || !queries.read(reinterpret_cast<char*>(query.data()), d * sizeof(float)))
      throw std::runtime_error("case input");
    std::priority_queue<std::pair<double, size_t>> best;
    for (size_t i = 0; i < n; ++i) if (masks[mask * n + i]) {
      const float* v = vectors.data() + i * d;
      double a = 0, b = 0, c = 0, e = 0; size_t j = 0;
      for (; j + 4 <= d; j += 4) {
        double x = double(v[j]) - double(query[j]); a += x * x;
        x = double(v[j+1]) - double(query[j+1]); b += x * x;
        x = double(v[j+2]) - double(query[j+2]); c += x * x;
        x = double(v[j+3]) - double(query[j+3]); e += x * x;
      }
      double distance = a + b + c + e;
      for (; j < d; ++j) { double x = double(v[j]) - double(query[j]); distance += x * x; }
      const std::pair<double, size_t> item{distance, i + 1};
      if (best.size() < k) best.push(item);
      else if (item < best.top()) { best.pop(); best.push(item); }
    }
    std::vector<std::pair<double, size_t>> result;
    while (!best.empty()) { result.push_back(best.top()); best.pop(); }
    std::reverse(result.begin(), result.end());
    out << "{\"id\":" << id << ",\"rows\":[";
    for (size_t i = 0; i < result.size(); ++i) {
      if (i) out << ',';
      out << '[' << result[i].second << ',' << std::sqrt(result[i].first) << ']';
    }
    out << "]}\n";
    if (++count % 64 == 0) std::cerr << "oracle " << count << '\n';
  }
  if (!out || !count) throw std::runtime_error("no output");
  std::cerr << "oracle complete " << count << '\n';
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
