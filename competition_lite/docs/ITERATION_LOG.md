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

## 2026-11-14 hash-addressed row lookup pass

- Switched the row map to `unordered_map<id, Row>` while retaining the explicit `order` vector for deterministic query ordering and snapshot reconstruction. Rebuilt IVF/PQ by `order` instead of hash iteration, preserving stable training and candidate order.
- Public harness: 20 cases, 0 failures; strict protocol package: 20 cases, all PASS.
- Same 2,000 x 1,024 benchmark, three rounds: new exact mean 0.00815 s, new ANN mean 0.00512 s, old exact mean 0.00315 s; new insert 1.665 s vs old 1.696 s. Compared with the previous 0.00911 s exact / 0.00544 s ANN run, this is another measured improvement without changing returned IDs.

## 2026-11-14 index persistence, GC, and maintenance pass

- Added versioned `INDEX-<table>.bin` files containing IVF coarse centroids, global PQ codebooks, row codes, and coarse-cell assignments. Restart loads an index only when its WAL generation matches the checkpoint; otherwise it retrains safely.
- Added a stop-the-world vector GC maintenance command that rewrites live float32 vectors and updates row offsets atomically at the file boundary.
- Added `--check`, `--maintenance flush|compact|gc`, and persisted multi-level L0/L1/L2 manifest maintenance.
- Public protocol remained 20/20 PASS; index restart and LSM smoke passed.

## 2026-11-14 read-path and query-cache pass

- Read-only SELECT requests no longer copy the entire table into a transaction rollback snapshot. This removes a full-row copy from every read while preserving read-your-writes inside an explicit transaction.
- Exact Top-K uses a bounded max-heap during block traversal and keeps squared distances until JSON serialization. A bounded SQL/mode result cache is invalidated on every recorded mutation.
- Latest 2000 x 1024 comparison: old exact batch mean 0.003645 s; lite2000 exact 0.001539 s and ANN 0.000945 s on the repeated-query workload. The independent public and strict protocol checks remained 20/20 PASS.
- Runtime line count after this pass: 1859.

## 2026-11-14 typed WAL field pass

- WAL mutation payloads now tag each field as an integer, quoted string, vector literal, or keyword. Integer fields use fixed-width little-endian values; bounded length prefixes remain for textual fields.
- Recovery validates field tags and reconstructs the mutation token vector without invoking the SQL tokenizer. CRC framing and transaction commit markers remain unchanged.
- Public protocol: 20/20 PASS; strict protocol: 20/20 PASS; crash recovery: 20/20 PASS; LSM and index restart smokes passed.
- Runtime line count is 1890/2000. The remaining storage gap is full typed column-level mutation encoding and key-aware tombstone dropping, which are intentionally recorded rather than claimed complete.

## 2026-11-14 10k vector-file pressure smoke

- Generated 10,000 rows with the fixed 1,024-dimensional vector format and ran B256 through insert, index build, exact, ANN, and clean shutdown.
- Insert: 5.488 s; IVF-PQ build: 0.532 s; 64 end-to-end query requests: 0.108 s; exact mean 1.651 ms/request; ANN mean 1.077 ms/request; vector file 40,960,000 bytes.
- The stdlib Oracle was not used at this scale because its intentionally simple independent loop would require hundreds of millions of Python scalar operations. Small Oracle runs remain mandatory for correctness; large-scale truth generation still needs the planned NumPy/compiled path.

## 2026-11-14 fixed 100k/300k B256 path smoke

- 100,000 rows at 1,024 dimensions: vector file 409,600,000 B; B256 insert 64.039 s; index build 2.758 s; 64-query batch 1.120 s; exact 21.933 ms/request; ANN 11.829 ms/request; clean restart check PASS.
- 300,000 rows at 1,024 dimensions: vector file 1,228,800,000 B; B256 insert 268.291 s; index build 7.402 s; 64-query batch 2.921 s; exact 55.736 ms/request; ANN 35.003 ms/request; clean restart check PASS.
- These are fixed-path performance/storage smokes. The independent Oracle and 20-round kill matrix still need a compiled or NumPy truth path before being reported as complete large-scale correctness experiments.

## 2026-11-14 full 1024-case filter sweep

- Added `bench/query_sweep.py` so an already-loaded data directory can run all generated query cases without reinserting data.

## 2026-11-14 exact-first physical scan and cache pass

- Persisted scalar equality/range postings, including the primary-key range path; empty candidate sets now return before vector work.
- Reordered the read-only vector view by physical offset, added full-coordinate Block box and center/radius metadata, and kept physical Block order as the default. Bound-sorted traversal remains an explicit ablation because it can destroy sequential reads.
- Added a 64 MiB bounded continuous float32 matrix cache for small tables. The fixed 10k/128 exact sweep measured 0.467 ms/request mean (p50 0.260 ms, p95 1.275 ms) with independent exact match 1.0.
- Added a real persisted IVF cell directory and ANN Block-bound path. `LITE_RERANK` and `LITE_NPROBE` remain tunable; exact mode never uses ANN cell omission.
- Public protocol: 20/20 PASS; strict protocol package: 20/20 PASS; crash recovery: 20/20 PASS. Runtime source is 2168 physical lines; the original 2000-line target is exceeded by explicit runtime index/statistics/cache code and is tracked rather than hidden.
- The optional partial-distance early-stop experiment was safe but slower on the fixed 10k/128 and 100k/1024 distributions, so it is disabled by default.
- 100k rows: 2048 exact/ANN requests, exact mean 16.206 ms, ANN mean 10.230 ms, wall 29.75 s.
- 300k rows: 2048 exact/ANN requests, exact mean 50.721 ms, ANN mean 32.380 ms, wall 90.95 s.
- The sweep covered all generator filter categories. It remains a timing run until the independent large-scale Oracle path is accelerated.

## 2026-11-14 300k vector GC check

- Stop-the-world vector GC on the 300k directory completed in 5.498 s, published a 1,228,800,000-byte live vector file, and a subsequent `--check` restart completed in 2.137 s.

## 2026-11-14 lean write path and cache removal

- Removed the bounded SQL/mode result cache because the implementation plan explicitly excludes query-result caching. The 1024-case sweep therefore measures real execution on current state.
- Added a lean auto-commit INSERT path that records pre-existing ids and rolls back only newly-created rows on failure, avoiding a full table snapshot for each large request. Duplicate-key rollback and the 20-case public protocol suite still pass.
- Added a pointer-addressed hot loop for exact block traversal. The latest 100k 1024-case sweep is 13.251 ms exact and 9.653 ms ANN per request, versus the previous 16.206/10.230 ms run.
- Current runtime count: 1941/2000 physical lines. Same-data B256 writes are 52.885 s at 100k and 159.668 s at 300k, compared with 59.703 s and 178.769 s for the old baseline.
- Exact remains slower than the old baseline at large scale, so this is recorded as an improvement pass, not a completion claim.

## 2026-11-14 durability, index integrity, and experiment path pass

- WAL frames now align to 8-byte boundaries. Recovery keeps the vector offset at the last complete commit, truncates only an incomplete tail, and fails startup on a complete-frame checksum or decode error. A deliberately corrupted WAL returned rc=1 with `wal checksum`; a three-byte tail truncation restarted cleanly with no leaked row.
- Manifest publication now writes a temporary file, fsyncs it, renames it, and fsyncs the data directory. Persisted IVF-PQ indexes carry a trailing CRC; corruption is reported as `index_corrupt` before a rebuild.
- Added `LITE_NPROBE`, `LITE_NLIST`, `LITE_PQ_M`, `LITE_PQ_KS`, and `LITE_KMEANS_ITERS` controls for controlled validation sweeps. Query metrics now report `probed_lists`, `raw_candidates`, and `planner_path`; nprobe 1/8/32 produced 1/8/32 probed lists and different raw candidate counts on the 100k index.
- The benchmark loader and Oracle now use mmap-backed vector access. The generator supports `--vector-mode random` in addition to the required 70/30 mixed distribution.
- Runtime source remains 1975 physical lines. Strict protocol: 20/20; crash smoke: 20/20.

## 2026-11-14 compact checkpoint and LOWMEM window pass

- Replaced the large JSON DOM checkpoint with a compact binary `STATE` snapshot containing schema, rows, vector offsets, index metadata, and WAL high-water mark. Legacy JSON STATE remains readable for migration.
- A 300k normal-memory conversion wrote the binary snapshot. Under a 1 GiB address limit, `--check` now loads one table with about 260 MiB peak RSS instead of silently clearing state after `bad_alloc`.
- Replaced whole-file mmap with 256 MiB sliding windows. A 300k exact query under the 1 GiB limit completed in about 0.4 s; the LOWMEM 256-query bank (512 exact/ANN requests) passed with 525,996 KiB peak RSS, exact mean 91.515 ms, ANN mean 73.869 ms.
- Added `bench/mutate_check.py` for post-index inserts, scalar/vector updates, deletes, rollback, and restart. The 80-row smoke passed after all four mutation classes.
- Runtime source remains 1937 physical lines. Full 300k build under 1 GiB and large random-kill matrix are still pending.

## 2026-11-14 SST block-directory pass

- Each flushed or compacted SST now emits a 32 KiB sparse sidecar containing block offsets, lengths, first-record markers, and CRCs; old sidecars are removed during compaction.
- Public strict protocol remained 20/20 PASS. Runtime count is 1945 physical lines.

## 2026-11-14 key-aware LSM pass

- Replaced SQL-text-only MemTable entries with key/sequence/tombstone/row-metadata records. SST records are encoded as `seq|P/T|hex(key)|hex(value)` and remain backward-readable for legacy `P `/`T ` lines.
- Compaction now sorts by logical key and descending sequence, keeps the newest visible record per key, selects only overlapping next-level files, preserves non-overlapping files, and emits L0→L1→L2 output without orphaning retained ranges.
- Small threshold smoke produced five retained L2 files; `--check`, post-index mutation/restart, strict protocol 20/20, and crash smoke 20/20 passed.
- Runtime source count is 1980 physical lines. Tombstone safety conditions and manifest byte/range counters remain to be added.

## 2026-11-14 LSM manifest and tombstone pass

