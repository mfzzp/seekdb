# Verification record

What was checked, how, and what remains unverified. Every number below was
measured on the development server (`/usr/bin/python3` 3.12.3, numpy 2.3.5 on
`PYTHONPATH`, 128-core host) against the committed tree; the commands are listed
at the end so any row can be reproduced.

## 1. The judge's contract, as implemented by `protocol_check`

Read from `protocol_check/_assertions.py` and `protocol_check/config.json` in this
repository, which are the public half of the judge:

| Rule | Value | This implementation |
| --- | --- | --- |
| Float comparison | `math.isclose(abs_tol=1e-5, rel_tol=1e-5)` | distances are exact to ~1e-12, so the tolerance is never the thing that decides |
| Non-float comparison | `type(actual) is type(expected)` then `==` | BIGINT stays a JSON integer end to end, strings and lists are never coerced |
| Vector projection | must be a JSON array | a `VECTOR` column is emitted as `[0.5,1.5,2.5]` |
| Unordered results | multiset comparison | ordered results are a valid multiset and ties still break on the primary key |
| Error responses | `ok:false`, non-empty `error.code`, and the code must equal `config.error_codes[name]` **only when the config defines that name** | codes are `duplicate_key`, `unsupported_sql`, `dimension_mismatch`, `unknown_table`, `invalid_value` — the three names the published `.result` files use are produced verbatim |
| Response size | `max_response_bytes` 16 MiB | a 1000-row x 128-dimension `SELECT *` measures 2.57 MB, about 6x below the cap |
| Time limits | 60 s per request, 600 s per case, 120 s startup | worst measured request is 4.1 s (first query over 300k rows), startup is 30 ms |
| Container | 2 CPUs, 2 GiB, 127 pids, read-only root, no network | one BLAS thread, ~74 MiB resident at 10k rows, writes only under `/data`, no runtime network use |

Thread and CPU behaviour, measured on the 128-core host:

| configuration | threads in the server process | query total, 400 queries |
| --- | ---: | ---: |
| numpy without `OPENBLAS_NUM_THREADS` | 64 | — |
| numpy with the Dockerfile's `OPENBLAS_NUM_THREADS=1` | 1 | 0.664 s / 0.678 s |
| pure Python (no numpy) | 1 | — |
| same, pinned to two CPUs with `taskset -c 0,1` | 1 | 0.653 s / 0.687 s unpinned |

The BLAS thread limit is not cosmetic: without it OpenBLAS sizes its pool from the
host's core count, so a 128-core host gives 64 threads and a host with 128 or more
cores could exceed the 127-pid container limit before serving a single request.
Pinning to two CPUs changes nothing measurable, so the 2-CPU quota is not a
constraint for this workload.

## 2. Performance (measured, paired, medians)

Judge-shaped protocol measurement, re-run against the original baseline at the
final commit: 10k records x 128 dimensions, K=10.

| Phase | Baseline | Candidate (numpy) | Candidate (no numpy) |
| --- | --- | --- | --- |
| load 100 x 100 rows | 0.889 s | 0.897 s | 0.906 s |
| one unfiltered query | 11.79 ms | 1.13 ms | 12.29 ms |
| one equality-filtered query | 1.74 ms | 0.58 ms | 1.74 ms |
| 200 queries | 1.456 s | 0.256 s | 1.616 s |
| 1000 queries | 7.432 s | 0.947 s | 7.198 s |

The numpy path is 7.8x faster end to end and 10x on an unfiltered query; the
pure-Python path is the baseline within noise (ratio 1.00 at 1000 queries), so the
fallback costs nothing and only the dependency adds speed. The selected ids are
identical in both paths (`ids_digest` 38e0d1533d51ec4e). Absolute values move with
host load between rounds - an earlier run of the same comparison read 9.64 s and
1.31 s - so the ratios are the stable part, not the milliseconds.

