# Lite2000 verification ledger

The latest local exact iteration is [EXACT_PATHS_20261114.md](docs/EXACT_PATHS_20261114.md): selectable direct scan/Block executors, default scan, stage profiling, rollback cache invalidation, and durable Manifest publication before old-file reclamation. Its Windows/MSYS measurements must not be mixed with the server measurements below. The root Dockerfile now compiles Lite. The source is 2230 physical lines; the pre-existing 2000-line checker failure remains. Historical checks and counts below describe their original snapshots.

The latest same-input, same-container C++ comparison is [COMPARE_100K_20261114.md](docs/COMPARE_100K_20261114.md), with raw records under `results/paired_100k_20261114/`. It supersedes earlier old/new performance conclusions: 128-dimensional mean exact improved 1.96x, but filtered exact regressed; 1024-dimensional mean exact was 1.70x slower and unfiltered ANN recall was 3.60%. Both exact datasets and all full-row restart checks passed. No official composite score is claimed.

## Current implementation

- Runtime: `competition_lite/src/minivectordb_lite.cpp`, **2185 physical lines** (comments and blank lines included).
- Build: C++17, `-O3 -DNDEBUG -Wall -Wextra`.
- Storage components: framed CRC WAL, separate float32 vector file with mmap reads, bounded continuous-vector matrix caching for small tables, persisted scalar postings and full-coordinate Block statistics, MemTable/Immutable/SST maintenance, checkpointed recovery state, exact Top-K, IVF with global non-residual PQ and raw-vector reranking, index persistence, and vector GC. BEGIN currently copies table state, so the planned incremental private write set is incomplete; SST is not yet the primary query/recovery source. Crash-safe publication and reclamation also remain incomplete.
- There is **no query-result cache**. The plan explicitly excludes one, so every SELECT is computed from current state.
- WAL frames use 8-byte alignment, complete-frame CRC rejection and incomplete-tail truncation. STATE v3 now records the checkpoint vector boundary, and WAL token counts are bounded by encoded payload length.
- Manifest and persisted IVF-PQ files are published through temporary files; IVF-PQ files carry a trailing CRC, and a bad index is reported on stderr before rebuilding. `LITE_NPROBE` is exposed for validation and response metrics report probed lists and raw candidates.
- The runtime remains single-writer. The original 2000-line target is exceeded by explicit index/statistics and cache paths; no hidden or generated runtime code is used to reduce the count.

## Checks completed

| Check | Result |
|---|---:|
| Official local public protocol cases | 20/20 PASS after latest optimization |
| Strict packaged protocol check | Historical 20/20 PASS before current write changes |
| LSM flush/compaction/restart smoke | PASS, 79 live rows and L0/L1 files |
| IVF-PQ index persistence/restart | PASS |
| Crash recovery smoke with nonzero vector validation | 20/20 PASS after current write changes |
| Eight-column B256/B1024 WAL recovery with/without checkpoint | 4/4 PASS including vector distances |
| JSON/SQL quoted-string regression | 2000 cases PASS |
| Runtime physical-line count | 2175 (current source; comments and blanks included) |

## Current write-path evidence

See [the hardware and write report](docs/WRITE_LIMITS_20261114.md) and [raw measurements](results/write_study_20261114.json). Identical prebuilt JSONL requests, the same SSD, three runs per configuration: 10k/128/B100 INSERT time changed from 0.797941 s to 0.604412 s; 4096/1024/B256 changed from 0.872792 s to 0.264739 s. These compare the previous lite binary with the current lite binary, not the old project. Input preparation and checkpoint times are reported separately. The measured same-shape two-file durable I/O references are 0.404893 s and 0.080258 s respectively.

Increasing the batch to 1024 yields 0.141261 s for 10k/128, but changes commit count and is reported as a separate workload. No WAL or vector sync was removed. The runtime is 2175 physical lines. STATE v2/JSON with a nonempty WAL is explicitly rejected because the old format lacks a reliable checkpoint vector boundary; clean legacy checkpoints can be upgraded.

## Current exact-first optimization evidence

The hot path first chooses the smallest persisted scalar posting/range candidate set, returns empty candidates immediately, and uses a bounded continuous float32 matrix only when the table fits `LITE_MATRIX_MB` (default 64 MiB). Larger tables use physically ordered mmap reads. Exact Top-K stores distance directly in the heap; Block traversal uses full-coordinate box and center/radius lower bounds, with physical order as the default to preserve sequential reads. Bound-sorted Block order is available with `LITE_BLOCK_ORDER=bound` for ablation.

One 1,024-case exact sweep on the fixed 10k/128-dimension workload measured mean 0.467 ms/request, p50 0.260 ms, p95 1.275 ms; the accompanying independent check reported exact match 1.0. On the fixed 100k/1024-dimension bank, the earlier 1,024-case exact sweep measured 36.62 ms/request across all filters; the no-filter subset was 109.8 ms, category equality 22.7 ms, category+tenant 10.4 ms, and the id<100 path 0.497 ms. These are engine-path timings, not claims that the old and new implementations use identical storage layouts.

