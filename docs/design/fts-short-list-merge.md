# Short posting-list membership merge

Control commit: `1b0ef73d7b04e38db86525097c8c2e07f29e8dad` (the user's
26.517/50 platform result). This change retains its resource-aware DDL workers.

## Change and eligibility

Covering, membership-only MATCH queries already avoid BM25 in the optimizer.
Their DAAT execution still pushed and popped one merge-heap item per posting,
read unused scores and rebuilt the heap between documents. For up to four
posting lists, `ObTextDaaTIter::do_one_merge_round()` now selects the smallest
current document ID directly. All lists sharing that ID advance together.

The fast path requires natural-language mode, no function lookup, no relevance
projection, no score filter and no pushed top-k. Larger and Boolean queries,
score thresholds, projected relevance and ranked LIMIT use the existing merge.
Four is a bounded algorithm-selection threshold, independent of parser, table,
query text, document IDs, result count and benchmark inputs.

The existing document-ID comparator handles both string and integer IDs.
The existing collector still checks minimum_should_match and resets per document;
one contribution is supplied per matching list. No visible score is generated
on this path. Existing batching, offset/limit and projection remain in use.
Only consumed lists advance; exhausted lists are cleared. Iterator and comparator
errors propagate. No data, match result or query answer is cached.

## Verification on 2026-11-08

Native Windows, LLVM 21, incremental build and link succeeded. Both changed
translation units (retrieval and its observer allocator caller) were rebuilt.
The final executable uses the control SQL library: the experimental SQL-side
word-map reuse is **not included**. All other libraries match the control.
These executables share an existing development build; this is not a clean
Linux compilation or an official evaluation.

Fresh dedicated instances: memory_budget=4G, cpu_count=4, INFO logging,
2G datafile, 2G log disk. Both bootstrapped and passed SELECT 1.

- Original 20,000-document data and four query definitions: counts
  8001/11000/7332/20 match; all matching primary keys and numeric relevance
  scores match exactly; the LIMIT result matches exactly.
- Five new integration tests pass. They use 1,106 rows and independent Python
  sets, exercise 1–6 terms, duplicates, disjoint/exhausted lists, empty/NULL
  input, integer/string document-ID tables, prepared reuse, transaction
  visibility, deletion and rollback. EXPLAIN verifies score-free eligibility
  and scored/ranked fallbacks. Full scored primary-key results are checked
  against the set oracle, in addition to membership counts.
- Four existing SQL regressions pass on both versions: fts_score_free_filter
  (24 assertions), fts_normalized_word_reuse (12), fts_word_map_regression (11),
  fts_ik_hotpath (10).
- Another 200 differential cases across space/BENG/IK/ngram pass. The numeric
  MATCH > 0 path is also used as a separate membership oracle.
- Both concurrent DDL integration tests pass: four parsers, ordinary indexes,
  commits/rollbacks, duplicate-key failure and subsequent successful DDL.

Run the added test against a dedicated instance with PyMySQL installed:
`SEEKDB_TEST_PORT=2881 python3 unittest/sql/engine/test_fts_short_merge.py`.
It creates and drops a unique test database. Optional variables are
SEEKDB_TEST_HOST, SEEKDB_TEST_USER and SEEKDB_TEST_PASSWORD.

## Timing

Six pairs, alternating control/candidate order; each metric has 30 warmups,
3,000 calls per tokenizer, 200 calls per query. SQL and text are read unchanged
from the original benchmark. Local timing uses a persistent PyMySQL connection
and multi-statements; it is not the official Linux shell harness. No compilation
or other benchmark ran concurrently with these samples. Background OS/database
work was not pinned or eliminated. Raw elapsed and process CPU samples are in
`fts-short-list-local-results.json`.

| Metric | Control ms | Candidate ms | Elapsed reduction |
| --- | ---: | ---: | ---: |
| Chinese query | 1.9920 | 1.5731 | 21.03% |
| BENG query | 2.9450 | 2.4094 | 18.19% |
| Mixed query | 2.3106 | 1.8799 | 18.64% |
| Ranked LIMIT | 3.6436 | 3.6257 | 0.49% |
| IK tokenize | 0.11553 | 0.10990 | 4.88% |
| BENG tokenize | 0.06349 | 0.05938 | 6.47% |
| SELECT 1 | 0.01812 | 0.01724 | 4.85% |

All six pairs improve each eligible query; process CPU reductions are
18.83%/18.06%/17.98%. LIMIT and tokenization do not use the new path, so their
differences are not attributed to the optimization. The result is evidence
for a query CPU improvement, not a prediction of platform scores.

One build smoke comparison: control 7.1854/5.6884/3.2642 seconds versus final
candidate 7.4080/5.6863/3.3651 for IK-all/IK-content/BENG. Construction code is
unchanged; no construction improvement is claimed, and the slower samples are
retained in the report. No benchmark data, SQL, timing, scoring or baseline file
was modified.

## Deferred experiment and earlier mismatch

An experimental thread-local word-map lease showed about 8% BENG build benefit
in three early pairs, but this did not reproduce in the next fresh pair and IK
did not improve reliably. It was removed before making the final candidate.

Before the pause, separate old instances had identical hit counts but slightly
different BM25 values, already present before the new merge. Existing code uses
estimated total document and token counts (ob_tsc_cg_service.cpp and
ob_text_retrieval_token_iter.cpp). Estimation is a possible explanation, not a
proven diagnosis. Fresh control/combined-experiment instances matched exactly;
fresh control/final-query-only instances also matched exactly. No tolerance or
relaxed assertion was used to pass the comparison. The old discrepancy remains
unattributed and is not represented as fixed by this patch.
