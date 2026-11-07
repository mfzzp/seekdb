/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <atomic>
#include "lib/task/ob_timer.h"

namespace oceanbase
{
namespace common { class ObMySQLProxy; }
namespace observer
{
// The catalog is the queue: claims and completed results survive server restarts.
// Model calls never run in the publishing transaction or on a business thread.
class AiColumnWorker final : public common::ObTimerTask
{
public:
  AiColumnWorker() : proxy_(nullptr), stopped_(true) {}
  ~AiColumnWorker() override { destroy(); }
  int start(common::ObMySQLProxy &proxy);
  void stop();
  void destroy();
  void runTimerTask() override;
private:
  // Separate timers allow bounded parallel model calls; the durable lease is
  // still the authority on which runner owns a task.
  static constexpr int64_t RUNNERS = 2;
  common::ObTimer timers_[RUNNERS];
  common::ObMySQLProxy *proxy_;
  std::atomic<bool> stopped_;
  DISALLOW_COPY_AND_ASSIGN(AiColumnWorker);
};
} // namespace observer
} // namespace oceanbase
