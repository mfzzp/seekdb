#include "cuvs_cpu_budget.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)
namespace fs = std::filesystem;
static void write(const fs::path &path, const char *text) {
  std::ofstream out(path); out << text; out.close(); CHECK(out.good());
}
int main() {
  CHECK(cuvs_cpu_quota_limit("1600000", "100000", 128) == 16);
  CHECK(cuvs_cpu_quota_limit("150000", "100000", 128) == 2);
  CHECK(cuvs_cpu_quota_limit("1", "100000", 128) == 1);
  CHECK(cuvs_cpu_quota_limit("1600000", "100000", 4) == 4);
  CHECK(cuvs_cpu_quota_limit("9223372036854775807", "1", 128) == 128);
  for (const char *quota : {"max", "-1", "0", "", "invalid", "12x", "999999999999999999999999"})
    CHECK(cuvs_cpu_quota_limit(quota, "100000", 7) == 7);
  for (const char *period : {"0", "-1", "", "invalid", "100x", "999999999999999999999999"})
    CHECK(cuvs_cpu_quota_limit("1600000", period, 7) == 7);
  const fs::path root = fs::current_path() / ("cuvs-quota-test-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  CHECK(fs::create_directory(root));
  CHECK(fs::create_directory(root / "group"));
  CHECK(fs::create_directory(root / "group/leaf"));
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/group/leaf", 1, 128) == 128);
  write(root / "cpu.max", "1600000 100000\n");
  write(root / "group/cpu.max", "800000 100000\n");
  write(root / "group/leaf/cpu.max", "max 100000\n");
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/group/leaf", 1, 128) == 8);
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/group/leaf/", 1, 128) == 8);
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/", 1, 128) == 16);
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/not-mounted", 1, 128) == 16);
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/../", 1, 128) == 128);
  write(root / "group/leaf/cpu.max", "150000 100000\n");
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/group/leaf", 1, 128) == 2);
  write(root / "group/leaf/cpu.max", "150000 100000 unexpected\n");
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/group/leaf", 1, 128) == 8);
  write(root / "cpu.cfs_quota_us", "1600000\n");
  write(root / "cpu.cfs_period_us", "100000\n");
  write(root / "group/cpu.cfs_quota_us", "500000\n");
  write(root / "group/cpu.cfs_period_us", "100000\n");
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/group/leaf", 0, 128) == 5);
  write(root / "group/cpu.cfs_quota_us", "-1\n");
  CHECK(cuvs_cpu_walk_quotas(root.string().c_str(), "/group/leaf", 0, 128) == 16);
  for (const auto *name : {"cpu.max", "group/cpu.max", "group/leaf/cpu.max", "cpu.cfs_quota_us",
                           "cpu.cfs_period_us", "group/cpu.cfs_quota_us", "group/cpu.cfs_period_us"})
    CHECK(fs::remove(root / name));
  CHECK(fs::remove(root / "group/leaf")); CHECK(fs::remove(root / "group")); CHECK(fs::remove(root));
  puts("PASS CPU quotas: v1/v2, parent limits, fractional budgets, explicit ceilings, malformed/unlimited/missing values");
}