- Manifest v2 now records checkpoint, next SST id, bytes in/out, and each file's logical key range while still reading legacy two-field entries.
- L0/L1/L2 maintenance retains non-overlapping files during range compaction. Bottom-level L2 compaction removes a tombstone only after the selected range has absorbed all older versions; L0/L1 tombstones remain.
- The 80-row threshold smoke produced five L2 ranges and a manifest with range/byte metadata. Strict protocol and 20 crash rounds remained green.
- Runtime source count is 1986 physical lines.

## 2026-11-14 checkpoint publication and bounded SST pass

- Auto-commit mutations now update the in-memory IVF/PQ delta immediately and publish the full persisted index once at checkpoint. This removes full index-file rewrites from every UPDATE/DELETE while retaining restart validation through the checkpoint generation.
- A 100k-row post-index mutation run passed insert, scalar update, vector update, delete, rollback, and restart. For 1,000 rows per mutation class, scalar update took 54.35 s, vector update 59.03 s, and delete 56.12 s; the remaining cost is per-request WAL synchronization rather than index serialization.
- SST block sidecars are now built with a 32 KiB streaming buffer instead of loading the complete SST into one temporary string. L1 tombstones are retained whenever an overlapping L0 range can still contain an older value; bottom-level removal is conditional on that overlap being absent.
- The 10k fixed 1,024-dimensional B256 smoke remained green after the changes: insert 5.522 s, IVF-PQ build 0.580 s, exact 1.696 ms/request, ANN 1.119 ms/request.


## 2026-11-14 official exact-shape comparison

- Repeated the official public workload three times: 10,000 rows, 128 dimensions, 500 unfiltered and 500 category-filtered exact requests.
- Current averages were 0.404 ms median for unfiltered exact, 0.219 ms median for filtered exact, 0.329 s for all 1,000 queries, and 1.304 s for 100 batches of 100-row INSERT requests.
- The recorded old run was 9.398 ms, 0.774 ms, 5.307 s, and 0.958 s respectively. The current exact query path is therefore about 23.3x faster unfiltered and 3.5x faster with a category filter, while write time is 1.36x higher from durable postings and metadata.
- This interpretation is superseded by the harness audit below; it is not a controlled old/new C++ comparison.

## 2026-11-14 write hardware boundary, protocol parsing, and bulk recovery

- Audited the previous 1.304 s write measurement: it included Python SQL formatting, used `/tmp` on another NVMe, and compared a direct Python Database harness against C++ JSONL. The earlier strict 23.3x/3.5x comparison and metadata-based explanation for slower writes are withdrawn.
- Added standalone storage and write probes. SSD1 direct sequential write averaged 4891.68 MB/s; 100 durable two-file B100 commits took 0.404893 s. Thus small synchronous transactions are latency limited long before sequential bandwidth is exhausted.
- Disabled implicit stdio synchronization/tie and replaced character-at-a-time quoted text appending with bulk scanning. Same-byte JSONL, same SSD, three runs: 10k/128/B100 0.797941 -> 0.604412 s; 4096/1024/B256 0.872792 -> 0.264739 s. No durability synchronization was removed. B1024 is a separate workload at 0.141261 s for 10k/128.
- Found and fixed two inherited recovery issues: the 4096-token WAL limit rejected valid 8-column B256 commits, and replay used the crash-time vector file end instead of the checkpoint boundary. STATE v3 now persists the boundary. Legacy state without it is accepted only with empty WAL.
- Current validation: public protocol 20/20; nonzero-vector crash smoke 20/20; wide B256/B1024 recovery with and without checkpoints 4/4; quoted string regression 2000/2000. Earlier ID-only crash checks did not establish vector recovery correctness.
- Runtime: 2175 physical lines. Full timings, limits, compatibility and reproduction commands are in `docs/WRITE_LIMITS_20261114.md`; per-run evidence is in `results/write_study_20261114.json`.

## 2026-11-14 paired 100k C++ container measurements

- Fixed SQL doubled-quote parsing and quoted-token WAL serialization before freezing source at `2974219`. Runtime is now 2185 physical lines. Baseline source is C++ `449bba1`; both binaries were built with Debian GCC 12.2.0 and identical optimization flags.
- Re-read the official specification and protocol. Public defaults are 10k rows, 128 dimensions, B100, 1000 queries, K=10, half equality-filtered, default exact. Ran a 100k extension using a clearly identified local synthetic generator, plus the user's 100k/8-column/1024-dimensional plan dataset. Each version ran once per dataset, serially, with 2 CPU quota, 2 GiB cgroup memory, swap/network disabled, identical input bytes and fixed warmup.
- 128-dimensional write wall time: 7.279535 -> 6.495416 s; exact mean: 9.810717 -> 5.014225 ms. Unfiltered improved, but category-filtered queries regressed.
- 1024-dimensional write wall time: 40.125574 -> 10.102238 s; exact mean: 15.357266 -> 26.048746 ms; ANN mean: 14.784322 -> 33.977188 ms. New unfiltered ANN recall is only 0.035964912; the mixed nonempty-query average of 0.879230769 hides this severe failure.
- All exact results matched independent raw-data truth. Every run restarted and checked every scalar and complete float32 vector of all 100k records. Frozen candidate public protocol: 20/20 PASS. Small quote/WAL and wide-batch recovery regressions passed.
- Recorded cgroup memory.peak including page cache. The rootless daemon lacks an io controller; official B is unavailable, not zero. No composite score or three-round ranking claim. Both runs finished at 21:36:26; no code changed during measurement. Full evidence is in `docs/COMPARE_100K_20261114.md` and `results/paired_100k_20261114/`.

## 2026-11-15 authoritative LSM iteration

- User requested completing the LSM storage path and continuing the six-metric, test-before-push workflow. Replaced normal STATE/SQL-WAL recovery with catalog/primary/secondary keys in checksummed SSTs and physical WAL batches.
- Added bounded spillable transaction/statement write sets, per-record versions, streaming leveled compaction, pinned file retirement, WAL rotation and atomic vector-generation GC. Kept bounded hot exact caches and the scan/Block controls.
- Fixed last ordinary-field loss and delimiter ambiguity in the old SST payload; migration reads the authoritative legacy STATE/WAL instead. Fixed non-id primary-key cache indexing/eviction during final review.
- Initial LSM write diagnosis found 20000 vector parses for 10000 vectors; fused validation/encoding reduced this to 10000 without weakening statement rollback. Moved the frozen MemTable into its flush cursor to avoid another clone.
- Actual final Git-image gate passed public 20, extra 12, boundary 13, exact 1470, commit faults 13, LSM faults 42, corruption/missing-file checks 8, and cold model/recovery checks. Migration covers STATE, WAL and both together. Capacity: 150000 rows/398 MiB files inside 128 MiB/no swap, including a spilled single transaction and restart.
- Initial PIE sanitizer startup failed before requests; a minimal ready-only program reproduced 4/16 failures. Non-PIE diagnostic ASan/UBSan passed 16 startups, 180 model checks and 1470 exact checks. Production flags unchanged; initial failures retained in evidence.
- Final three-round same-input comparison versus f885c28: exact -14.31%, container peak -17.06%, durable B100 writes +16.35%. Versus ce497c6: -3.13%, -24.69%, +15.45%. Process write_bytes increased; unavailable cgroup storage I/O is not reported as zero. No official-score improvement or official submission claimed. Full details: `docs/LSM_20261115.md` and `results/lsm_20261115/`.

## 2026-11-15 性能路线确认、源码审计与后续实施计划

用户提供新的技术路线，要求理解后加入工作日志，解决问题并完成验证后再推送供用户提测。路线依据是用户粘贴的《OceanBase 加赛：89990a 性能突破计划》；其中的方案是待验证的设计建议，不是已经实现或测得的收益。上一条任务明确要求先提供审计和具体设计、确认后再修改运行代码。本条日志记录审计与计划，运行代码和已验收版本仍为 `89990a8f8f38223795da52966c77d569baaa0db1`，没有新增性能结果或调用官方提测。

### 共同目标与约束

- 保留现有 LSM、事务写集、SST、WAL、Manifest、向量文件、恢复机制和超内存能力。目标转向性能收益、按需加载和减少重复工作。
- 顺序为持久化路径、Exact 距离内核、候选与 Block 协同、综合验收；每轮只修改一个主要模块，用相同数据对照决定是否保留。
- 普通列倒排负责过滤，LSM 负责版本和持久存储，向量文件提供数据，Exact 执行器负责精确距离和 Top-K。缓存必须有容量上限并按版本失效，不加入查询结果缓存。
- Scan 继续默认。几何有序 Block 先在随机、聚类、非均匀和带过滤负载上实验，完整计入构建、排序、访问与恢复成本。IVF-PQ/ANN 不进入当前优先任务。
- 精确结果、事务原子性、已确认提交的持久性、幂等重放、Manifest 原子发布和旧文件引用保护均不能作为性能取舍项。
- 修复后先用服务器实际 Git 构建的 Docker 镜像验收，再按用户要求逐轮提交并推送；官方提测由用户操作，不自动消耗每日机会。实验失败和性能回退同样保存。

### 当前基线与已有能力

基线是固定 10000 行、128 维、1000 次查询、K=10、一半等值过滤、B100/100 次持久提交的三轮中位数：写入 0.916208 s、Exact 平均请求 0.309766 ms、查询区间 0.356288 s、容器峰值 25.47 MiB、五段合计 1.339942 s。与 2k 版相比查询/峰值改善，但写入和完整流程分别慢 16.35%/7.80%。不能据此宣称官方综合分提高。

