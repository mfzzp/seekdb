# Lite2000 verification ledger

## Current implementation

- Runtime: `competition_lite/src/minivectordb_lite.cpp`, **1941 physical lines** (comments and blank lines included).
- Build: C++17, `-O3 -DNDEBUG -Wall -Wextra`.
- Storage: private transaction writes, framed CRC WAL, separate float32 vector file with mmap reads, MemTable/Immutable/SST maintenance, checkpointed recovery state, scalar postings, exact Top-K, IVF with global non-residual PQ and raw-vector reranking, index persistence, and vector GC.
- There is **no query-result cache**. The plan explicitly excludes one, so every SELECT is computed from current state.\n- WAL frames now use 8-byte alignment, complete-frame CRC rejection, safe incomplete-tail truncation, and the vector cursor keeps only the last complete commit.\n- Manifest and persisted IVF-PQ files are published through temporary files; IVF-PQ files carry a trailing CRC, and a bad index is reported on stderr before rebuilding. `LITE_NPROBE` is exposed for validation and response metrics report probed lists and raw candidates.
- The runtime remains single-writer and stays below the 2000 physical-line budget.

## Checks completed

| Check | Result |
|---|---:|
| Official local public protocol cases | 20/20 PASS after latest optimization |
| Strict packaged protocol check | 20/20 PASS after latest source and query-loop changes |
| LSM flush/compaction/restart smoke | PASS, 79 live rows and L0/L1 files |
| IVF-PQ index persistence/restart | PASS |
| Crash recovery smoke | 20/20 PASS |
| Runtime physical-line budget | 1975 / 2000 (current source) |

The tools under `competition_lite/bench` generate the fixed eight-column schema, write `vectors.f32`, compute a blockwise mmap-backed independent Oracle, exercise S1/B256/B1024/T256, support mixed and pure-random vectors, and emit per-query JSON plus summaries.

## Small benchmark evidence

For 200 rows and 16 dimensions, 8 query cases, one run per mode:

| mode | insert seconds | exact mean/query | ANN mean/query | requests |
|---|---:|---:|---:|---:|
| S1 | 0.8467 | 0.000203 | 0.000168 | 218 |
| B256 | 0.0110 | 0.000223 | 0.000188 | 19 |
| B1024 | 0.0121 | 0.000187 | 0.000160 | 19 |
| T256 | 0.8596 | 0.000226 | 0.000163 | 618 |

The independent Oracle check on 32 cases returned exact match 1.0 and ANN recall 0.90625. The latter is below the plan's internal 0.95 target and remains open.

## Same-data old/new comparison without result caching

The comparison uses the fixed 1024-dimensional generator, B256 writes, and the complete 1024-case filter sweep (2048 requests: exact and ANN for each case). The old binary is the existing baseline index path; this is a timing comparison, not a claim that both indexes use identical algorithms.

| rows | old exact | lite exact | old ANN | lite ANN |
|---:|---:|---:|---:|---:|
| 100,000 | 11.534 ms | 13.251 ms | 10.818 ms | 9.653 ms |
| 300,000 | 36.211 ms | 50.721 ms | 35.520 ms | 32.380 ms |

The pointer-based exact loop improved the 100k lite result from 16.206 ms to 13.251 ms. It is still about 15% slower than old exact at 100k; 300k remains slower because scalar candidate construction and row materialization dominate. ANN is about 1.12x faster at 100k and 1.10x faster at 300k in this sweep.

## Same-data B256 write comparison

| rows | old baseline | lite2000 | speedup |
|---:|---:|---:|---:|
| 100,000 | 59.703 s | 52.885 s | 1.13x |
| 300,000 | 178.769 s | 159.668 s | 1.12x |

The lite numbers use the lean auto-commit path, which avoids copying the full table snapshot for each large INSERT while retaining rollback of newly inserted rows. Duplicate-key rollback was rerun after this change and passed.

## Fixed-dimension large-path smoke

These runs use the fixed 1,024-dimensional generator and B256 requests. They measure the engine path; no independent large-scale Oracle claim is attached.

| rows | vector file | insert | index build | 64-query batch | exact mean/request | ANN mean/request | restart check |
|---:|---:|---:|---:|---:|---:|---:|:---:|
| 100,000 | 409,600,000 B | 64.039 s | 2.758 s | 1.120 s | 21.933 ms | 11.829 ms | PASS |
| 300,000 | 1,228,800,000 B | 268.291 s | 7.402 s | 2.921 s | 55.736 ms | 35.003 ms | PASS |

The fixed-path smoke predates the lean auto-commit optimization; the same-data write table above is the current write comparison.

## Maintenance evidence

The 300k vector GC command completed in 5.498 s, kept the 1,228,800,000-byte live vector file, and a subsequent `--check` restart completed in 2.1377 s. This proves the command path and restart check, but not yet the full thresholded segment-reclamation protocol with row-id/ref-epoch evidence.

## T00-T11 gap audit

- **T00:** clean branch, isolated worktree, contract file, ancestor check, and local/server paths are recorded. Official unknowns still need binding from the formal package rather than assumptions.
- **T01:** JSONL line protocol, CLI, checks, and maintenance commands work. Negative SQL coverage is not exhaustive.
- **T02:** WAL now has aligned frames, CRC rejection, and safe incomplete-tail truncation; it still reconstructs canonical mutation tokens rather than fully typed column-level PUT/PATCH/DELETE records, and vector spool thresholds are not separately implemented.
- **T03:** vectors are separate and mmap-readable; MemTable/Immutable/SST maintenance exists with byte/WAL thresholds configurable for small tests. SST files are not yet metadata-only sparse-block files with checksummed block directories, and checkpointed STATE remains the recovery source.
- **T04:** count-based L0/L1/L2 compaction exists. Range-overlap selection, sequence/key merge semantics, safe tombstone dropping, and byte/range metrics are incomplete.
- **T05:** exact search is independent of ANN and uses scalar postings plus safe block bounds. Small independent Oracle exact match is 1.0. A NumPy/blockwise Oracle for 100k/300k and the full plan-sized truth set is missing.
- **T06:** IVF, global non-residual PQ, ADC, raw rerank, persistence, index CRC, and nprobe observability exist. Defaults remain conservative for build time; `LITE_NLIST`, `LITE_PQ_M`, `LITE_PQ_KS`, `LITE_KMEANS_ITERS`, and `LITE_NPROBE` permit controlled sweeps. The measured small ANN recall is 0.90625 (<0.95 target).
- **T07:** insert/update/delete and index persistence/rebuild work. VectorId/ref_epoch generations, thresholded rebuild policy, and 128-query post-update/restart/compaction/GC oracle checks are incomplete.
- **T08:** public/strict protocol, smoke recovery, and 20 tiny crash rounds pass. The 13-point C01-C13 matrix, ASan/UBSan, short-write/ENOSPC/interior-CRC cases, and 20 large-state random kill rounds are missing.
- **T09:** 100k/300k B256 and the 1024-case engine sweep are recorded. S1/B1024/T256 at scale, K=1/K=100, updates/deletes, random-vector runs, and independent large truth are missing.
- **T10:** a GC/restart smoke exists. The 1 GiB LOWMEM run, true cold-cache run, and large 20-round kill/restart matrix are missing.
- **T11:** commits, line checks, ancestor checks, and reproducible local/server paths exist. Clean platform submission, ablations, and a successful remote push remain incomplete.


