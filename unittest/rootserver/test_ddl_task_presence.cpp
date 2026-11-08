// Standalone regression for the scheduler presence check used by FTS parents.
#include "rootserver/ddl_task/ob_ddl_scheduler.h"
#include <atomic>
#include <cstdio>
#include <thread>

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::rootserver;

namespace
{
class TestDDLTask : public ObDDLTask
{
public:
  TestDDLTask() : ObDDLTask(share::DDL_CREATE_INDEX)
  {
    task_id_ = 42;
    target_object_id_ = 500001;
    dst_schema_version_ = 1;
    is_inited_ = true;
  }
};
}

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

int main()
{
  oceanbase::rootserver::ObDDLTaskQueue queue;
  const ObDDLTaskID id(42);
  bool exists = true;
  CHECK(OB_NOT_INIT == queue.check_task_exists(id, exists));
  CHECK(!exists);
  CHECK(OB_SUCCESS == queue.init(16));
  queue.set_stop(false);
  CHECK(OB_INVALID_ARGUMENT == queue.check_task_exists(ObDDLTaskID(), exists));
  CHECK(!exists);
  CHECK(OB_SUCCESS == queue.check_task_exists(id, exists));
  CHECK(!exists);

  TestDDLTask task;
  ObDDLTask *running = nullptr;
  CHECK(OB_SUCCESS == queue.push_task(&task));
  CHECK(OB_SUCCESS == queue.check_task_exists(id, exists) && exists);
  CHECK(OB_SUCCESS == queue.get_next_task(running) && running == &task);
  // A running child has left the work list but must still be found by ID.
  CHECK(0 == queue.get_task_cnt());
  CHECK(OB_SUCCESS == queue.check_task_exists(id, exists) && exists);
  CHECK(OB_SUCCESS == queue.add_task_to_last(running));
  CHECK(OB_SUCCESS == queue.get_next_task(running));
  CHECK(OB_SUCCESS == queue.remove_task(running));
  CHECK(OB_SUCCESS == queue.check_task_exists(id, exists) && !exists);

  std::atomic<bool> done(false);
  std::atomic<int> errors(0);
  std::thread reader([&]() {
    while (!done.load()) {
      bool found = false;
      if (OB_SUCCESS != queue.check_task_exists(id, found)) {
        ++errors;
      }
    }
  });
  for (int i = 0; i < 1000; ++i) {
    if (OB_SUCCESS != queue.push_task(&task)
        || OB_SUCCESS != queue.get_next_task(running)
        || OB_SUCCESS != queue.remove_task(running)) {
      ++errors;
      break;
    }
  }
  done.store(true);
  reader.join();
  CHECK(0 == errors.load());
  CHECK(OB_SUCCESS == queue.check_task_exists(id, exists) && !exists);
  queue.destroy();
  std::puts("DDL task presence lifecycle and concurrent checks passed");
  return 0;
}