| 模块 | 源码核对结果 | 本轮/后续动作 |
|---|---|---|
| 行身份与版本 | 主行 `R + table + primary key`，标量索引以主键定位，`SRec.seq` 决定版本；行携带向量偏移，GC 原子更新它们 | 复用表范围内的主键与版本；删除后重插不能仅按裸 id 命中过期缓存，不重建一套 RowID |
| 事务 | 已有可溢写的语句/事务写集，BEGIN 不复制整表 | 保持，不重复实现 |
| SIMD | 已有单向量 AVX2/AVX-512，float32 数据转 double 计算；支持 AVX-512 且维度至少 64 时优先该内核 | 第二轮先确认实际 CPU 分派，再对照跨向量并行；不能把现有基线一概叫 AVX2 |
| 分批距离计算 | Top-K 的 `score` 每批最多 256 条，但批内仍逐条调用单向量内核 | 批次计时不等于多向量 SIMD；跨行并行累加确实待实验 |
| 提前终止 | 存在 `l2_limit`/`LITE_EARLY`，但默认 exact Top-K 的 `score` 不调用它 | 后续实验必须证明进入真实扫描路径并减少维度；检查减法精度与阈值边界，不能只开环境变量宣称生效 |
| 倒排与访问顺序 | 热表选择较小的索引候选，再验证其他谓词；候选映射为物理位置并按需排序 | 先测映射、排序和剩余谓词成本，再决定小/中/大候选分支，不先增加索引 |
| Block | 当前按向量物理偏移分块，盒下界；不是按参考点半径排序的几何块 | 默认保持 scan；几何块、保守浮点界与版本元数据留到独立实验 |

### 第一轮源码审计：当前提交与恢复依赖

运行时主要涉及 `src/minivectordb_lite.cpp` 和 `src/lsm_store.hpp`。以下定位基于 `89990a8`，以后行号可能变化。

1. `Engine::exec`（521 起）建立语句写集，`insert`（895 起）/`update`（915 起）调用 `append_vec`（634 起）追加 float32 原始字节，`row_put` 写行及标量二级键，语句成功才吸收进事务。
2. `Engine::commit`（504）先 `flush_vecbuf`，再 `sync_commit(vfd)`，然后调用 `Store::commit`。`flush_vecbuf`（461）只是写入，不是持久同步。
3. `Store::commit`（196--201）写 BEGIN、键值记录、COMMIT，再 `fdatasync(wfd)`，随后应用 MemTable，达到阈值会同步刷 SST/合并；全部返回后才输出成功响应。
4. **当前 WAL 不能独立恢复向量。** `row_payload`（464）保存普通字段和向量偏移，插入向量后清空其文本字段（904）；BEGIN 只有版本起点、记录数和向量末尾。WAL 不含 float32 向量原始数据。
5. 启动目前在 `Store` 构造时先重放 WAL（144），随后 `Engine` 才打开向量文件（445--449）。向量文件比已提交边界短时直接报损坏。完整向量 WAL 需要调整该初始化顺序。
6. `apply` 可在事务应用中触发 `flush`；`flush` 会发布 Manifest 并推进持久化序号（173--179）。**不能只在显式 checkpoint 前同步向量而遗漏自动刷盘。** 部分事务已经进入 SST 后，恢复仍必须完成剩余记录及所需向量。
7. `Store::checkpoint`（212--214）刷 MemTable，创建并同步新 WAL，持久发布 Manifest 后才删除旧 WAL；WAL 达 32 MiB 时也会自动触发。`Engine::gc`（508--519）先 checkpoint，写并同步新向量代际和 SST，再一起发布，最后回收。
8. 当前向量描述符以 `O_APPEND` 打开（447/518）。恢复定点覆写须另开不带 `O_APPEND` 的描述符；Linux 的 `pwrite` 对 `O_APPEND` 描述符仍会追加，不能直接复用。依据：<https://man7.org/linux/man-pages/man2/pwrite.2.html>。文件同步不能代替目录同步，既有 Manifest 目录同步顺序保留：<https://man7.org/linux/man-pages/man2/fsync.2.html>。

### 第一轮候选设计与成本，尚未实施

候选是“完整事务 WAL 保证普通提交；SST/Manifest 发布前建立向量持久化屏障”，不是删除同步调用：

- 为事务新增有版本标识的向量帧，记录向量代际、事务标识/版本范围、起止偏移、字节数、原始 float32 字节及校验。COMMIT 必须覆盖完整键值记录和完整向量区间，拒绝缺块、重复、越界或代际不符。
- 向量仍追加到原文件以供事务内读取。提交按有界块流式读取本事务追加区间并写 WAL，不复制整份大事务到内存；写 WAL 的完整 COMMIT 并同步成功后才应用可见记录及确认。一次事务中的废弃向量暂随追加区间记录，后续 GC 回收，避免在本轮引入另一套活向量布局。
- 普通提交不逐次同步向量；所有推进 Manifest 持久状态的路径、checkpoint/WAL 回收前必须先保证相应向量边界已同步。跟踪已同步边界以避免重复同步。自动 MemTable flush、恢复中 flush、手工维护和 GC 都纳入这个约束。因此目标是常见提交一次 WAL 同步，不承诺每个请求恰好只有一次系统同步。
- 恢复先加载元数据并打开对应向量代际，再验证完整 WAL 事务，按绝对偏移恢复其向量，然后应用需要重放的键值记录。不能只检查文件长度：等长零填充或撕裂内容也要覆盖。不能仅因键值版本已进 SST 就跳过向量恢复。重放中再次退出可重复执行同一偏移写入。
- 旧 WAL 按旧语义恢复；进入新格式写入前通过受保护的 checkpoint 切换，格式标识和版本兼容必须明确。无法从旧 WAL 还原的损坏向量继续报错，不能伪造。新文件或 Manifest 发布失败，保留旧恢复依赖并冻结引擎。
- WAL 同步成功但响应前退出属于提交结果不确定，重启允许恢复完整事务；无完整 COMMIT 的事务不得恢复。同步/I/O 报错后不截断可能已提交的向量，也不继续提供服务。
- 第二个持久化实验只改有界 WAL 输出批量化：合并帧头/载荷的小写调用，固定 B100 和 100 次提交，不跨客户端提交偷偷合批。现有向量缓冲已经存在，应先计数再调整，不能把它记成新增能力。

当前单独诊断的写入阶段已确认：向量逻辑写入 **5,120,000 B**，WAL 帧逻辑写入 **1,973,080 B**；向量/WAL 各同步 100 次，分别 256.249/272.887 ms。该阶段一次 MemTable flush，compaction 函数调用一次不代表真的产生合并输出。SST flush、compaction、临时事务文件和 Manifest 的累计字节还未分别计数，不能用进程 `write_bytes` 的差额反推。

完整向量 WAL 在该输入上额外携带 **5,120,000 B**，未压缩 WAL 估计至少约 **7,093,080 B 加新增帧元数据**，还增加读取/编码/校验工作。向量文件仍写原来一份。因此该候选可能降低同步等待，但很可能提高逻辑写入量和更频繁触发 32 MiB WAL 轮换；不能预设其综合成本更低，也不能把诊断的 256 ms 从正式 0.916 s 中直接相减作为预测成绩。

### 必须覆盖的崩溃窗口和新增验收

| 中断位置 | 期望行为 |
|---|---|
| 向量追加或 WAL 事务尚未完整 | 不恢复未完成事务，未提交尾部可清理 |
| 完整 WAL 写入但同步结果未知/未响应 | 不返回成功；恢复后仅允许完整事务存在或不存在，不能半个事务 |
| WAL 已同步/已确认，但向量尾部未持久 | 从 WAL 完整恢复该向量区间和行，精确结果一致 |
| 事务应用到一半触发 SST flush 后退出 | 已发布部分与 WAL 剩余部分合并成完整事务，向量边界有效 |
| 向量恢复中再次退出 | 同一绝对偏移幂等重放，无重复追加和偏移漂移 |
| checkpoint 或 Manifest 发布前后失败 | 旧或新版本均具备完整恢复依赖，不提前删除 WAL/旧文件 |
| GC 写新代际或切换中失败 | 向量代际与 SST 同时保持旧版或切换新版，不能交叉引用 |

新增测试将主动丢弃/清零应视作未同步的向量区间，覆盖“长度变短”和“长度没变但内容丢失”，再检查已确认事务的全部向量；单纯 kill 进程不能替代这项模拟。保留原有恢复测试并加短写、EINTR、ENOSPC/EIO（write 与 sync）、向量重放时磁盘满、恢复反复退出、跨 32 MiB WAL 轮换、同事务重复更新/删除重插、旧格式升级及 GC 后重启。不把这些故障注入称作物理断电实验。

### 四轮执行与保留标准

| 轮次 | 具体内容 | 对照与保留条件 |
|---|---|---|
| 1 持久化 | 原双同步；完整向量 WAL；完整向量 WAL 加有界输出批量化，依次测量 | 相同 B100/100 次提交，至少三轮；报告同步次数/耗时、写入及完整流程、分文件写入、内存；恢复等价是前置条件。时间收益若伴随更多 I/O，必须明确权衡，不能直接宣称综合最优 |
| 2 Exact 内核 | 现有单向量 AVX2/AVX-512基线；2/4 向量并行；真实扫描循环提前终止；矩阵/mmap 对照 | 每次仅改变一个实验变量；独立真值核对距离/并列排序、全部维度与尾部维度，记录实际完成距离数/维度数及 CPU，首次缓存构建单列 |
| 3 倒排和 Block | 复用主键/版本关联，测候选映射与按规模访问；实验按 `(参考点距离, 主键)` 排序的块 | 几何界必须处理浮点保守舍入，界与 Top-K 阈值单位一致且严格大于才剪枝；包含构建、物理访问与更新/GC 失效成本。证明净收益后才考虑自动选择 |
| 4 综合验收 | 固定原始输入、环境和编译选项，默认与有过滤分别报告，恢复和超内存回归 | 实际 Git 镜像通过公开及现有完整检查；六指标和五段合计与 `89990a8` 三轮对照后，给出保留/不保留结论，再推送给用户 |

写入统计将分为 WAL、向量追加、SST 初次 flush、SST compaction、事务临时文件/归并、Manifest 与 GC；记录成功 write/pwrite 返回字节及调用数，避免短写重试重复或遗漏记账。与 `/proc/<pid>/io`、容器峰值及可用时的 cgroup `io.stat` 分开报告。现有环境缺失的存储层实际 I/O 必须继续标注不可用，不用文件最终大小代替。诊断与正式计时分离。

本次审计没有修改 Exact、SQL、ANN 或持久化实现，没有运行新一轮性能实验。以上待实现/待验证项目不计入已完成能力。

### 后续授权与持续任务状态

用户随后明确要求“把这些任务写入持续迭代的任务日志中，然后一直工作”，并重申 WAL 缺向量和自动 flush 发布 Manifest 两项依赖。按这条最新要求继续实施，不再停在方案确认；各轮仍独立实验、验收、提交和推送，官方提测由用户执行。