The optional `LITE_EARLY=1` partial-distance path was tested on the 100k/1024 and 10k/128 banks. It pruned candidates safely but was slower than the full SIMD path on these distributions, so it is disabled by default and remains an ablation.

Historical public-shape measurements recorded old 9.398 ms median unfiltered, 0.774 ms filtered, 5.307 s query total and 0.958 s writes; lite recorded 0.404 ms, 0.219 ms, 0.329 s and 1.304 s. A later harness audit found that the old script called Python Database directly while lite used JSONL, query generation differed, and write timing included Python SQL formatting. The previously claimed apples-to-apples 23.3x/3.5x speedups and the causal explanation for 36% slower writes are withdrawn. Preserve these only as historical observations, not a controlled old/new C++ comparison. The current write report uses byte-identical JSONL and explicitly separated timing.

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

The current 100k post-index mutation smoke also passed after moving full index publication to checkpoint: 1,000 scalar updates took 54.35 s, 1,000 vector updates 59.03 s, and 1,000 deletes 56.12 s, followed by rollback and restart checks. These numbers include one durable WAL commit per request; they are not comparable to the batched B256 write table.

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

The fixed-path smoke predates the lean auto-commit optimization; the same-data write table above is the current write comparison. LOWMEM used the converted binary STATE and windowed mmap path.

## Maintenance evidence

The 300k vector GC command completed in 5.498 s, kept the 1,228,800,000-byte live vector file, and a subsequent `--check` restart completed in 2.1377 s. The compact binary STATE conversion measured about 1.1 GiB peak in the normal-memory conversion, while subsequent 1 GiB load/check used about 260 MiB. This proves the command path and restart check, but not yet the full thresholded segment-reclamation protocol with row-id/ref-epoch evidence.

## T00-T11 gap audit

- **T00:** clean branch, isolated worktree, contract file, ancestor check, and local/server paths are recorded. Official unknowns still need binding from the formal package rather than assumptions.
- **T01:** JSONL line protocol, CLI, checks, and maintenance commands work. Negative SQL coverage is not exhaustive.
- **T02:** WAL now has aligned frames, CRC rejection, and safe incomplete-tail truncation; it still reconstructs canonical mutation tokens rather than fully typed column-level PUT/PATCH/DELETE records, and vector spool thresholds are not separately implemented.
- **T03:** vectors are separate and mmap-readable; MemTable/Immutable/SST maintenance exists with byte/WAL thresholds configurable for small tests. Checkpoint STATE is now a compact binary snapshot with a legacy JSON fallback, greatly reducing recovery memory. SST files now emit 32 KiB sparse block sidecars with per-block CRC and first-record markers using a bounded streaming buffer; key/seq-aware merge semantics and overlap selection are implemented. Stable VectorId/ref_epoch metadata is still missing.
- **T04:** MemTable records now carry key/sequence/tombstone/row metadata; SST flush sorts them, Compaction performs key-aware latest-record merge, and higher-level selection keeps only overlapping ranges. L0→L1→L2 and orphan-file retention passed a small smoke. Manifest range entries and bytes-in/out counters are recorded; bottom-level tombstone removal is now conditional on there being no overlapping L0 range, while the full checkpoint/ref_epoch proof remains open.
- **T05:** exact search is independent of ANN and uses scalar postings plus safe block bounds. The independent Oracle is now mmap/blockwise and small exact match remains 1.0; a fast NumPy/compiled truth run for the complete 100k/300k bank is still missing.
- **T06:** IVF, global non-residual PQ, ADC, raw rerank, persistence, index CRC, and nprobe observability exist. Defaults remain conservative for build time; `LITE_NLIST`, `LITE_PQ_M`, `LITE_PQ_KS`, `LITE_KMEANS_ITERS`, and `LITE_NPROBE` permit controlled sweeps. The measured small ANN recall is 0.90625 (<0.95 target).
- **T07:** post-index insert, scalar update, vector update, delete, rollback, and restart checks now have an executable `bench/mutate_check.py` smoke. Full index-file rewrites were removed from each auto-commit mutation; checkpoint publishes the durable index generation. VectorId/ref_epoch generations, thresholded rebuild policy, and 128-query post-update/compaction/GC Oracle checks remain incomplete.
- **T08:** public/strict protocol, smoke recovery, and 20 tiny crash rounds pass. The 13-point C01-C13 matrix, ASan/UBSan, short-write/ENOSPC/interior-CRC cases, and 20 large-state random kill rounds are missing.
- **T09:** 100k/300k B256 and the 1024-case engine sweep are recorded. S1/B1024/T256 at scale, K=1/K=100, updates/deletes, random-vector runs, and independent large truth are missing.
- **T10:** compact binary recovery and windowed mmap now pass a 300k LOWMEM load/check plus 256-query (512 request) subrun at 1 GiB, with about 526 MiB peak RSS; full 300k build under the cap, true storage-cold, and large 20-round kill/restart matrix remain missing.
- **T11:** commits, line checks, ancestor checks, and reproducible local/server paths exist. Clean platform submission, ablations, and a successful remote push remain incomplete.






