#include "lib/vector/cuvs_worker.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "check failed line %d: %s\n", __LINE__, #condition); std::abort(); } } while (0)
using oceanbase::common::obvsag::CuvsWorker;
struct Job { int input; int output = 0; pthread_t thread{}; };
static void *work(void *argument)
{
  Job *job = static_cast<Job *>(argument);
  job->output = job->input * 3 + 7;
  job->thread = pthread_self();
  return nullptr;
}
static void *fail(void *) { throw std::runtime_error("injected job failure"); }

int main()
{
  { CuvsWorker never_started; CHECK(!never_started.run(nullptr, nullptr)); }
  CuvsWorker worker;
  Job first{1}; CHECK(worker.run(work, &first));
  CHECK(!pthread_equal(first.thread, pthread_self()));
  for (int i = 0; i < 1000; ++i) {
    Job job{i}; CHECK(worker.run(work, &job));
    CHECK(job.output == i * 3 + 7 && pthread_equal(first.thread, job.thread));
  }
  std::vector<std::thread> callers;
  for (int t = 0; t < 8; ++t) {
    callers.emplace_back([&worker, &first, t] {
      for (int i = 0; i < 100; ++i) {
        Job job{t * 100 + i}; CHECK(worker.run(work, &job));
        CHECK(job.output == job.input * 3 + 7 && pthread_equal(first.thread, job.thread));
      }
    });
  }
  for (auto &caller : callers) { caller.join(); }
  CHECK(!worker.run(fail, nullptr));
  Job recovered{93}; CHECK(worker.run(work, &recovered)); CHECK(recovered.output == 286);
  for (int i = 0; i < 50; ++i) { CuvsWorker short_lived; Job job{i}; CHECK(short_lived.run(work, &job)); }

  // Host-only dispatch measurement, never reported as GPU or SQL performance.
  constexpr int rounds = 1000;
  auto measure = [&](bool reuse) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) {
      Job job{i};
      if (reuse) { CHECK(worker.run(work, &job)); }
      else {
        pthread_t thread; pthread_attr_t attr;
        CHECK(pthread_attr_init(&attr) == 0);
        CHECK(pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024) == 0);
        CHECK(pthread_create(&thread, &attr, work, &job) == 0);
        CHECK(pthread_attr_destroy(&attr) == 0);
        CHECK(pthread_join(thread, nullptr) == 0);
      }
      CHECK(job.output == i * 3 + 7);
    }
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / rounds;
  };
  const double cold = measure(false), reused = measure(true);
  std::printf("PASS worker: 1800 varying/concurrent jobs, exception recovery, 50 lifecycles; host-only dispatch cold_us=%.3f reused_us=%.3f\n", cold, reused);
}