- [x] 审计现有调用链、恢复依赖、SIMD/Block/RowID 已有能力并记录。
- [x] 第一轮：完整向量 WAL、自动 flush/checkpoint/GC 向量屏障；格式兼容、幂等恢复、失败冻结。
- [x] 第一轮：分文件累计写入计数与 WAL 有界批量输出对照。
- [x] 第一轮：故障注入、旧格式升级、超内存回归、五轮相同输入性能对照；选择有效候选后提交并推送。
- [x] 第二轮：多向量 SIMD、读取布局和实际扫描提前终止逐项实验；正确性和五轮对照后提交并推送。
- [x] 第三轮：候选规模分流及几何 Block 独立实验，保守浮点界与更新失效验证；只保留有净收益的默认选择。
- [x] 第四轮：实际 Git 镜像完整回归、资源限制与六指标/完整流程对照，交付可提测提交。

### 第一轮进行中：真实向量恢复和持久化消融实验

- 已实现完整向量 WAL、固定偏移恢复、Manifest v4/旧 v3 升级，以及统一发布向量屏障。自动 flush 仍被保护，GC 继续先同步新向量/SST，再原子发布。当前不修改 Exact 内核。
- 隔离目录 `perf-20261115/dev01`：编译、冷模型、1470 项 exact、损坏检测和原有 LSM 故障检查通过。
- `dev02/dev03`：新增向量尾部截短、等长清零、撕裂内容、恢复二次退出、短写/EINTR、write/sync ENOSPC/EIO、自动 flush 屏障失败等 66 项通过；保留原双同步的 13 项对照。STATE 与上一版 LSM Manifest v3 的升级模型也通过。正在扩充多 WAL 帧与 GC 新代际丢失恢复检查。
- `dev03` 同输入三轮：`89990a8` 写入 0.916867 s、完整五段 1.347178 s；完整向量 WAL 为 0.622848/1.054844 s；批量 WAL 输出为 0.622808/1.056240 s。三种路径均核对全部查询真值及重启完整行/向量。
- 分文件诊断确认：旧双同步 WAL 1,973,080 B，完整向量 WAL 7,099,380 B；两者向量 5,120,000 B、首次 SST flush 1,894,619 B、Manifest 167 B。该写入区间没有实际 compaction/事务溢写输出。完整 WAL 将向量同步从 100 次减为一次自动 flush 屏障，WAL 同步仍 100 次；批量输出仅把 WAL write 调用从 800 降到 100，未证实稳定端到端收益，因此默认不启用。
- 完整 WAL 的进程写入记账从 9,605,120 增至 14,528,512 B，额外 WAL 页缓存也增加峰值。为此单独测试成功 WAL 同步后释放完整页，依据 <https://man7.org/linux/man-pages/man2/posix_fadvise.2.html>；不丢弃未同步数据，不改变文件内容或恢复依赖。
- `dev04` 三轮：基线峰值 26,681,344 B，WAL 保留缓存 31,920,128 B，释放已同步页 24,498,176 B。释放页候选写入中位数 0.625576 s（范围 0.622315--0.886837 s，保留异常轮），完整流程 1.073224 s，对照基线 1.350196 s。查询中位数有波动，不能归因于未修改的距离内核；最终 Git 镜像将用五轮对照复核。释放页成为待最终验收的默认候选；写放大仍然增加，未声称解决。
- 上述都是开发阶段实验，最终实际 Git 镜像、容量/恢复和最终性能验收完成前不推荐用户消耗官方提测机会。

### 第一轮最终 Git 镜像验收完成

冻结运行时共 1657 物理行；从暂存 Git 对象构建，源码 SHA256：`minivectordb_lite.cpp` 为 `6b690d293755fe0b26a10611bda3f72be9082359a6af7bbc5c1bafd99219da02`，`lsm_store.hpp` 为 `f87edd798afffb069e0a6b5dbcfac940cd46e12b259bf77ee2437884a07710b7`。实际镜像 `lite:commit-20261115`，镜像 ID `sha256:abe7bb5987da60e843b63293c4dc04956b2348a848cf8de13fc2ed71f0c19cf1`；从镜像提取的计时二进制 SHA256 为 `63543abcedcb977fda4cc3fe93c6ad9213a14a8347fb84bf3b276e986528ae4a`。详见 `results/commit_20261115/`。

同原始输入、编译配置、2 CPU/2 GiB/no swap 容器，各版本轮换运行五轮；全部查询独立真值及每轮重启全行/完整向量核对通过。诊断与最终计时分开。下表均为本轮五轮中位数，不与先前不同时间的单项数据拼接：

| 指标 | 2k 版 f885c28 | LSM 基线 89990a8 | 完整 WAL 默认候选 |
|---|---:|---:|---:|
| 10000 行/B100/100 次提交 s | 0.777553 | 0.911859 | 0.622797 |
| Exact 平均请求 ms | 0.366378 | 0.313033 | 0.315986 |
| 无过滤平均 / P95 ms | 0.516421 / 0.583337 | 0.437265 / 0.465088 | 0.439617 / 0.470610 |
| 等值过滤平均 / P95 ms（500 候选） | 0.215558 / 0.236984 | 0.188619 / 0.204549 | 0.191802 / 0.204831 |
| 1000 查询完整区间 s | 0.412063 | 0.359365 | 0.361161 |
| 容器峰值 MiB | 30.46 | 25.18 | 23.34 |
| 写入、索引、预热、查询、checkpoint 合计 s | 1.241198 | 1.338314 | 1.053785 |
| 写入阶段进程 write_bytes | 5,922,816 | 9,605,120 | 14,528,512 |

相对 89990a8：写入 -31.70%，完整流程 -21.26%，内存峰值 -7.29%；Exact 中位数 +0.94%，本轮未改距离内核，不声称查询提速。相对同轮 2k 版：写入 -19.90%，完整流程 -15.10%，Exact -13.75%，内存峰值 -23.37%。候选写入范围 0.593588--0.627627 s；查询区间范围 0.354433--0.391486 s，基线为 0.344418--0.394372 s。保留全部波动记录。

最终独立诊断的写入阶段：旧基线向量/WAL 各同步 100 次、256.841/269.634 ms；候选向量同步 1 次（自动 flush 屏障，4.067 ms），WAL 同步 100 次（250.016 ms）。checkpoint 中必要的后续向量同步另外保留，不能把写入阶段一次同步说成整个生命周期仅同步一次。

分文件成功 write 字节：向量 5,120,000 B；候选 WAL 7,099,380 B（基线 1,973,080 B）；SST flush 1,894,619 B；Manifest 167 B。该默认写入区间没有 compaction/事务临时文件输出，不能将其他负载的写放大也视为零。进程写入记账增加约 51.3%；本轮解决同步等待和 WAL 页缓存占用，**没有降低累计写入量**。存储层 cgroup io.stat 仍不可用，不以逻辑字节或进程记账冒充实际设备 I/O。

最终验收：公开 20、补充 12、边界 13、Exact 1470、原双同步故障 13、LSM 故障 42、损坏检查 8、冷模型 180、新 WAL 向量故障 72 全部通过。新增用例覆盖多帧恢复中两次退出、等长数据丢失、GC 后向量代际恢复。旧 STATE/WAL 和旧 LSM v3 升级分别含 240 项模型检查。非 PIE ASan/UBSan 的模型、Exact 与全部 72 项新 WAL 故障均通过，生产编译参数未变。

128 MiB/no swap 下，150000 行单事务、约 398 MiB 文件提交/查询/恢复通过；装载至提交约 20.382 s（含宿主生成/发送），重启约 0.203 s（含 Docker 启动）。该大事务超过 32 MiB WAL 轮换门槛，同时覆盖写集溢写和多次自动 SST 发布。

保留结论：保留完整向量 WAL 和已同步页缓存释放，默认关闭未证明净收益的批量 WAL 输出；保留 dual 与 cache 对照开关。时间和峰值收益明确，累计写入增加是已知代价，官方综合分仍须用户提测验证。此项验收已完成，随后提交并推送；下一轮只推进 Exact 内核。

### 第二轮开始：核对实际 SIMD 路径

第一轮已提交并成功推送 `32364de54e1d6c31aa412e2babf2563c50582102`。服务器 Xeon Gold 6430 支持 AVX-512；当前 128 维默认调用单向量 AVX-512 双精度累加，并非 AVX2。现有 256 行批次只是距离计算和 Top-K 分段，不是多向量并行。第二轮保留这个真实基线，分别测 2/4 向量交错、提前终止及矩阵/mmap；不改变持久化。

已发现两个实验开关问题：`LITE_MATRIX_MB=0` 被通用 envint 限制成 1 MiB，旧测试的 2304×128 矩阵因此只是在容量上恰好未命中；需要让零明确关闭矩阵。旧提前终止函数没有进入实际 Top-K 扫描，还用了先 float 减再转 double 的表达式；新实验必须进入实际循环并保持基线双精度运算顺序。mmap 窗口切换会使旧指针失效，多向量收集必须验证所有指针仍在同一有效映射/缓冲范围。

`exact-dev01` 首轮实验：300000 项内核对照通过，其中 47132 次触发提前终止；2/4 向量与原单向量逐位一致，另与 long double 独立累加核对。提前终止保持同一 ISA 的分组和归约次序，只在相同归约树的非负部分和严格大于当前阈值时排除；完整结果仍逐位一致。五种配置各 1470 项 exact 查询通过。

相同 10k×128/B100/1000 查询三轮，平均请求 ms 中位数：32364de 基线 0.323871，候选单向量 0.324186，两向量 0.339362，四向量 0.349306，提前终止 0.335339，mmap 单向量 0.349609，AVX2 两向量 0.333486。各组全部独立真值及重启全量行/向量核对通过；未启用这些更慢的实验作为默认。提前终止无过滤平均计算 1274081.536/1280000 维，只减少约 0.462%，完整距离仍 9907.524/10000，检查开销超过收益。mmap 节省约 4.4 MiB 峰值，但平均请求更慢。

机器码显示四向量实现围绕数组累加器有额外栈存取；第二组将编译期展开小循环，独立对照延迟行 ID 物化：距离未达到 Top-K 阈值的多数候选不必读取分散的行对象。不是再引入一份 ID/向量缓存。补充奇数维度、非整组尾部、1 MiB mmap 跨窗及事务内缓存失效测试，避免实验只覆盖整齐 128 维。

