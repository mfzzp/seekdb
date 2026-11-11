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

## 3. Correctness evidence

- 117 regression tests, passing with and without numpy (31 skipped without it).
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

## 4. Verified, inferred, unverified

**Verified by measurement here:** the performance tables above; exact agreement of
every TopK route with `math.dist` or an independent scan; crash recovery under
SIGKILL; bounded memory at 10k and 100k rows; the read-only runtime; the pinned
numpy wheel exists for cp312 manylinux and imports and runs a mat-vec; the public
20 cases pass.

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
