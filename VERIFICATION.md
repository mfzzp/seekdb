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
| Response size | `max_response_bytes` 16 MiB | largest realistic answer (1000 rows x 128 floats) is ~0.6 MiB |
| Time limits | 60 s per request, 600 s per case, 120 s startup | worst measured request is 4.1 s (first query over 300k rows), startup is 30 ms |
| Container | 2 CPUs, 2 GiB, 127 pids, read-only root, no network | one BLAS thread, ~74 MiB resident at 10k rows, writes only under `/data`, no runtime network use |

## 2. Performance (measured, paired, medians)

Judge-shaped protocol measurement, 10k records x 128 dimensions, K=10:

| Phase | Baseline | Candidate (numpy) | Candidate (no numpy) |
| --- | --- | --- | --- |
| load 100 x 100 rows | 0.907 s | 1.026 s | 0.963 s |
| one unfiltered query | 17.72 ms | 1.60 ms | 17.72 ms |
| one equality-filtered query | 2.21 ms | 1.16 ms | 1.73 ms |
| 200 queries | 1.96 s | 0.44 s | 2.05 s |
| 1000 queries | 9.64 s | 1.31 s | 9.00 s |

A same-code control pair gives a 4% noise floor on this host, and the 13% write
difference disappears without numpy (ratio 0.99), so it is the numpy runtime being
resident rather than a write-path regression.

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

Cold start (cumulative time, 10k rows): the dense matrix wins even for the first
query (50 ms against 68 ms for the scan), so it is built immediately. At 100k rows
a single query is 2x faster and 3.5x lighter with chunking (266 ms / 71 MiB against
521 ms / 247 MiB), and the matrix takes over from the second query.

Protocol overhead decomposition (per request, 10k rows): execution 645 us (89%),
SQL parse 51 us (7%), JSON in and out 31 us (4%), pipe plus two context switches
about 43 us.

## 3. Correctness evidence

- 69 regression tests, passing with and without numpy (26 skipped without it).
- 480/480 differential fuzz queries identical to the original baseline over four
  seeds; 1000-query `ids_digest` identical; 720/720 in an earlier six-seed run.
- 20/20 public cases in both modes, including against a pristine `git archive`
  export of the committed tree.
- Numeric: a real mis-ranking was found and fixed (ULP cluster at magnitude 30,
  128 dimensions, where the identity kernel clamped a whole cluster to zero); the
  regression test for it fails on the previous commit and passes now.
- Crash: a real SIGKILL between commits, inside a transaction, during a 100k-row
  statement, and a torn log suffix; recovery keeps every acknowledged row, drops
  uncommitted ones, leaves no partial statement, and truncates the suffix exactly.
- Scale: 100k rows for exact, ann, filtered and fallback paths with ids identical
  to independent scans; memory stays bounded at 128 MiB without numpy.
- Soak: 3,856 mixed operations over 300 rounds, resident memory flat (30 -> 31 MiB
  with numpy, 15 -> 16 MiB without), zero model mismatches.
- Protocol: 27-statement shape/resilience sweep, read-only application directory,
  UTF-8 round trip, and stdout carrying nothing but protocol lines.
- 88-statement golden transcript that fails on any behavioural drift (verified by
  mutating one expectation).

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
