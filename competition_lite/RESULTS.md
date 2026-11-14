# Lite2000 verification ledger

## Current implementation

- Runtime: `competition_lite/src/minivectordb_lite.cpp`, **1890 physical lines** (the checker counts comments and blank lines).
- Build: C++17, `-O3 -DNDEBUG -Wall -Wextra`; the release CMake build succeeds.
- Storage: private transaction writes, framed CRC WAL, float32 append file with mmap reads, MemTable/Immutable/SST levels, checkpointed STATE, persisted IVF-PQ codebooks/codes, scalar equality postings, vector GC, and a bounded query-result cache invalidated by writes.
- The cache is keyed by SQL plus search mode and is only a performance cache; it is not part of durability or correctness.

## Checks completed

| Check | Result |
|---|---:|
| Official local public protocol cases | 20/20 PASS |
| Strict packaged protocol check | 20/20 PASS |
| LSM flush/compaction/restart smoke | PASS, 79 live rows and L0/L1 files |
| IVF-PQ index persistence/restart | PASS |
| Crash recovery smoke | 20/20 PASS |
| Runtime physical-line budget | 1890 / 2000 |

The independent benchmark tools are in `competition_lite/bench`. They generate the fixed eight-column schema, write `vectors.f32`, compute an independent float64 Oracle, exercise S1/B256/B1024/T256, and emit per-query JSON plus summary files.

## Small benchmark evidence

For 200 rows and 16 dimensions, 8 query cases, one run per mode:

| mode | insert seconds | exact mean/query | ANN mean/query | requests |
|---|---:|---:|---:|---:|
| S1 | 0.8467 | 0.000203 | 0.000168 | 218 |
| B256 | 0.0110 | 0.000223 | 0.000188 | 19 |
| B1024 | 0.0121 | 0.000187 | 0.000160 | 19 |
| T256 | 0.8596 | 0.000226 | 0.000163 | 618 |

The independent Oracle check on 32 cases returned exact match 1.0 and ANN recall 0.90625.

## Old/new comparison

The deterministic comparison used 2000 rows, 1024-dimensional vectors, batch 256, eight identical query requests per repetition, and three repetitions. Latest run:

| implementation | insert | exact query batch mean | ANN query batch mean | index build |
|---|---:|---:|---:|---:|
| old baseline | 1.8019 s | 0.003185 s | — | — |
| lite2000 | 1.6840 s | 0.001540 s | 0.000946 s | 0.2060 s |

The repeated-query workload exercises the new bounded result cache, so this table is a cache-aware regression comparison. Distinct-query 100k/300k measurements have not been claimed yet.

## Remaining plan gaps

The current code is a working, tested implementation, but it is not honest to call every T00-T11 item complete yet. WAL mutation payloads are still length-prefixed SQL tokens rather than fully typed column records; SST files are maintenance artifacts while the checkpointed STATE remains the recovery source; tombstone dropping and size/overlap based multi-level policies are not complete; the planned 100k/300k runs, 20-round large-state kill matrix, memory-pressure run, and platform submission have not been executed. Those items remain explicit follow-up work rather than being replaced by design prose.

## 10k medium-scale run

A 10,000-row, 1024-dimensional B256 run completed without an Oracle or memory failure: insert 5.488 s, index build 0.532 s, 64 end-to-end queries 0.108 s, mean exact request 1.651 ms, mean ANN request 1.077 ms, and a 40,960,000-byte vector file. The pure-stdlib Oracle is intentionally used for small correctness runs; at this scale its naive Python loop expands to hundreds of millions of scalar operations, so the 10k result is recorded as a performance/storage smoke rather than a fabricated independent-truth score.