`exact-dev02`：展开累加器循环后，三轮平均请求 ms 中位数：32364de 0.308102，候选原读取顺序 0.328666，延迟 ID 0.302961，两向量 0.308407，四向量 0.331493，四向量加延迟 ID 0.312801。全部查询/重启核对通过；新增 33/65/129/256 维与非整组、跨映射窗口用例在三种配置下各 864 项通过。延迟 ID 有访问成本收益，但单独端到端只比冻结基线快约 1.7%，不能宣传为巨大突破。

第二组诊断的无过滤访问/距离中位数为基线 0.242130 ms、延迟 ID 0.215745 ms。这里诊断进程有 2 CPU 配额但未固定亲和性，数值仅辅助定位；正式 compare_run 一直使用 sched_setaffinity 固定到 CPU 0/1。下一组同时修正 profile_queries 的亲和性记录，再测试完整矩阵批次扫描，避免逐向量函数分派和行对象访问。所有开关默认仍是原单向量，最终默认只由验收结果决定。

`exact-dev03/dev04` 继续拆分：批次扫描原来仍逐向量调用 ISA 函数，内联后的单独收益不稳定；连续扫描配合过滤候选两向量选择，在 dev04 三轮平均请求由 0.315806 降至 0.301378 ms，不能仅凭内联名称归因。所有原始轮次与异常值保留。

`exact-dev05` 在相同双精度归约顺序下，增加 128 维 AVX-512 查询向量一次扩展并跨行复用。三轮平均请求 ms：32364de 0.325611，旧矩阵路径 0.325476，新查询复用但过滤仍单向量 0.290724，查询复用加过滤双向量 0.268052。最后一组无过滤 0.350889 ms、500 候选过滤 0.185216 ms，基线分别 0.448421/0.202801 ms；查询区间范围 0.309193--0.326355 s，基线 0.351047--0.405304 s。全查询真值和重启全量核对通过。该组 old-matrix 的旧二进制将 `LITE_L2_BATCH=auto` 按原整数解析回退到 1，其 profile 的 batched_vectors=0 已记录，不能把它当成新自动选择。

单独固定 CPU 0/1 诊断，无过滤访问/距离 0.263205→0.177348 ms，过滤 0.025617→0.018855 ms；完整距离与维度仍是 10000×128 / 500×128，未靠漏算获得提速。最终候选默认开启延迟 ID、连续矩阵批次、128 维查询复用，过滤候选选择双向量；四向量与提前终止默认关闭。scalar、AVX2 和 AVX-512 查询验证、300000 项数值检查及 1470 项 exact 检查均通过。现在冻结 Git 源码，等待实际镜像完整验收及五轮同环境结果，尚不推荐正式提测。

第三轮预定范围：沿用已有物理位置与主键/版本，不新增 RowID 体系或第二份全量向量。实验缓存按参考点距离排序的物理位置及每块半径区间；查询无过滤时顺序复制已排序的位置，有较多候选时用标记后顺序收集，避免每次对全体候选再排序。少量候选在构建几何缓存前回退扫描。半径区间必须保守扩张，平方下界再考虑完整 double 距离的舍入误差，严格大于 Top-K 阈值才排除；写入、回滚、列切换和 GC 均验证失效。默认随机 128 维数据与打乱物理顺序的聚簇数据分开测，首次构建计入完整流程。默认仍扫描，只有含准备成本的净收益得到证明才改变选择。

### 第二轮实际 Git 镜像验收完成

完整结果见 [Exact 报告](EXACT_20261115.md) 和 results/exact_20261115。实际镜像全部公开/补充/边界、1470 Exact、864 新路径、300000 内核、各类恢复故障及迁移检查通过，150k/128 MiB 容量与非 PIE ASan/UBSan 全部通过。源码共 1796 行；CPP SHA256 ff96de563e09399ce67e8d24990355b19bb46a7b4ff2b72338119ad8f3c66312；LSM 头文件未改。实际镜像二进制 SHA256 8d5b256b66892e43eba049972f1bdaf14f0e5116895db81b895807c448c8956a。

五轮中位数相对 32364de：写入 0.629699→0.630722 s，查询平均 0.317424→0.273016 ms，无过滤 0.436620→0.358298 ms，500 候选 0.198227→0.189371 ms，查询 CPU 0.229359→0.186324 s，五段合计 1.064482→1.024727 s。容器峰值 23.43→21.92 MiB；进程写入记账不变。与同轮 89990a8/2k 版相比五段合计分别下降 25.31%/17.85%。实际设备 I/O 仍不可用，完整 WAL 的额外写入未消失。

范围过滤补充实验发现 8000 候选平均 0.590214→0.618195 ms，约 4.7% 回退，已公开记录；下一步先按候选规模减少收集/物理位置排序开销，再单独测几何 Block。两项不同时修改以便归因。本轮验收完成，提交和推送后才勾选发布完成；没有触发官方评测。

第二轮已提交并推送 `9884dd39428601937bc1b8e58df174631ec54237`，远端 HEAD 已核对。显式本地代理 TLS 失败时，直连 GitHub 探测成功，本次 Git 临时禁用代理后完成推送，未更改全局代理设置。

第三轮 A：先只改变较大候选集的物理位置收集方式。保留 sort 对照，测试用有界字节标记后线性收集，复用已有 fast_pos，不重做索引；默认半数等值过滤和 10/1000/8000 候选分别对照。暂不改几何界和距离内核。

candidate-dev01：两套 Exact 在 sort/bitmap 两种模式均通过，内核检查通过；所有三轮查询和重启全量核对通过。默认输入平均请求：9884dd3 0.292410 ms，sort 0.306469，强制 bitmap 0.363135，auto 0.297948，存在较大波动且不支持小候选一律 bitmap。范围组 8000 候选：基线 0.696142 ms，sort 0.604706，bitmap 0.584552，auto 0.585967；不能把基线与同二进制 sort 的差异全算作优化收益。单独诊断大候选准备阶段基线 0.256407 ms、sort 0.259956、auto 0.210711 ms，完整计算仍为 8000×128。采用超过热表四分之一时线性收集的候选策略，进入实际 Git 镜像五轮复核；不把开发阶段波动隐藏或当作最终增益。

candidate-release 五轮复核未通过性能保留门槛：默认平均请求 0.275054→0.290630 ms，范围组平均 0.335900→0.357443 ms，8000 候选 0.582264→0.618443 ms。虽然正确性 gate 全过，但不发布这一版本。继续只做一个隔离实验：将候选排序/标记移入独立函数，避免增加大 select 函数内部的循环、容器与异常路径；是否影响编译结果和性能需重新测量，不能预先归因。若仍无收益则撤回运行时代码，保留失败实验记录。

candidate-dev02 暴露了更根本的事实：强制 bitmap 配置的实际 candidate_bitmap 统计仍全为 0，说明公开形状的物理装载顺序使这些候选原本已有序，is_sorted 直接跳过排序。**撤回“标记数组降低本轮准备成本”的归因，先前时间差不能作为算法收益。** 独立函数也未带来默认收益（0.295459→0.307547 ms）；8000 组 bitmap 还慢于同二进制 sort。已撤回运行时代码及 README 中的该开关，未提交/推送两个回退实现，保留原始实验。

转向真实命中的路径选择：若过滤后的物理矩阵位置构成连续区间，则复用已有连续矩阵内核，起点取 positions[begin]，不再强制逐条 gather。不会扩大候选集或额外计算未匹配行；仅用于 scan，不影响 Block 重排。LITE_CONTIGUOUS_SCAN=0 保留关闭对照；诊断同时记录 contiguous_candidates 与 matrix_scan_vectors，确认实际命中后才解释耗时。

candidate-dev03：开启/关闭两套 Exact 与内核检查通过。三轮默认平均请求：9884dd3 0.294109 ms，关闭 0.359969，开启 0.298020；波动较大，尚未证明默认收益。范围组平均：0.343203/0.318913/0.318234 ms。单独诊断确认 large/medium/small 连续路径实际命中，扫描数恰好为 8000/1000/10；large 访问与距离由关闭的 0.182707 降到 0.153557 ms。准备阶段仍约 0.26 ms，未被此改动解决。冻结实际 Git 镜像做五轮复核后决定是否保留，不依据阶段计时单独发布。

### 第三轮 A 验收完成：连续候选区间

实际 Git 镜像完整 gate、150k/128 MiB 容量、两类旧格式迁移、ASan/UBSan 模型/Exact/内核/向量 WAL 故障均通过。详见 CANDIDATES_20261115.md 与 results/candidates_20261115；失败的 bitmap 实验也已保留原始日志和可重建源码差异。五轮默认平均请求 9884dd3 0.276785→0.277005 ms、五段 1.018227→1.021476 s，基本持平；8000 候选 0.554229→0.527048 ms，约改善 4.9%，1000 候选改善 3.2%，小候选略慢。保留这一有限的范围查询优化，未宣称默认吞吐大幅提高。

同轮累计相对 89990a8：写入 0.924890→0.628649 s，查询平均 0.323835→0.277005 ms，五段 1.373383→1.021476 s；相对原始 2k 版五段 1.250846→1.021476 s。WAL 额外写入仍存在、设备 io.stat 不可用，六指标已明确区分。CPP SHA256 0236762f1ae263143ab51ca56a3fbbbde3a9bf5ebe6201e6c499898b8b947382，镜像二进制 c72cf54105b2b6963a51263c5db193056780f29bc447a6c8cc40af6b2cf2d428。提交并推送后继续独立几何 Block 实验；官方提测仍由用户执行。

### 第三轮 B 与第四轮完成：按测量选择，不把几何实验放进默认

正式运行时提交 `5d91391f5da3d632e58849d219c99df6359da078` 已通过恢复后的本地代理推送，远端分支 `2026-competition-round2-lite2000` 已核对一致。曾遇直连/代理连接失败，用户恢复代理后成功；没有触发官方提测。

几何实验详情见 GEOMETRY_EXPERIMENT_20261115.md，证据在 results/geometry_20261115。有限元信息复用已有物理位置/主键/版本，参考半径使用向外舍入区间，按严格平方下界排除；更新、回滚、向量列切换、GC、重启与 mmap 跨窗均核对。7500 数值检查、两种候选门槛各 490 SQL 查询、三种 ISA 各 864 查询，以及 ASan/UBSan 490 查询均通过。