Scalar predicates resolved before any vector work (10k rows, K=10, median of 7):

| predicate | selectivity | scalar-first | plain scan |
| --- | ---: | ---: | ---: |
| `g < 1` | 0.1% | 0.29 ms | 3.20 ms |
| `g < 10` | 1% | 0.51 ms | 3.29 ms |
| `g < 100` | 10% | 0.28 ms | 4.31 ms |
| `g < 500` | 50% | 0.58 ms | 9.58 ms |
| `g < 1000` | 100% | 0.77 ms | 16.86 ms |

At 100k rows the same queries take 0.6-10 ms against 310-517 ms for the scan, with
ids identical to the scan in every case.

Large table cold start (300k rows x 128 dimensions, measured at the current
revision; both queries return the same 10 rows):

| path | first query | warm query | peak RSS | matrix |
| --- | ---: | ---: | ---: | ---: |
| equality filter | 3.0 s | 3.1 ms | 358 MiB | 146.5 MiB |
| range filter | 1.7 s additional | 16.7 ms | 358 MiB | reused |

Cold start: cumulative time to answer N TopK queries on identical data.

| rows | strategy | N=1 | N=2 | N=3 | N=10 | peak RSS |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 10k | scan only | 67.6 ms | 78.7 | 89.6 | 164.2 | 88 MiB |
| 10k | matrix | **50.3 ms** | **51.2** | **52.0** | **57.1** | 57 MiB |
| 10k | chunk only | 58.5 ms | 114.3 | 169.9 | 555.0 | 54 MiB |
| 100k | scan only | 579 ms | 1114 | 1656 | 5429 | 127 MiB |
| 100k | matrix | 521 ms | **531** | **542** | **604** | 247 MiB |
| 100k | chunk only | **266 ms** | 553 | 862 | 4294 | **71 MiB** |

The matrix wins even for the first query at 10k rows, so it is built immediately;
at 100k rows a single query is twice as fast and 3.5x lighter with chunking, and
the matrix takes over from the second query.

Protocol overhead, one process per variant, 300 samples after warm-up (µs):

| request | decode | parse | execute | encode |
| --- | ---: | ---: | ---: | ---: |
| `SELECT id FROM t LIMIT 10` | 1.9 | 10.3 | 5.4 | 4.8 |
| `SELECT id FROM t ORDER BY id LIMIT 10` | 2.3 | 14.5 | 118.8 | 5.3 |
| TopK via alias | 7.3 | 51.1 | 645.0 | 23.9 |
| TopK with the expression in `ORDER BY` | 8.8 | 62.6 | 703.4 | 24.9 |
| TopK, distance projected twice | 8.5 | 69.0 | 695.6 | 32.4 |

Execution owns 89% of the request, SQL parse 7% and JSON in both directions 4%;
a blocking round trip takes 609 us against 557 us of server stages, so the pipe
and the two context switches cost about 43 us.

Shared budget under a simulated container limit (150k rows x 128 dimensions, a
filtered TopK at 1% selectivity; the limit is injected because
`/sys/fs/cgroup/memory.max` cannot be written here):

