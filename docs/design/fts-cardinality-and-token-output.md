# Cardinality-only FTS limits and direct token-array output

Control commit: `2ca918f879e6bf7eb8f2c01a778ffbedfc616f21`.
This revision retains the resource-aware index-build workers and short-list
membership merge. It does not change construction parallelism, benchmark data,
benchmark SQL, timing, scoring, baselines or CI workflows.

## Limited views whose rows are not observable

For example, `SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST('search terms') LIMIT 20 OFFSET 5) v` observes only the number of rows in the limited view. The previous implementation inserted an implicit relevance order and evaluated scores and unused projected columns before counting.

`is_fts_cardinality_only_view()` proves a deliberately narrow case: an outer
SELECT containing exactly COUNT(*), over one direct derived table, with no
outer row-dependent filter, grouping, DISTINCT, window, ordering, subquery,
assignment or locking. The inner SELECT must have one base table, LIMIT,
plain column/constant projections and one natural-language MATCH. Explicit
ordering, grouping, DISTINCT, windows, WITH TIES, assignments, subqueries and
locking exclude it. The decision contains no table names, parser names, term
values or particular LIMIT values.

Preprocessing replaces unused projections with a constant and omits the
implicit relevance order. The existing optimizer's additional membership-only
proof still governs score elimination: covering access, supported boolean
MATCH filter, no boosts or score use, no adaptive/vector scan. General score
thresholds and expressions cannot accidentally become membership predicates.
Existing posting-list iteration, visibility checks, row deduplication and
LIMIT/OFFSET termination remain responsible for the result.

Bare limited searches still return the same ranked rows. COUNT(column), outer
filters, explicit ordering, grouped/distinct views and projected relevance
retain their normal paths. This is not a general ranked Top-K shortcut and
does not introduce WAND or change relevance arithmetic.

## DEFAULT TOKENIZE output

`make_token_array_binary()` serializes the flat array directly with existing
ObJsonBin metadata, value-entry and string primitives. It preserves word-map
iteration order, document headers, string bytes and the generic serializer's
length/offset-width selection, including its width-boundary retry. The result
is handed directly to `pack_json_str_res()`; it is not wrapped and parsed into
another JSON object first. Size and allocation errors propagate.

The original tree builder remains available. ALL mode still uses its original
tree path. Parsing, dictionaries, normalization, token membership, frequencies
and original JSON output semantics do not change.

## Verification on 2026-11-08

Windows LLVM 21 incremental compilation/link succeeded; the new shared header
also compiled as a standalone include. Modified source hashes match the build
overlay. Both dedicated instances bootstrapped and returned SELECT 1. They
share an existing native development build, so this is not a clean Linux build.

- New COUNT/LIMIT integration suite: 5 tests passed, including 48 limit/offset
  cases, explicit-PK/heap tables, missing/empty search, prepared reuse,
  transaction delete/reinsert/rollback, EXPLAIN eligibility and ranked fallbacks.
- Native JSON differential test: 148 arrays matched the original generic
  serializer byte for byte; every decoded string also matched. Cases cover
  empty arrays, embedded NUL, escaping, UTF-8 and 8/16/32-bit size boundaries.
- Original TOKENIZE test: 56 statements had identical results/error codes;
  56 extra space/BENG/IK/ngram DEFAULT/ALL cases also matched. Long generated
  SQL expressions use a fixed result-column alias to avoid truncation of a
  multibyte expression-derived field name; output assertions are unchanged.
- Four existing SQL regressions passed on both versions (57 assertions each),
  as did 200 additional parser/query differential cases.
- Existing short-list integration suite: 5 tests passed. Existing concurrent
  DDL suite: 2 tests passed, including transactions and a failed unique index.
- The original 20,000-document queries matched all IDs and numeric scores
  exactly, and the raw limited result matched exactly. Hit counts remain
  8001/11000/7332/20; verification does not rely on counts alone.

Run the integration checks against a dedicated instance, with PyMySQL installed:

`SEEKDB_TEST_PORT=2881 python3 unittest/sql/engine/test_fts_count_limit.py`

`SEEKDB_TEST_PORT=2881 python3 unittest/sql/engine/test_fts_short_merge.py`

`SEEKDB_TEST_PORT=2881 python3 unittest/rootserver/test_ddl_build_concurrency.py`

These tests create and drop uniquely named databases. Optional variables are
SEEKDB_TEST_HOST, SEEKDB_TEST_USER and SEEKDB_TEST_PASSWORD. The native test
`unittest/sql/engine/test_fts_token_json.cpp` is a standalone main, built with
the engine's include/compile flags and seekdb link dependencies in place of
the observer main object; it is not a registered CMake test target. Native
compile, link and execution each exited 0. Linux compilation and the official
evaluation remain pending.

## Local measurements and limits

Six pairs alternate control/candidate order. Each uses 30 warmups, 3,000 calls
per tokenizer and 200 per query, with the original benchmark texts and SQL.
Both processes have affinity to the same four logical CPUs, cpu_count=4,
memory_budget=4G, INFO logging, 2G datafiles and 2G log disks. The local harness
uses persistent PyMySQL multi-statements, not the official Linux shell client.
No build or other benchmark ran concurrently with the final timing samples.

| Metric | Control ms | Candidate ms | Elapsed reduction |
| --- | ---: | ---: | ---: |
| COUNT over limited FTS view | 6.41161 | 0.52188 | 91.86% |
| IK TOKENIZE | 0.18315 | 0.17519 | 4.35% |
| BENG TOKENIZE | 0.11103 | 0.10616 | 4.39% |
| Chinese membership query | 2.58438 | 2.26175 | 12.48% |
| BENG membership query | 4.35443 | 3.75640 | 13.73% |
| Mixed membership query | 2.80855 | 2.66163 | 5.23% |
| SELECT 1 | 0.02903 | 0.03496 | -20.43% |

The limited-count improvement is large in all six pairs (process CPU reduced
92.37%). Tokenization gains are small and variable; CPU reductions are 1.40%
and 3.76%. The other three query paths are unchanged by this revision: their
timing differences are not claimed as new code gains. The slower SELECT 1 is
retained, and OS/database background variability remains. These numbers are
not a prediction of platform score or globally optimal performance.

Fresh original-data construction succeeded (IK-all 13.046 s, IK-content
9.472 s, BENG 5.294 s). These are smoke results, not a controlled construction
comparison: the earlier control run used unrestricted CPU affinity and final
setup overlapped other validation work. Construction code is unchanged.

## Statistics diagnosis and rejected intermediate version

Before final comparison, the control had a 20,000-row table-statistics record,
whereas the fresh candidate had no record. Numeric BM25 values differed despite
identical matching rows. Executing `ANALYZE TABLE fts_large_bench.docs` and
`ALTER SYSTEM FLUSH PLAN CACHE` on both dedicated instances aligned row counts
and made complete scored results byte-for-byte equal without a source change
or numerical tolerance. The existing code generator supplies estimated total
document count to BM25. The diagnostic retains before/after statistics and
full-result hashes. The control used SSTables while the candidate retained
memtable rows, a remaining limitation of this local performance comparison.
The official benchmark scripts were not changed to add these commands.

An intermediate direct builder still returned an ObJsonBin wrapper, causing
another conversion in result packing. In one pinned six-pair run it regressed
IK by 14.23% and BENG by 5.68%. That version is not shipped. Direct binary result
packing removed the redundant conversion; final results above are a separate
experiment, not averages pooled with rejected runs.

Raw final samples, exact-result hashes, statistics diagnostics, source hashes,
verification summaries and rejected-run summaries are preserved in
`fts-cardinality-token-local-results.json`.