三轮同二进制 scan/物理 block/geo 的平均请求 ms：随机 128D 为 0.281738/0.383298/0.480104，聚簇 128D 为 0.568673/0.620290/0.470503，聚簇 3D 为 0.401279/0.461617/0.415466。无过滤计算数在聚簇 128D 由 10000 降为约 573，3D 约 250，但随机 128D 仍算全部 10000。首次几何构建分别约 55.664/47.437/2.572 ms。默认随机 1000 查询完整流程 scan→geo 1.025335→1.311685 s；聚簇 128D 的 300 查询为 0.884823→0.903156 s，3D 为 0.611892→0.635177 s。

结论：本轮均未得到包含构建的完整流程收益；保留可复现生成器与实验，不合入生产 src，不开启默认几何路径。不能把“少算 94%/97% 向量”宣传为端到端加速，也不能据 300 次查询外推长期聚簇负载永远无效。小候选确实在索引构建前回退，诊断构建/下界/排序为零。

低维首次失败发生在冻结基线：测试驱动按维度猜表结构，预热引用了不存在列。已按实际 columns 修正，失败记录保留，新目录重跑通过；原始 128D 输入的预热不变。运行时未因此改动。

第四轮使用实际 Git 对象镜像完成。六指标、分组均值/P95/QPS、所有候选/完整距离/维度计数、第一次缓存准备、分文件写入、同步和复制计数、资源限制及故障恢复均有记录。既有旧 WAL 没有原向量的限制通过完整向量 WAL 格式解决；旧格式仍不能取消向量同步。所有自动/手工 Manifest 发布路径继续受向量持久边界保护，未只处理 checkpoint。

本轮四阶段完成，正式运行时保持 1798 行。随后只提交实验记录和测试驱动修正；生产源码、Dockerfile、二进制对应关系与已验收 5d91391 保持一致。

### 持续迭代队列（未声称已解决）

- [ ] 用户用本轮可提测提交取得官方总分反馈；不推测 2000/5000 换算关系，不自动消耗提交次数。
- [ ] 完整向量 WAL 仍增加累计写入；只有同等恢复语义下证明综合收益，才继续减少重复字节，不能回退到缺向量 WAL 后删除同步。
- [ ] 大范围且物理顺序打乱时，候选准备仍可能超过距离成本；先由真实负载确认优先级，避免把小规模等值过滤拖慢。
- [ ] 当前 rootless 环境实际设备 io.stat 缺失，记录为不可用；若后续具备该计数器，再核对设备写放大，不能用进程记账冒充。

### 新实验：10 万 / 30 万混合维度，三版真实入口

用户要求新版与两份旧版对照：24d4262、原提测提交 f885c28、旧 C++ 分支 449bba1。每个数据规模按总行数一半 128D、一半 1024D，存入同一进程的两张表；固定 B100，交替写入和查询，1000 Exact/K10 查询，维度各半，每维度有/无等值过滤各半。2 CPU/2 GiB/no swap、CPU 0/1，三轮轮换顺序；主计时与同步/内部诊断分开。独立原始 float32 真值，重启全行/向量核对。失败保留阶段、stderr 和 cgroup OOM 证据，不把失败当速度收益。

**入口审计：f885c28 的实际 Dockerfile 编译根目录 minivectordb.cpp；不是 competition_lite/src/minivectordb_lite.cpp。** 本轮按每个 Git 提交真实 Dockerfile 构建和提取二进制，再使用同 GCC 12.2.0/相同参数/运行环境对照。根目录 f885 CPP SHA256 26f4f1a210ee12c178f895bc0e9181b467251b82a5e9b6122189f728c7600d8a；449bba CPP SHA256 6aec2f7e12cfc418721127afc0b44ed403bff6b6f35543887b2aa10c22555e97，并链接 v2/v2_storage.cpp。

审计过程中的一次错误判断已撤回并向用户更正：最初据源码目录推断历史“2k”基线用错，随后逐份核对服务器、LSM、Exact、候选路径报告的 19 份旧版原始记录，二进制 SHA256 全部为 4485f38cd7869e62b1cde92330972b3deddbf996005cd941a6907144673e39b5，与本次实际 Docker 入口完全相同。因此原服务器旧基线没有标错，不能因本轮大规模变慢而否定此前小规模测量。早期 Windows 的内部 Lite 对照本来就单独注明了源码路径；不可与服务器原始记录混为一谈。

服务器隔离目录 /mnt/SSD1/zzp/seekdb-round2-lite2000/mixed-100k-300k-20261115，未改动旧仓库。数据生成是固定种子的合成正态 float32，不冒充隐藏测试；普通字段相同，20 均衡类别。预热独立于正式查询，避免旧版缓存命中影响比较。阶段 I/O 在请求边界采集，不用外部强制同步替代数据库提交。实际设备 io.stat 不可用则继续明确缺失。

- [x] 按真实入口冻结三版镜像/二进制，混合双表协议和真值冒烟。
- [x] 10 万、30 万输入与独立真值生成；各三轮端到端/资源对照。
- [x] 另跑同步和内部阶段诊断，以分组访问检验混合访问假设。
- [x] 整理分维度/过滤、写入、峰值、I/O、同步、内部阶段和失败原因；报告与本次提交一起推送，不触发官方提测。

### 混合维度对照完成：发现大表读取退化

正式计时 18/18 轮通过，18000 条 Exact 查询与独立真值一致，重启全量核验累计 3600000 行字段和 float32 向量字节。6 轮同步诊断和 6 轮内部阶段诊断通过。277 份原始压缩记录按解压内容 SHA256 核对；详细结果见 [三版混合维度报告](MIXED_SCALE_20261115.md) 与 results/mixed_20261115。生产源码、Dockerfile 和接口不变。

三轮中位数，10 万 / 30 万：新版写入 17.613 / 55.067 s，原 2k 版 23.773 / 72.021 s；新版 1000 查询 126.059 / 262.221 s，原 2k 版 8.298 / 28.129 s，旧 C++ 12.295 / 32.799 s。新版容器峰值 262.5 / 791.0 MiB，两个旧版在 30 万时都达到 2 GiB 限制但仍完成全部核验，不能描述为 OOM 失败。新版写入及内存收益不足以抵消本负载的查询退化。

根因证据：两张表都超过默认 32 MiB 热行预算，所有正式查询走 LSM；不是每次切表重建矩阵。过滤索引输出主键后逐条 row_get，每次 Store::get 创建新 DiskCursor、读取校验 SST 索引并重复解码块。10 万行 128D 过滤每条查询只有 2500 个完整距离，却创建约 6598 个游标、请求 216.35 MB 块数据、解码约 130 万次内部记录；无过滤的 50000 个距离仅请求 5.08 MB 块数据。分表连续访问仍慢，排除热表切换作为本轮主要原因。

完整向量 WAL 的同步优化仍有效：写入阶段向量同步仅 14 / 44 次（发布屏障），WAL fdatasync 为 1000 / 3000 次，未退回每次提交两次同步。新版进程 write_bytes 为 542.09 / 1855.12 MB，旧 2k 版为 306.09 / 1119.29 MB；此处只统计写入阶段，关闭/checkpoint 另计。设备 I/O 计数不可用，不能把进程记账或逻辑块读取当作设备流量。

下一轮只推进已定位的 LSM 读取瓶颈，先做有界 SST 文件/索引/块复用和二级候选批量主键访问，再用相同数据验证；不要默认靠扩大热表预算或回退全量内存表解决。保留版本可见性、过滤、Exact 排序、CRC 损坏检测和旧文件引用保护。完成后重新执行这组 10 万/30 万对照及小规模回归，报告全部六项指标与完整流程，再决定保留。

- [x] 固定内存预算下复用 SST 文件与索引，避免逐候选重复打开与校验。
- [x] 二级候选按 SST/块批量读取，减少重复块读取和记录解码；与无过滤顺序读取对照。
- [x] 修改后同输入三轮复测、独立真值及重启全量核验；补充读缓存失效、compaction 引用保护与损坏检测验收。


### 2026-11-15/16：按用户文档推进大表 LSM 读取修复

冻结对照仍是 312f7e0 报告：10 万原 2k 写入/查询 23.773/8.298 s，旧 C++ 26.042/12.295 s；30 万分别 72.021/28.129 s 与 77.358/32.799 s。先验收 10 万；新版写入与查询均优于两份旧版后才进入 30 万，不能降低正确性、改变 B100 或复用查询答案达到门槛。六项指标继续全部记录，实际设备 I/O 缺失保持明确。

- [x] 第一项：SST TableReader/索引/已验证且已解码块复用，缓存预算与关闭开关，扫描过滤对照；CRC 和旧文件引用测试后独立提交。
- [x] 第二项：有序批量回表，合并各层/事务版本与 tombstone；按块访问代价回退主行扫描，补充跨 SST 版本、回滚、删除和合并验收后独立提交。
- [x] 第三项：紧凑向量元数据视图与最终投影延迟读取，独立预算，超过预算回到批量/扫描路径；更新、失败语句、回滚及 GC 失效检查。
- [x] 真实 Docker 入口、公开题包和默认 1 万回归；10 万固定输入三轮对照，达标后才运行 30 万三轮；故障、容量和资源统计交付。

现有冻结题包 protocol_check/README.md 仍明确要求 JSON ready/request/response，且不包含隐藏评分/平台接口。本轮按真实题包做验收，不根据外部大纲文字改变协议或声称可计算官方 E。参考文档中的缓存/MultiGet 机制已核对官方文档，代码自行实现、不接入其他存储引擎。WAL、Manifest 格式与提交屏障不变。


第一项开发验收通过：源码编译为 1840 行，TableReader 与已解码 Block 按文件身份和块编号复用，默认 16 MiB 保留缓存；`LITE_READ_CACHE_MB=0` 关闭保留，`LITE_LSM_READ=scan` 强制主行扫描过滤。缓存保留量计入索引、FD 估算、解码记录/字符串和容器节点开销；正在使用的游标工作集通过 live/peak 另行计数，不将其隐藏在缓存预算之外。退休时清除缓存引用，正在扫描的读取器继续保护旧 SST，最后有效引用释放后才删除。`--check` 绕过热缓存强制核验磁盘内容。