| container limit | derived capacity | row cache | dense matrix | warm query | peak RSS | answer exact |
| ---: | ---: | ---: | --- | ---: | ---: | --- |
| 2048 MiB (the judge's) | 1440 MiB | 96 MiB | built, 146.5 MiB | 2.7 ms | 348 MiB | ✔ |
| 1024 MiB | 672 MiB | 96 MiB | built | 2.5 ms | 348 MiB | ✔ |
| 512 MiB | 288 MiB | 96 MiB | built | 2.1 ms | 348 MiB | ✔ |
| 256 MiB | 96 MiB | 38.4 MiB | refused | 32.8 ms | 70 MiB | ✔ |
| 128 MiB | 8 MiB (floor) | 16 MiB | refused | 38.5 ms | 68 MiB | ✔ |

At the judge's 2 GiB the matrix is comfortably inside the budget; at 256 MiB and
below the budget refuses it, the query falls back to the chunked path (about 15x
slower but still exact) and peak resident memory drops to about 70 MiB, well under
the limit.

Bulk insert and response size (measured at the final revision):

| one INSERT statement | rows | statement | round trip | per row |
| --- | ---: | ---: | ---: | ---: |
| 16 dimensions | 1,000 | 89 KB | 0.021 s | 20.9 us |
| 16 dimensions | 10,000 | 899 KB | 0.190 s | 19.0 us |
| 16 dimensions | 50,000 | 4.5 MB | 0.909 s | 18.2 us |
| 128 dimensions | 1,000 | 640 KB | 0.049 s | 49.4 us |
| 128 dimensions | 10,000 | 6.4 MB | 0.388 s | 38.8 us |
| 128 dimensions | 50,000 | 32 MB | 2.073 s | 41.5 us |

Parsing and writing are linear, so even a single 50,000-row statement at 128
dimensions finishes two orders of magnitude inside the 60 s request limit.

Responses are compact JSON. A `SELECT *` projection of 128-dimension vectors grows
by about 2.7 KB per row: 1,000 rows is 2.58 MiB, 5,000 is 12.88 MiB, and 8,000
(20.61 MiB) crosses the judge's 16 MiB cap, which happens at roughly 6,200 rows.
Any implementation that projects vectors as JSON arrays of floats meets that
boundary; the public cases never project a vector column at all.

## 3. Correctness evidence

- The current suite contains 153 regression tests. The dated validation record
  below distinguishes executed tests from dependency-specific skips. The older
  130-test run passed with NumPy; 37 tests were skipped without it.
- 20/20 public cases plus twelve extra cases through the judge's own runner
  (`protocol_check/run.py` with `tests/extra_cases`), in both modes, including
  against a pristine `git archive` export of the committed tree.
- 480/480 differential fuzz queries identical to the original baseline over four
  seeds; 1000-query `ids_digest` identical; 720/720 in an earlier six-seed run.
- Numeric: a real mis-ranking was found and fixed (ULP cluster at magnitude 30,
  128 dimensions, where the identity kernel clamped a whole cluster to zero); the
  regression test for it fails on the previous commit and passes now. The rerank
  window also uses the epsilon of the matrix dtype, so the float32 matrix used
  above `MATRIX_MAX_BYTES` gets a float32-sized window instead of a float64 one
  that would be 5.4e8 times too narrow.
- Crash: six SIGKILL scenarios — between commits, inside a transaction, during a
  100k-row statement, a 10k-row transaction killed before COMMIT, a kill landing
  inside recovery of a 300k-row log (with the test asserting no ready line was
  produced first), and a torn log suffix. Recovery keeps every acknowledged row,
  drops uncommitted ones, leaves no partial statement, and truncates the suffix
  exactly.
- Scale: 100k rows for exact, ann, filtered and fallback paths with ids identical
  to independent scans; memory stays bounded at 128 MiB without numpy.
- Soak: 3,856 mixed operations over 300 rounds, resident memory flat (30 -> 31 MiB
  with numpy, 15 -> 16 MiB without), zero model mismatches.
- Protocol: 27-statement shape/resilience sweep, read-only application directory,
  UTF-8 round trip, and stdout carrying nothing but protocol lines.
- 88-statement golden transcript that fails on any behavioural drift (verified by
  mutating one expectation), and the twelve extra judge cases (verified the same way).
- Two implementations of the same protocol, written separately in this session,
  agree on 125 statements apart from the four divergences that were reviewed and
  decided one by one; the second one hard-requires numpy and dies without it,
  which is the deployment failure this implementation avoids.

### The budget follows a real kernel limit

The reader used to look only at `/sys/fs/cgroup/memory.max`, which is where a plain
Docker container exposes its limit but which does not exist when the process sits in
a nested cgroup - a systemd scope, a Kubernetes pod, docker-in-docker. There the read
returned `None`, the budget became unbounded, and the container limit was ignored.

It now walks `/proc/self/cgroup` upwards and takes the tightest `memory.max` it finds,
falling back to the old paths. Measured inside real systemd scopes on this host:

| real cgroup limit | derived capacity | row cache | matrix cap |
| --- | ---: | ---: | ---: |
| none (host) | unbounded | 96 MiB | 192 MiB |
| 2 GiB | 1440 MiB | 96 MiB | 192 MiB |
| 512 MiB | 288 MiB | 96 MiB | 128 MiB |
| 256 MiB | 96 MiB | 64 MiB | 64 MiB |
| 128 MiB | 8 MiB (floor) | 32 MiB | 32 MiB |

Those match the numbers previously obtained by injecting a limit into the budget, so
the adaptive path is confirmed against the kernel rather than a mock. The whole suite
also passes inside a real 2 GiB / 2 CPU scope (124 tests, with and without numpy) and
the public cases pass at 2 GiB and at 512 MiB.

### Cases do not depend on each other

The harness starts a fresh session - and a fresh data directory - per case, so no
case can see another's rows. That is structural, but it was checked anyway, because
an implementation that kept state outside the data directory would still pass the
suite in its published order:

| run | result |
| --- | --- |
| every case alone, one process each | 20/20 pass |
| all twenty cases in reverse order | 20/20 pass |

### The whole public suite in ann mode

Only one public case selects `ann` (`vector_index_maintenance`); the rest run in the
default `exact` mode, so a regression confined to `ann` could have stayed hidden. The
package was copied with `--mode ann` prepended to all twenty cases, keeping the
published `.result` files unchanged, and run twice:

| run | numpy | pure Python |
| --- | --- | --- |
| 20 public cases, all in `ann` | 20/20 pass | 20/20 pass |
| 12 extra cases, all in `ann` | 12/12 pass | 12/12 pass |

Every published expectation therefore holds under `ann` as well, which is what an
implementation that resolves `ann` exactly should do. Together that is 32
judge-shaped cases run end to end through the judge's own runner in the mode the
public corpus barely exercises.

### Specification checklist

Every normative sentence in `STDIO_PROTOCOL.md` maps to something that was run:

| requirement (section) | evidence |
| --- | --- |
| `{"type":"ready"}` exactly, before the first request, every start (§2) | handshake mode; `test_protocol_surface` |
| one response per request, `id` echoed with its JSON type (§1.5) | `test_blank_lines_do_not_produce_responses`, string-id run |
| newline-terminated, flushed per response (§1.1, §1.3) | every protocol run; the judge's own transport |
| stdout carries protocol only (§1.4) | one write in `send()`, diagnostics to stderr |
| no blank lines or extra lines (§1.6) | blank lines are skipped, `test_malformed_non_blank_line_is_answered` |
| no NaN/Infinity, no duplicate keys (§1.7) | `json.dumps(allow_nan=False)`, insert-time rejection |
| a semicolon can sit inside a string literal (§3) | `test_semicolons_and_newlines_inside_sql_strings` |
| a newline inside a string stays escaped on one line (§1.2) | same test |
| affected rows are integers, empty results keep their columns (§3) | public cases, golden transcript |
| errors keep `ok` boolean and a non-empty code, process continues (§3) | golden transcript, fuzz |
| shutdown answers then stays silent, exit 0 (§4) | `test_protocol_surface`, gate |
| `/data` is reused, never wiped (§4) | restart cases, `test_text_integrity` of the data dir |
| committed data survives a kill without shutdown (§4) | six SIGKILL scenarios |
| the image defaults to stdio with no private arguments (§7) | Dockerfile guards, bare-environment run |

## 4. Verified, inferred, unverified

**Verified by measurement here:** the performance tables above; exact agreement of
every TopK route with `math.dist` or an independent scan; crash recovery under
SIGKILL; bounded memory at 10k and 100k rows; the read-only runtime; a start under
`env -i` with a read-only application directory, which serves queries and writes no
bytecode into it; the judge's `--handshake-only` mode; the pinned numpy wheel exists
for cp312 manylinux and imports and runs a mat-vec; the public 20 cases pass, alone,
in reverse order, and in `ann` mode, as do the twelve extra cases.

**Inferred, not proven:** that the hidden cases use the same SQL surface as the
public ones; that an empty `error_codes` in the public config means codes are not
matched in the hidden config; that the hidden workload resembles the published
performance setup (10k x 128, 1000 queries); that time and memory fit the 2 CPU /
2 GiB container, since the container itself could not be run.

**Unverified, and impossible to verify here:** the Docker image build (no Docker
runtime on either machine — the wheel, the interpreter and the read-only runtime
were verified separately); the 29 hidden cases; platform-level `memory.peak` and
`io.stat`; and a kernel OOM kill (SIGKILL from a parent process was used).

## 5. Reproducing

```bash
# unit tests, both dependency modes
PYTHONPATH=<src>:/path/to/numpy python3 -m unittest discover -s tests
PYTHONPATH=<src> python3 -m unittest discover -s tests

# public cases through the official checker
python3 protocol_check/run.py --command '["python3","<src>/minivectordb.py","--data-dir","{data_dir}"]'

# the measurement scripts referenced above live next to the results directory:
#   protocol_bench.py, bench_competition.py, crosscheck_large.py, soak.py,
#   stage_breakdown.py, roundtrip_breakdown.py, ann_index_check.py,
#   diff_two_impls.py, make_golden.py
```

### Repository state in this working copy

`git fsck` reports one broken link below the published base: commit `f4d4e26` names a
parent (`9f6df2b`) that was never fetched, so a full `git rev-list HEAD` stops about
25 commits below `e62d28e`. The hole predates this work and does not touch the
delivered range: `git rev-list --count e62d28e..HEAD` counts all 73 commits, the bundle
contains exactly that range, and the replay onto `e62d28e` produces a clean tree that
passes the suite. A `git fetch origin` with network access fills the missing ancestor;
nothing in the submission depends on it.

## 6. Delivery dry run

The commits are delivered as a bundle and replayed onto the published base, so the
transfer can be checked without pushing anywhere:

```
git bundle create /tmp/round2-delivery.bundle e62d28e..HEAD
git bundle verify /tmp/round2-delivery.bundle
git clone --no-hardlinks <repo> /tmp/dryrun && cd /tmp/dryrun
git checkout e62d28e && git fetch /tmp/round2-delivery.bundle HEAD
git merge --ff-only FETCH_HEAD
```

Measured: the bundle is 218 KB, `git bundle verify` reports only the required base
commit, the merge is a fast-forward that adds 72 commits, the working tree is clean,
and the replayed tree passes 20/20 public cases and all 117 tests. The procedure is
also in `scripts/dry_run_delivery.sh`.


## Cache reuse candidate, 2026-11-11

Baseline: `8f744ea8eb472d4d732f4f787b9cf05a543a9e32`. The older
performance and delivery tables above describe prior recorded experiments.
The JSON below identifies this candidate by SHA-256 of its source files.

The unchanged new regression set fails on the baseline (deleted-row
visibility and 3,000 packed reads after a one-row update) and passes here.
The candidate copies still-valid matrix entries by log offset, invalidates
scalar indexes on mutations, and falls back when two matrix allocations
would exceed its shared budget. Group building no longer unpacks vectors
to inspect a scalar column. Commit/fsync behavior is unchanged.

| Data / disk | Scalar update then query, old/new ms | Vector update then query, old/new ms |
|---|---:|---:|
| uniform 20000 x 128 / HDD2 | 114.538 / 10.072 | 117.841 / 9.037 |
| uniform 20000 x 128 / SSD1 | 68.752 / 13.257 | 66.189 / 10.073 |
| clustered 30000 x 32 / HDD2 | 83.934 / 4.370 | 151.101 / 4.453 |
| clustered 30000 x 32 / SSD1 | 151.006 / 3.641 | 149.592 / 3.571 |

Each timing is the median of ten post-update queries, excluding the preceding
SQL update. Warm query phases use twenty requests. Every answer digest
matches the baseline. HDD runs execute baseline first; SSD runs execute
candidate first. These are shared-host samples, not official scores or a
guarantee of improvement for every query. Some first/warm-query samples
regress; all samples are retained in the JSON. No system-wide page cache
was flushed, and storage read_bytes was zero for these recently loaded
datasets. The measurements demonstrate decoding/copy costs, not cold-disk
throughput. Source and data are separated; benchmark data is placed on
each named disk. Tests create their own data directories.

Reproduce one sample (new directory required):

`PYTHONPATH=/tmp/npydeps OPENBLAS_NUM_THREADS=1 python3 tests/bench_cache_reuse.py . /path/to/new/data --rows 20000 --width 128 --distribution uniform`

Validation: 130 tests with NumPy; 93 executed and 37 skipped without NumPy;
20 public cases in exact and all-ann modes with both dependency modes;
12 extra protocol cases with both dependency modes. The final tree is also
checked under a real 2 GiB cgroup and taskset CPUs 0,1.
Docker/Podman are absent: an actual container build and --image test remain
unverified. No official evaluation submission is performed by this script.

## 7. Current block and commit candidate (2026-11-12)

The current candidate adds conservative, snapshot-local logical metric blocks and
keeps the append-log format unchanged. Autocommit row frames and their commit
marker are flushed together, while the durability `fsync` remains. The metric
path is gated at 4,194,304 candidate coordinates and falls back to the existing
vectorized or chunked path for smaller or weakly selective workloads.

On the Linux SSD runner, with 2 CPU quota, 2 GiB memory, no swap and pids 128,
the full 153-test suite passed in NumPy mode and passed with 50 dependency skips
without NumPy. The same candidate also passed the independent oracle in every
recorded metric benchmark. Representative warm-query results were 3.131 to
1.801 ms for clustered 32,768 x 128 data, 3.166 to 1.819 ms for correlated
filters, 3.612 to 2.211 ms on the HDD runner, and 13.863 to 5.527 ms for
clustered 131,072 x 128 data. Uniform 32,768 x 128 data stayed on the fallback
path and measured 3.079 to 3.459 ms; the small 10,000-row sample also stayed on
the fallback path. Result rankings and distances matched the independent FP64
oracle in all these comparisons.

The write probe produced identical persisted log hashes and restart results on
both disks. For 1,000 single-row statements, raw `write` calls fell from 2,002
to 1,001 while `fsync` remained 1,001; for batch sizes 100 and 1,000 the counts
were 22 to 11 and 8 to 6 respectively, with the same fsync counts. These counts
reduce syscall overhead but do not remove serial fsync latency.

These are controlled synthetic measurements on a shared server. They do not
flush the system page cache, so a first query is process-cold derived state rather
than a cold physical-disk measurement. The block structure is an in-memory
derived index; physical row clustering, LSM compaction, HNSW and GPU execution
are outside this candidate. The exact source hashes, commands, artifacts and
known limitations are recorded in `agent/block-optimization-20261112.json`.

The same final working tree was built and exercised in the installed rootless
Docker runtime. The acceptance record
`/mnt/SSD1/zzp/docker-block-acceptance-20261112T014700Z-b651a3d089/acceptance.json`
reports a passing image build, NumPy probe, cgroup limits, read-only application
files, handshake, 20 public exact cases, 12 extra cases and 20 public ANN cases.
