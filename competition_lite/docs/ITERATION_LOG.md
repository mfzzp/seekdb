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