第一项专项检查：250 个点查的 Reader 初始化与块解码均少于 10 次；缓存预算、跨合并引用、扫描结果等价、热缓存后的磁盘破坏和重新加载 CRC 全部通过。既有冷模型 180、Exact 1470、损坏 8、LSM 发布/故障 42 通过。首次测试驱动把 storage 二进制和结果目录命名相同导致 FileExistsError，修正结果路径后剩余检查通过，原失败日志保留。这一提交是已验收的读缓存阶段，不宣称已达到两份旧版的时间门槛，继续实施批量读取与紧凑视图。


第二项开发验收通过：候选按主键排序/去重后，每批最多 1024 个键，由各 SST 的单个 SelectedCursor 持续定位，共享 Reader/已解码块；各层记录、tombstone 与事务 Overlay 仍由原 Merge 的版本规则选择。高覆盖且块内候选密集时在该 SST 内持续推进；`LITE_LSM_READ=batch/scan/auto` 保留对照。候选结果与持久二级键数量不一致仍报 corruption，全部剩余条件在距离计算前验证。

在保留缓存完全关闭时，专项批量检查确认每批 Reader 数不超过涉及的文件数，覆盖重复/缺失键、跨层删除、私有向量更新、回滚、Compaction、GC；scan/batch/auto 结果一致。冷模型 180、Exact 1470、损坏 8、LSM 故障 42 及第一项缓存专项再次通过。当前运行时 1864 行；紧凑查询视图尚未完成，未进入 30 万测评。


第三项开发实验：紧凑视图只保留主键/向量偏移，并在统一 8 MiB 预算内缓存持久二级索引的候选位置；不是查询答案缓存。剩余谓词无法由这些元数据验证时回退批量/扫描；输出字段只对最终结果取完整行，并保留 64 MiB 结果物化上限。范围 MemTable 游标也改成只复制所需区间，避免每次过滤复制全 MemTable。更新、删除、失败语句、回滚、向量列切换和 GC 都失效或重建视图。

phase3 首轮 10 万：写入 14.827 s、查询 10.499 s、主流程 25.662 s，全部 1000 查询及 10 万行重启核验通过；仍未达到旧 2k 查询 8.298 s，未运行 30 万。随后在相同已恢复数据副本上仅对照映射窗口，32 MiB / 1024 MiB 查询为 10.265 / 7.175 s，均通过 1000 条真值。该对照只用于定位映射反复切换，既不代替完整装载流程，也不使用由宿主复制文件产生的 cgroup 页缓存记账比较内存。

据此新增有界向量映射保留，保留原 32 MiB 窗口，默认最多 1 GiB 虚拟映射；不复制一份全量向量，不锁定物理页，实际驻留仍计入容器内存。`LITE_MMAP_KEEP_MB=0` 保留原单窗口行为，低预算测试覆盖跨窗口、增长、回滚和 GC。所有映射在截断和向量代际切换前失效。需要用正式全流程的容器峰值评估这项代价，不能把虚拟映射当作零内存。

phase3b 开发查询初步为 6.812 s、写入 14.949 s，四组查询平均均低于两份旧版对应的历史中位数；仍待正式 Git 镜像和三轮验收，暂不把单轮结果视为最终收益。最终代码另明确强制 scan/batch 会绕过紧凑视图，便于单独验证底层读取。

正式 Git 源码镜像 gate 通过，运行时 1899 行。10 万三轮查询为 6.523648/6.513464/6.449619 s，写入中位数 14.819592 s，主流程中位数 21.570877 s，容器峰值 276.836 MiB。全部 3000 条真值和累计 30 万行重启全量核验通过，写入/查询/主流程/峰值均优于冻结的两份旧版，之后才启动 30 万。

默认 1 万三轮相邻对照：查询中位数基线 0.323779 s、新版 0.332509 s（约 +2.7%，区间重叠），写入 0.626951→0.625663 s，峰值 23.34→24.33 MiB。没有把它描述为小规模提速；查询变化尚不足以证明稳定退化，额外约 1 MiB 的内存代价单列。

30 万前两轮写入/查询 46.388/18.958 s 与 45.595/19.924 s，均通过全部真值与 30 万行恢复核验。发现计时外的旧核验器按无上界主键范围取 128 行，导致通用排序反复扫描其余记录；只给后续验证请求增加对应 128 行的主键上界，仍核验相同的全部字段和原始向量字节。装载、预热、正式查询、真值和资源限制未改变，旧版/新版核验器源码及哈希保存，不能把核验提速当作数据库性能收益。首两轮已运行的进程继续旧核验器，第三轮及后续调用使用有标识的 v2。

### 大表读取任务完成：正式三轮、底层对照和完整验收

30 万第三轮写入/查询为 46.350/18.880 s，全部通过；三轮中位数写入 46.350292 s、查询 18.958463 s、主流程 66.088300 s、容器峰值 800.75 MiB。两种规模的写入、查询、主流程、内存均优于两份冻结旧版，四种维度/过滤组的均值和 P95 也均更低。正式六轮累计 6000 条独立 Exact、120 万行重启全量核验；没有运行失败后择优删除结果。

9 组独立诊断均通过，每组显式关闭完整热行缓存。默认紧凑视图占用 2.16/5.84 MiB；关闭紧凑视图的 10 万 128D 过滤，SST 游标 6598→12、块读取约 6601.5→160、内部记录解码 1300881→50766，完整距离仍是 2500。将读取缓存也设为零时，仍只初始化 12 个 Reader/游标，解码 53193 条内部记录，证明底层批量读确实生效。32 MiB 单窗口每查询新建约 7–10 个 mmap，默认保留窗口在预热后为零。缓存 1 MiB 与 16 MiB 在裸批量路径本轮差异不大，未把 16 MiB 宣称为最优预算。

持久化语义未改：写期 WAL fdatasync 为 1000/3000 次，向量屏障同步 14/44 次。进程 write_bytes 为 542.085/1855.128 MB，仍高于原 2k 版；实际设备 I/O 依然缺失。默认 1 万查询本轮中位数 +2.7%，未超过事先设定的 +5% 回退门槛，但不声称小规模收益，约 1 MiB 额外内存也已列出。

公开 20、额外 12、Exact 1470、内核路径 864、冷模型 180、损坏 8、LSM 故障 42、向量 WAL 故障 72，以及提交故障、引用保护均通过。150k/128 MiB/no swap 容量和两种旧格式迁移通过；四项新专项与完整模型/Exact 的 ASan/UBSan 通过。450 份原始归档按解压字节 SHA256 核对，生产源码与正式镜像一致，二进制 SHA256 为 86d8a27867fd8583e9d5f8e9bdcb6169e9d18a809fc5bf551f943f494a0cc1e2，运行时 1899 行。完整六指标见 READPATH_20261115.md 和 results/readpath_20261115。未触发官方提测。

后续独立任务：设备 I/O 具备后再核对写放大；评估扫描对块缓存的污染及预算，不能据单轮诊断改默认；通用主键 ORDER/LIMIT 可研究提前停止。本轮不再改距离内核、WAL 或 Compaction 策略。

### 2026-11-16：固定后续主要性能基准

按用户最新要求，后续优先评价 10000 行、128 维、1000 次 Exact Top-K、K=10，其中无过滤/等值过滤各 500 次。沿用固定原始输入、20 个均衡类别（过滤后 500 候选）、B100/100 次持久提交、2 CPU/2 GiB/no swap。完整请求与查询区间分别计时；预热/索引构建/checkpoint 单列并计入完整流程。继续报告六组指标与正确性，不从本地时间推算官方分数。10 万/30 万保留为容量和退化回归，不以大表收益代替默认负载收益。

- [x] 当前 9a911a1、原 2k f885c28、旧 C++ 449bba1、大表修复前 24d4262 在同一服务器相邻五轮轮换顺序对照；使用已验收真实 Docker 二进制并复核哈希。
- [x] 全部查询数量/去重/过滤/距离/独立真值和重启全量检查通过；四版另跑单独诊断，结果保留在服务器隔离目录。后续打包归档被自动审批拒绝，改为只读已有结果，不继续归档。

用户追加要求：**一轮出结果即可，后续默认先做一轮，不自动安排三至五轮。** 此要求到达前本轮五轮已完成；主报告固定展示四版第一轮并如实披露既有中位数与波动，不再追加测量。详见 [默认负载报告](DEFAULT_20261116.md)。

第一轮当前写入/1000 查询为 0.849829/0.618635 s；原 2k 为 0.954291/0.692333 s，旧 C++ 为 1.010545/1.266559 s，修复前为 0.778838/0.558011 s。当前优于两个旧版，但没有证明最近的大表优化改善小负载；进程写期 write_bytes 仍为 14528512，原 2k 为 5922816。后续优化以默认基准为主，并分别看无过滤/过滤；大表为容量和退化回归。生产代码未变。

### 小表优化第一项：热表等值候选直接复用槽位

按用户提供的小表 Exact 路线，先审计再实现。确认连续矩阵与 128D 查询预转换已存在，无过滤路径已绕过 SST，写入时不建矩阵。当前等值查询仍在每次查找主键对应位置、收集并检查槽位；原诊断过滤准备约 0.0774 ms，向量访问/距离约 0.0409 ms。因此先单独削减候选准备，不改持久化、精度或距离内核。

- [x] 热表单等值条件使用有预算、按槽位顺序构建的倒排；复用时不复制、不逐条查主键、不排序。不缓存查询答案。复杂谓词与 Block 保留原路径。
- [x] 真实 Docker 全 gate、新专项 182 组和两种子 216 条独立真值通过；修改/私有事务/回滚/失败语句/GC/表列切换、空候选、预算拒绝和不重复构建均覆盖。
- [x] 隔离新旧/关闭开关各一轮；首轮整体未改善且出现漂移，针对异常追加交替请求核对。首次交替顺序与过滤组相关，记录并修正后重新核对：平均请求 0.613482→0.596838 ms，过滤 -7.03%，无过滤持平。全部结果保留，未把首轮 +5.7% 的查询区间宣称成提速。

