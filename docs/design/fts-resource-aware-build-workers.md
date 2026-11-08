# Resource-aware local DDL build workers

## Problem and change

The local DDL builder used one thread for every mini-mode instance (a memory
budget below 9.6 GiB). FTS parents can submit independent inverted-index and
doc-word builds together, but the single worker executes their scans, parsing,
sorting and writes sequentially. Recorded local BENG builds spent about three
seconds on the inverted index and another two-and-a-half seconds on doc-word;
completion cleanup took only tens of milliseconds.

`ObDDLLocalBuilder::get_thread_cnt_()` now selects two workers in mini mode when
the logical memory budget is at least 4 GiB and the effective CPU count is at
least two. Smaller-memory and single-CPU instances retain one worker; non-mini
instances retain the existing sixteen-worker pool. The four-GiB activation
threshold is a conservative resource policy, not a reservation of that amount
per task. Effective CPU count uses the existing configured/auto-detected CPU
helper. Selection occurs at builder initialization, not continuous resizing.

This is the shared local DDL build queue, so ordinary index builds can also
overlap. Existing task dependencies, snapshots, checksum validation, error
propagation and index publication are unchanged. The asynchronous queue already
supports concurrent processing in non-mini mode. The selected worker count is
included in its existing initialization log.

No table names, parser names, document contents or benchmark parameters enter
the selection rule. No benchmark or scoring files were modified.

## Local verification

Control: `7b78ea426e634e3c063fe77b52d669ac0b975f2b`.
Windows executables used the same existing development build, rebuilding the
share translation unit and library with LLVM 21. All other linked objects were
identical. Both instances bootstrapped and passed SELECT 1. This is incremental
Windows validation, not a clean Linux build or platform evaluation.

- `test_ddl_build_concurrency.py` passed on control and candidate: four concurrent
  FTS builds (space, BENG, IK and ngram), an ordinary index build, and concurrent
  committed/rolled-back changes; final matching primary keys equal a base-table
  LIKE oracle. A duplicate-key index build returns 1062 while FTS proceeds, and
  a subsequent ordinary index build succeeds.
- Concurrent DML/schema work produced a pre-existing 1205 lock conflict on the
  control. The test applies bounded retries only to this transient DDL error,
  then requires successful completion and all final data assertions. Other
  errors, including the deliberately tested 1062, are not suppressed.
- Existing `fts_build_completion` (12 assertions) and `fts_score_free_filter`
  (24 assertions) passed on both versions.
- With the original 20,000-document data and query SQL, counts remain
  8001/11000/7332/20. Full matching primary keys, numeric relevance scores and
  LIMIT results match the control.
- Separate fresh instances with 2 GiB/four CPUs and 4 GiB/one CPU selected one
  worker and completed a populated BENG index build correctly. The 4 GiB/four
  CPU candidate selected two workers.

To run the concurrent integration test on a dedicated instance with PyMySQL
installed: `SEEKDB_TEST_PORT=2881 python3 unittest/rootserver/test_ddl_build_concurrency.py`.
Optional connection settings are `SEEKDB_TEST_HOST`, `SEEKDB_TEST_USER` and
`SEEKDB_TEST_PASSWORD`. Tests create and remove uniquely named databases.

## Build timing

Both instances used `memory_budget=4G`, `cpu_count=4`, INFO logging and identical
datafile/log settings. Three alternating pairs rebuilt the original three index
definitions on the original 20,000 documents. The CPU configuration differs
from earlier local experiments, so their absolute timings are not comparable.

| Build | Control mean seconds | Candidate mean seconds | Reduction |
| --- | ---: | ---: | ---: |
| IK title + content | 24.1656 | 13.9176 | 42.41% |
| IK content | 19.3947 | 10.3001 | 46.89% |
| BENG title + content | 11.3102 | 6.0851 | 46.20% |

Each build category improved in all three pairs. These are persistent-PyMySQL
timings on Windows, not the official Linux harness. They do not predict a
platform score. In particular, a platform instance below the activation memory
threshold, or with a single effective CPU, will retain the old worker count.

The previous platform 2.7-second/6-second BENG variation has not been reproduced
or attributed to a specific cause. This change addresses the observed serial
build bottleneck; it does not claim to explain that variation.

The SQL-stage timestamps confirm the mechanism. Across the three BENG pairs,
control inverted-index and doc-word execution had zero overlap. Candidate
execution overlapped by 5.205, 4.738 and 4.313 seconds, respectively, on distinct
DDL workers. These intervals use the explicit `ddl_event_info.event_ts_` fields;
the local Windows build sometimes prints malformed fractional log prefixes.

A further fresh-instance comparison, with both processes restricted to the
same four logical CPUs, measured 18.416/14.650/8.483 seconds for control and
11.834/8.601/4.950 seconds for candidate. All three builds improved again.

## Query timing limitations

Tokenization and queries were also measured, including unfavorable results.
The following are candidate elapsed-time reductions relative to control;
negative values mean a slowdown. Each column contains three alternating samples
with 30 warmups, 3,000 calls per tokenizer and 200 calls per query. These are
different instance/affinity experiments and must not be pooled as one score.

| Metric | Original instances | Same instances, CPUs 0-3 | Fresh instances, CPUs 0-3 |
| --- | ---: | ---: | ---: |
| IK TOKENIZE | -2.12% | -29.77% | +0.34% |
| BENG TOKENIZE | -29.04% | -38.21% | -3.94% |
| Chinese query | -19.62% | -20.45% | +4.48% |
| English query | -4.10% | -24.01% | +15.97% |
| Mixed query | +1.23% | -7.25% | +11.26% |
| LIMIT query | +3.05% | -8.63% | +9.54% |

The initial regressions did not consistently reproduce on fresh instances.
There were no outstanding DDL records or client queries during the idle check;
control/candidate used 0.297/0.219 CPU seconds over five idle seconds. This does
not establish the cause of the query differences or rule out a regression.
Build gains are repeatable locally; overall query gains are not established.

A diagnostic attempt to exchange binaries over the old Windows data directories
failed for both binaries during log-block initialization (`OB_NOT_INIT`, -4006),
before local DDL builder initialization. No crossed query results were obtained.
Fresh instances subsequently bootstrapped and completed the comparison above.
Crash recovery on this local development build is therefore not validated.

All raw timing samples, build intervals, source fingerprint and correctness
results are in [the local results](fts-resource-aware-build-local-results.json).
The candidate still needs a clean Linux build and the official full benchmark.
For a dedicated Linux instance, use the repository's unchanged benchmark
instructions with `SKIP_LOAD=0`, 20,000 rows, three samples and a new output file.
Retain all nine timing metrics when comparing against the previous submission;
the local measurements above are not a prediction of the platform score.
