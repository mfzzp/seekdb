# FTS build completion tracking

## Observed overhead

`ObFtsIndexBuildTask::wait_aux_table_complement()` previously queried the
durable DDL result table on each scheduling pass for every unfinished child.
The local scheduler already knows whether that child is queued or executing,
and children already report completion through `on_ddl_task_finish()`.
Local 20,000-document builds generated thousands of unsuccessful result-table
lookups while their children were still executing.

This observation does **not** establish the cause of the platform's roughly
three-second build-time jumps. The statistics synchronization retry and DAG
cancellation delays inspected during this investigation are not on the normal
successful FTS build path. No retry delays, DDL parallelism, sort memory limits,
benchmark inputs or scoring rules are changed.

## State-dependent choice

For FTS parents with an unknown child result:

1. If the child is present in the scheduler's task-ID map, wait for the existing
   completion callback without querying the durable result table.
2. If the child is absent, use the original durable lookup. This covers recovered
   parents and a callback arriving before the dependency was registered.
3. Cache the recovered result in the dependency map. A failed child still fails
   the parent; missing results remain pending and lookup errors still propagate.

The ID map includes executing tasks that have already left the work list. The
new lookup holds the queue lock only while checking presence and does not
expose a task pointer. The parent copies its dependency states under its own
read lock, then releases that lock before accessing the scheduler or SQL. This
avoids reversing the scheduler-to-parent lock order used by callbacks.
Repeated identical completion reports are accepted; conflicting reports are
rejected instead of silently overwriting a known result.

`ObIndexBuildTask::cleanup_impl()` persists its result before notifying the
parent and before the scheduler removes it. A child still cleaning up therefore
remains active. The fast check is gated by `is_fts_task()`; multivalue and sparse
vector builds retain durable polling.

The existing query optimization remains unchanged: eligible membership-only
queries use streaming posting-list merging without BM25; queries needing scores,
ranking or lookup retain scoring, and large term lists retain the existing
TAAT fallback. These choices depend on semantics and execution state, not table
names, language-specific benchmark cases or previously measured timings.

## Validation on 2026-11-08

Control: `ccdc1059f95750dd711fde43ac7c237305c0fcfe`.
Both Windows executables used the same existing development build and rebuilt
rootserver translation unit, with identical prior FTS objects and dependencies.
Clang/LLVM 21 compilation, linking, fresh instance bootstrap and SELECT 1 passed.
This was an incremental Windows build, not a clean Linux build.

- `unittest/rootserver/test_ddl_task_presence.cpp`: standalone executable linked
  against the kernel libraries with the server main replaced. Initialization,
  invalid/missing IDs, queued/executing/requeued/removed states and 1,000
  concurrent registration/removal cycles passed.
- `fts_index.fts_build_completion`: 33 statements and 12 assertions passed on
  both versions. Covers empty/populated builds, multiple indexes, reused DocID
  mapping, drop/rebuild, table recreation, DML rollback and an ordinary index.
- Existing `fts_index.fts_score_free_filter`: 46 statements and 24 assertions
  passed on both versions.
- Four concurrent builds (space, BENG, IK, ngram), 1,000 documents per table,
  completed with the expected full hit count and primary-key sum on both versions.
- With the unmodified 20,000-document generator, all four benchmark query counts
  matched (8001, 11000, 7332, 20). Complete matching IDs and numeric scores for
  the three count searches, and the returned LIMIT IDs, matched between versions.
- Additional multilingual searches also matched complete IDs and scores.

Two disposable, validation-only instances used an instrumented binary. Neither
the instrumentation nor its environment switches are in the submitted source:

- Dropped every `ObDDLScheduler::on_ddl_task_finish()` callback: all 12 build
  regression assertions still passed through durable-result recovery.
- Also forced a child `OB_ERR_UNEXPECTED` before `report_error_code()` in
  `ObIndexBuildTask::cleanup_impl()`: the parent returned SQL error 4016 in about
  0.62 seconds, retained all 100 base rows, removed the failed FTS index and allowed
  an ordinary index to be built afterward. It did not report success or time out.

Actual process-restart recovery on Linux remains to be tested; missing-callback
injection exercises the durable fallback but does not replace a restart test.

## Performance limits

Local timing uses persistent PyMySQL connections on Windows, the unmodified
20,000-row dataset and the three original index definitions, with INFO logging
on both versions. It is not the official Linux harness or a predicted score.
An initial three-pair run overlapped a short unit-test compilation, so a second
three-pair run was collected without compilation or other test workloads.
All samples, including regressions, are retained in the local validation report.

Second run, arithmetic mean of three alternating pairs:

| Build | Control seconds | Candidate seconds | Time reduction |
| --- | ---: | ---: | ---: |
| IK title + content | 12.3467 | 12.6541 | -2.49% |
| IK content | 10.4110 | 10.1155 | 2.84% |
| BENG title + content | 5.8891 | 5.8086 | 1.37% |

These small, mixed timing changes do not establish a robust speedup. Across the
complete local test lifetime, unfinished-child durable lookup log entries fell
from 20,382 in the control to zero in the candidate; successful durable fallback
was separately confirmed by the fault tests. Raw timings, source hashes and
validation summaries are in `fts-build-completion-local-results.json`.

The platform's build-time variation remains unresolved. This change removes
observed repeated work; it does not guarantee that any particular parser's build
will be fastest on the next platform run. Run the existing FTS regressions and
the unchanged complete benchmark (`SKIP_LOAD=0`) on Linux before drawing that
conclusion.