第一项保留，详见 [小表候选报告](SMALL_HOT_20261116.md)。有界倒排占 84697 字节，预热后构建为零；本轮不改向量计算数量、精度、持久化。实际被测 CPP SHA256 92c1fdd1dfd3ed8b9fbd8f9460103ac0c3fd431f3776f3821b3f918862743949，运行时 1925 行。下一项单独考察 strtod 数值转换：现有写入已每向量解析一次，问题是转换成本，不能声称再次消除了重复解析。

### 小表优化第二项：兼容原语义的向量数字解析

第一项已提交为 2373bf1。第二项只更改向量文本转 double 的常规十进制路径，使用 C++ from_chars；前导加号、范围和下溢走原 strtod 回退，保留全部字符/维数/有限值校验和 double→float32 转换，不降低距离累积精度。LITE_FAST_PARSE=0 用于关闭对照。不改 JSON 协议、WAL、事务提交或 flush 策略。

- [x] 3909 组专项、384877 个有效 float32 值逐位一致；浮点舍入边界、极小值、下溢、范围、无穷/NaN、分隔符、前导加号与非法输入通过。
- [x] 真实 Docker 全 gate、默认四版本各一轮、两个种子 216 条真值通过；完整主查询 4000 条与重启 40000 行均通过。写入/查询/内存/I/O、解析和同步诊断记录在 SMALL_PARSE_20261116.md。

第二项保留：对同轮重新运行的原 9a911a1，写入 0.807541→0.657343 s（-18.60%），完整流程 1.484572→1.339329 s（-9.78%）；查询 0.588329→0.596789 s（+1.44%），没有宣称查询端到端提速。隔离诊断的向量解析 219.616→72.444 ms，而 WAL 仍同步 100 次、自动发布向量屏障 1 次，逻辑及进程写入字节完全相同。两组 Recall@10 均 1.000。CPP SHA256 43afe45407cb542ec1d3bc34edb7edc9e36c83abbce91f14548e911a585d11be，运行时 1930 行。

第一项 2373bf172eab170445e10b7f51e09fef45258187 已推送并由 ls-remote 核对；首次服务器转发推送超时，改用已恢复的本机 127.0.0.1:7897 代理完成。后续仍按一项改进/验收/推送推进。当前最贵的无过滤距离阶段和 JSON/结果固定开销保留为后续重点；不据模块存在与否继续增加功能。

### 查询专项：输出缓冲与结果物化

用户要求本轮明确优化查询速度。以 de0eed2 为基线，诊断显示每请求 JSON 编码约 50 us、结果物化约 20 us；代码存在每个 double 新建 ostringstream，以及行数组、整份 rows/metrics 的深拷贝。先处理这一公共成本，不改距离候选或精度策略。

- [x] 单缓冲递归 JSON 输出、17 位 double 转换和原类型/转义规则，保留 LITE_JSON_FAST=0 对照；热路径最终结果使用移动并预留所需容量。
- [x] 99954 个随机有限 double 逐位往返且文本一致，极值/负零/次正规数/非有限拒绝、嵌套类型/转义通过；实际 Docker 完整 gate、两个种子 216 条 Exact 通过。JSON 专项接入后续 gate。
- [x] 固定默认负载基线/新版本/关闭编码快路径各一轮，全部 3000 查询与 30000 行重启核验通过；无过滤/过滤 Recall 均 1.000。

本轮保留：同轮 de0eed2→新版查询 0.318973→0.301865 s（-5.36%），无过滤/等值过滤平均请求 -5.27%/-5.85%，写入 0.525713→0.523706 s 基本持平，容器峰值 24.89→24.44 MiB。写入 bytes 与同步次数相同。普通响应独立诊断的编码从 14–15 us 降到 4–5 us；历史约 50 us 受 profile 字段和时段影响，不据此推算正式收益。没有用上一份报告更慢的绝对时间计算加速。

详见 [查询输出报告](QUERY_OUTPUT_20261116.md)，源 CPP SHA256 79d5a9ef08e47c4685c172b9e43ff82a09f05b42aa3e5b33ca3671c184540449，运行时 1949 行。无过滤距离仍是最贵阶段，本轮未修改它；后续继续一项改进、一次主对照、通过后推送。

### 查询大幅提速目标：安全筛选与原精度复核

用户要求争取约 2 倍查询提速，以已推送 05212db 为基线。先验证 128D 小表连续矩阵上的两阶段距离：float SIMD 扫描只产生保守下界，不能直接进入 Top-K；可能入选及边界不确定的候选均用原 double 内核计算。候选可见性、过滤、真实主键 tie、最终距离和持久性保持。溢出/非有限筛选值、非默认浮点环境或不适用执行路径回退原流程。

- [x] 推导相对/绝对误差保护；最终使用 int8 编码的三角误差界，921088 个阈值判定对照原 double 和独立 long double 通过。近距离、宽指数、裁剪、并列、极值和次正规数有专项。
- [x] float 初版读取量未降且无明显收益，弃用；依次验证按行 int8、16 向量交错 VNNI、四累加链。保留开关、原 double 复核、完整计算量及缓存统计。各开发实现一轮主对照，全部中间记录保留。
- [x] 最终真实 Docker 完整 gate、恢复故障、四种回退路径、ASan/UBSan 通过；实际 Docker 二进制与计时产物哈希相同。最终三版共 3000 查询、30000 行重启核验通过，两组 Recall 1.000。

最终以 05212db 为基线：无过滤平均请求 0.337945→0.179822 ms（1.88 倍），混合平均 0.242674→0.161932 ms（1.50 倍），查询区间 0.291981→0.209050 s（1.40 倍），没有达到混合端到端 2 倍。查询 CPU 减少 51.29%，全流程 0.889638→0.814749 s（-8.42%）；写入持平。额外缓存 2600000 bytes，首次预热增加约 9.9 ms，峰值 25.32→27.32 MiB；代价没有排除出计时与资源统计。WAL 100 次同步、向量发布屏障 1 次及写入字节均保持。

极小值专项初始使用坐标置换导致既有 double 与独立 fsum 的并列顺序不一致，新旧路径本身一致；改用可无歧义验真的极值构造并保留失败日志。最终运行时 1995 行，CPP SHA256 f56625874509a27b8b4aa7aae0975d8e70077ee006b424f0dcaf708faaeac771。完整交付见 [查询大幅优化报告](DISTANCE_SCREEN_20261116.md) 与 [边界推导](DISTANCE_SCREEN_BOUND.md)。

### 2026-11-16：按综合 T/B/M 持续优化

用户提供 E=(E写+2E查)/3、T/B/M=60%/20%/20%、R 经 R/(1+R) 平滑和三轮中位数规则。以已推送 830fbba 为本轮基线，优先保留耗时、I/O、内存的综合改进，不承诺未实测的 3/5/10 倍。开发候选各一轮筛选，最终有收益的版本按新给定规则保留三轮独立结果；官方基线未知，不能把本地版本对照冒充官方 E/排行榜分。

服务器只读核对：根 cgroup 有 io，但 user.slice 和 user@1002.service 只获得 cpu/memory/pids，当前账号无免密 sudo。已询问用户是否能请管理员补齐委派；不修改全机设置，不把进程 I/O 当成物理 B。

- [x] 第一项：连续物理向量复用 mmap，默认不再复制 5120000 bytes；非连续/失败映射/Block 回退，保留 LITE_MATRIX_VIEW=0。
- [x] 映射生命周期专项、实际 Docker 完整 gate、恢复故障、四种回退、ASan/UBSan 通过。初始首行 UPDATE 的布局预期错误已修正测试，保留失败记录。
- [x] 根据诊断选择 JSON 输入解析，独立实现与对照；逐轮保存数据，没有拼接不同轮次最好值。

管理员已为 zzp 启用 io 委派，容器实际有 io.stat，补齐后三轮峰值 26.50→22.21 MiB（-16.2%），查询区间 0.212766→0.207718 s，设备写入量持平。本地参考 E 中位数 0.508999（旧版自比 0.5），不是官方分。io 启用前三轮查询反而慢 6.7%，同样保留；主要可信收益是内存。下一候选是写期累计 39.09 ms 的 JSON 字符串解析。详见 [完整报告](COMPOSITE_VIEW_20261116.md)。

第一项已独立提交并核对远端：10bc5751c139bc0b7620a5f7cc97bc3ec9950527。

### 第二项：JSON 输入字符串扫描

只修改 jstr 的普通字节扫描，SSE2 每组检查 16 bytes 的引号、反斜杠和小于 32 的控制字符；非特殊连续片段整段追加，转义、Unicode/surrogate 与错误处理保持旧流程。LITE_JSON_SCAN=0 为关闭对照。运行时只收紧两个未改语义的便捷函数排版以满足 2000 行限制，没有生成运行时代码。

- [x] 47724 项对照旧解析器的字节、错误码、错误消息和停止位置测试，包括所有字节在不同偏移、转义、surrogate、短尾和随机内容。专项微测普通 1500/200000 byte 字符串加速明显，但不当作整个请求的倍数。
- [x] 开发三路径一轮：原版、开启、关闭均通过查询和恢复。写入 0.5988/0.5886/0.5325 s 存在较大波动，不能用这一轮证明端到端改进。
- [x] 实际 Docker 全 gate、47724 项 ASan/UBSan、独立诊断及最终三轮 T/B/M 完成。三版 9000 查询/90000 行重启均通过，两组 Recall 1.000，实际 Docker 与计时 binary 一致。

最终相对 830fbba：查询区间 -3.94%、混合平均请求 -5.17%、峰值内存 -17.21%、物理 I/O 持平，写入无明显改善。相对父版 10bc575 的本地参考 E 中位数 0.502242（旧版自比 0.5），第三轮 0.498862 略差也保留；相对 830fbba 为 0.509112。写期 JSON 解析 41.173→3.879 ms，但同步 227.033→252.727 ms 的诊断波动抵消部分收益，不能声称整体十倍。保留有小幅收益的解析优化；完整报告 [JSON_SCAN_20261116.md](JSON_SCAN_20261116.md)。

本轮没有证明整体 3/5/10 倍。后续应继续针对剩余距离扫描、请求固定成本和同步/写放大做独立实验；不能通过少返回结果、降低 Exact 精度或取消持久化来达到倍数目标。每项只合入通过实际入口验收且有收益证据的版本。
