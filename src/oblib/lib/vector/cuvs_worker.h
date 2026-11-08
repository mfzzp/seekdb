/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <condition_variable>
#include <mutex>
#include <pthread.h>

namespace oceanbase {
namespace common {
namespace obvsag {

// Keep the large stack required by hipVS, without creating/joining a thread for
// every SQL probe. A worker belongs to one live index, never to cached results.
class CuvsWorker {
public:
  CuvsWorker() = default;
  ~CuvsWorker()
  {
    std::lock_guard<std::mutex> submit(submit_mutex_);
    if (started_) {
      {
        std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
        wake_.notify_one();
      }
      pthread_join(thread_, nullptr);
    }
  }
  CuvsWorker(const CuvsWorker &) = delete;
  CuvsWorker &operator=(const CuvsWorker &) = delete;

  bool run(void *(*function)(void *), void *argument)
  {
    if (function == nullptr) { return false; }
    std::lock_guard<std::mutex> submit(submit_mutex_);
    std::unique_lock<std::mutex> guard(mutex_);
    if (!started_) {
      pthread_attr_t attr;
      if (pthread_attr_init(&attr) != 0) { return false; }
      int rc = pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024);
      if (rc == 0) { rc = pthread_create(&thread_, &attr, &CuvsWorker::main, this); }
      pthread_attr_destroy(&attr);
      if (rc != 0) { return false; }
      started_ = true;
    }
    function_ = function;
    argument_ = argument;
    done_ = false;
    pending_ = true;
    wake_.notify_one();
    completed_.wait(guard, [this] { return done_; });
    return succeeded_;
  }

private:
  static void *main(void *argument)
  {
    CuvsWorker *worker = static_cast<CuvsWorker *>(argument);
    std::unique_lock<std::mutex> guard(worker->mutex_);
    for (;;) {
      worker->wake_.wait(guard, [worker] { return worker->pending_ || worker->stopping_; });
      if (worker->stopping_) { break; }
      auto function = worker->function_;
      void *arg = worker->argument_;
      worker->pending_ = false;
      guard.unlock();
      bool succeeded = true;
      try {
        function(arg);
      } catch (...) {
        succeeded = false;
      }
      guard.lock();
      worker->succeeded_ = succeeded;
      worker->done_ = true;
      worker->completed_.notify_one();
    }
    return nullptr;
  }

  std::mutex submit_mutex_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable completed_;
  pthread_t thread_{};
  void *(*function_)(void *) = nullptr;
  void *argument_ = nullptr;
  bool started_ = false;
  bool stopping_ = false;
  bool pending_ = false;
  bool done_ = false;
  bool succeeded_ = false;
};

} // namespace obvsag
} // namespace common
} // namespace oceanbase
