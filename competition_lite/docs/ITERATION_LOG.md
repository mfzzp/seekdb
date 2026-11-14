# Iteration log

## 2026-11-14 small optimization pass

- Baseline commit: bbb8412
- Kept: generic scalar equality postings, rollback rebuild, exact candidate narrowing, nth_element for ascending distance LIMIT K, and removal of -ffast-math/-march=native.
- Correctness: public harness 20 cases, 0 failures; scalar/update/rollback smoke passed; exact tie smoke passed.
- Small benchmark: 2000 rows, 1024 dimensions, batch 256, 16 queries, one run. Old query 0.01965 s; new query 0.01844 s. Old insert 0.23483 s; new insert 0.26579 s in that run.
- Physical-offset ordering was measured three times and reverted: with ordering mean 0.01844 s, without ordering mean 0.01786 s.
- Four-way accumulation was reverted because the small query result was unchanged.
- Large 100k/300k experiments remain deferred.
