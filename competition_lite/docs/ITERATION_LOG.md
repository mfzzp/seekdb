# Iteration log

## 2026-11-14 small optimization pass

- Baseline commit: bbb8412
- Kept: generic scalar equality postings, rollback rebuild, exact candidate narrowing, nth_element for ascending distance LIMIT K, and removal of -ffast-math/-march=native.
- Correctness: public harness 20 cases, 0 failures; scalar/update/rollback smoke passed; exact tie smoke passed.
- Small benchmark: 2000 rows, 1024 dimensions, batch 256, 16 queries, one run. Old query 0.01965 s; new query 0.01844 s. Old insert 0.23483 s; new insert 0.26579 s in that run.
- Physical-offset ordering was measured three times and reverted: with ordering mean 0.01844 s, without ordering mean 0.01786 s.
- Four-way accumulation was reverted because the small query result was unchanged.
- Large 100k/300k experiments remain deferred.

## 2026-11-14 binary WAL framing pass

- Added a fixed 24-byte little-endian WAL header: magic, payload length, transaction id, record kind/flags, and CRC32C.
- Commit order remains vector flush, WAL fdatasync, then state advancement; recovery rejects bad magic, oversized payloads, CRC failures, and incomplete tails.
- Small checks: fresh WAL header begins with `32 4c 56 4d`; restart after SIGKILL recovered the committed insert; appended garbage and a three-byte truncated tail were ignored without a crash.
- Public harness: 20 cases, 0 failures.
- This is an intermediate durability step. Mutation payloads are still canonical operation strings, so the next durability pass must replace recovery-time SQL application with structured mutation opcodes before claiming full T02 compliance.
- Large 100k/300k experiments remain deferred until the storage and ANN work has no obvious small-scale regression.

## 2026-11-14 structured WAL mutation pass

- WAL mutation frames now carry an opcode plus bounded length-prefixed SQL token fields. Recovery decodes this binary record and dispatches directly to mutation handlers; it does not call the SQL tokenizer.
- Verified frame payloads: CREATE uses opcode `01`, INSERT uses opcode `03`; the 24-byte framing and CRC remain unchanged.
- Public harness: 20 cases, 0 failures. WAL smoke, SIGKILL recovery, garbage-tail recovery, and truncated-tail recovery all passed.
- This removes the main recovery parser dependency, but the payload is still token-oriented rather than fully typed column values. The next storage pass will address true multi-level SST/manifest behavior and tombstone compaction.

## 2026-11-14 L0/L1 SST pass

- Replaced the single SST list with a level-aware manifest. Flushes write sorted L0 records; four L0 files trigger a multiway merge with existing L1 into a new L1 file.
- Delete records are marked with a `T ` tombstone prefix and are retained through the merge. This is the first real level transition; key-aware tombstone dropping and range metadata are still pending.
- 72-row smoke created L0/L1 files and a retained tombstone; after restart `id=2` stayed deleted and scalar filtering returned the expected rows.
- Public harness: 20 cases, 0 failures.
- Large experiments remain deferred.

## 2026-11-14 IVF-PQ and ADC pass

- Replaced the first-dimension bucket shortcut with trained IVF coarse centroids, global non-residual product-quantizer codebooks, per-row PQ codes, ADC candidate ranking, and raw-vector reranking.
- Insert/update/delete and restart rebuild the IVF/PQ state. Exact mode remains a full independent scan.
- The first probe/candidate setting produced 0/10 recall on a periodic 4,000-row smoke, so it was rejected. The final small setting probes all coarse lists and retains a bounded ADC candidate set before raw rerank; the same smoke reached 10/10 recall.
- 2,000 rows x 1024 dimensions, 8 queries x 3 runs: exact mean 0.01641 s, ANN mean 0.01548 s (about 5.7% faster in this run). 4,000 rows x 32 dimensions showed ANN slower, so this optimization is intended for the fixed 1024-dimensional experiment.
- Public harness: 20 cases, 0 failures. Vector index update/restart smoke passed.
- Large experiments remain deferred.

## 2026-11-14 integrated durability/checkpoint pass

- Added a real shutdown checkpoint boundary: rollback any open transaction, flush the current LSM memtable/SST state, write an atomic `STATE.tmp` snapshot carrying the committed WAL high-water mark, fsync it, rename it, then truncate and fdatasync the WAL.
- Recovery loads the snapshot first and replays only WAL transactions newer than that high-water mark. Vector append offsets are rolled back for committed replay and incomplete tails are ignored. Shutdown rollback now matches the public transaction contract.
- Checkpoint smoke: normal shutdown leaves `wal.log` at 0 bytes; restart preserves committed rows; a SIGKILL after an uncommitted insert does not expose that insert after restart. Public harness: 20 cases, 0 failures.
- Correct-path comparison: 2,000 rows x 1,024 dimensions, batch 256, 8 queries x 3 runs. Old exact mean 0.00314 s; new exact mean 0.01145 s; new ANN mean 0.00588 s; new insert 1.619 s vs old 1.761 s; new index build 0.196 s. These numbers are a framework checkpoint, not the final 100k/300k result.
- Large experiments remain deferred until the integrated storage/recovery path and ANN maintenance are stable.

## 2026-11-14 protocol response contract pass

- Normalized the JSONL entrypoint so every successful SQL request carries `ok:true` together with its result or affected-row payload. Error responses already carry `ok:false`, a non-empty code, and a message.
- Strict `protocol_check/run.py` against the rebuilt binary: all 20 packaged public cases passed, including restart and shutdown rollback. This closes the distinction between semantic SQL correctness and judge-level response conformance.

## 2026-11-14 post-contract benchmark rerun

- Rebuilt after the response-contract fix and reran the same deterministic comparison once per round, three rounds total: old exact mean 0.00320 s; new exact mean 0.01155 s; new ANN mean 0.00586 s; new insert 1.616 s vs old 1.753 s; new index build 0.188 s. ANN returns the same ten IDs as exact on this workload.

## 2026-11-14 LSM framework smoke

- 80-row mutation/restart smoke crossed the 16-record MemTable flush threshold repeatedly and produced both L0 and L1 files (`L0-6.sst`, `L0-7.sst`, `L1-5.sst`).
- After restart, a scalar update remained visible through the rebuilt index, a delete stayed absent, and 79 live rows were returned. The smoke passed on the clean HDD data directory.

## 2026-11-14 contiguous vector distance pass

- The vector flush boundary now refreshes the read-only mmap after appending `vectors.f32`, so exact scans reuse a contiguous file window instead of falling back to one `pread` per row.
- Replaced the AVX2 distance kernel's float-to-double conversion loop with float SIMD accumulation and one final reduction. Public SQL: 20 cases, 0 failures; strict protocol package: 20 cases, all PASS.
- Same 2,000 x 1,024 benchmark, three rounds: new exact mean 0.00907 s (from 0.01155 s), new ANN mean 0.00543 s, old exact mean 0.00305 s; new insert 1.659 s vs old 1.721 s. This is a measured ~21% exact-query improvement, while the old implementation remains the current exact latency baseline.

## 2026-11-14 squared-distance ranking path

- Exact and ANN ranking now keep squared L2 distances internally and apply the square root only when serializing the projected distance. This removes one square-root per scanned row while preserving the public Euclidean result and tie order.
- The 20-case public harness remained at 0 failures after the change. The three-round comparison stayed within run noise at new exact 0.00911 s and ANN 0.00544 s, so this pass is kept for the cleaner ranking invariant rather than claimed as a standalone speed win.
