# Score-free full-text membership filters

## Bottleneck and scope

The three full-text COUNT queries in the competition benchmark only consume
whether a document matches. At baseline commit
`272f628cb877c1293368c0a12998d94833f865fe`, their plans nevertheless have
`calc_relevance=true`: they estimate document frequency/length and evaluate
BM25 for posting-list entries, then discard the score after filtering.

The optimizer now recognizes a positive, natural-language MATCH predicate on
a single base table when the index covers the query. The MATCH must not be
used by any other expression. Queries with ORDER BY, LIMIT, subqueries,
Boolean mode, field boosts, score projections, numeric score predicates,
multiple MATCH predicates or index-back reads retain the scored path.
This is a semantic check; it does not depend on table names, COUNT specifically,
search text, data values or the benchmark.

Eligible scans union and deduplicate the same posting lists using the existing
DAAT merge, without allocating BM25 aggregation scans. A positive internal
membership contribution is never exposed as a user-visible relevance score.
The large-query TAAT fallback also handles absent score statistics and
duplicate document IDs. Empty/NULL/stopword queries remain empty.

Simply turning off BM25 was insufficient: the old no-projection branch chose
TAAT even for small queries and materialized all hits in a hash store. Local
measurements showed a substantial regression. The final implementation keeps
the normal streaming merge for queries within the existing DAAT token limit.

## Validation on 2026-11-08

Windows native builds used the same existing development build dependencies,
with the changed translation units rebuilt and the executable relinked using
Clang/LLVM 21. Both independent instances bootstrapped and passed SELECT 1.
This is not a clean Linux build or a platform evaluation.

- Added `fts_score_free_filter.test` and its expected result: membership union,
  repeated terms, ordinary predicates, numeric thresholds, projected scores,
  Boolean exclusion/intersection, LIMIT/OFFSET, prepared-statement reuse,
  insert/rollback, update/delete and a 300-term TAAT query.
- Existing hotpath, normalized-word, word-map and IK regressions passed.
- Existing TOKENIZE script: 56 statements, including expected errors.
- 220 TOKENIZE differential cases matched the baseline.
- 200 query cases across space, BENG, IK and ngram matched both the baseline
  and a scored `MATCH > 0` oracle, including 300-term queries.
- EXPLAIN showed `calc_relevance=false` for eligible membership queries;
  protected query shapes retained scoring or functional lookup.
- The unmodified 20,000-document generator and benchmark SQL produced the
  same counts (8001, 11000, 7332, 20), full document IDs, projected scores and
  ranked results as the baseline.

## Local performance comparison

Dedicated Windows instances, identical data/configuration, alternating version
order for six samples, 30 warmups, 200 executions per query per sample.
Timing used a persistent PyMySQL client and SQL multi-statements; it is a local
comparison, not the official mysql-client timing harness or a platform score.

| Query | Baseline mean ms | Candidate mean ms | Time reduction |
| --- | ---: | ---: | ---: |
| Chinese COUNT | 2.2651 | 1.8970 | 16.25% |
| English COUNT | 3.3833 | 2.8980 | 14.34% |
| Mixed COUNT | 2.6216 | 2.2962 | 12.41% |
| LIMIT | 3.5669 | 3.6658 | -2.77% |

All six paired samples improved for each COUNT query. LIMIT remains on the
scored path; its measured regression is included rather than discarded.
Unchanged TOKENIZE/SELECT 1 controls also varied, so these figures are not a
guarantee of the same improvement on Linux. This change makes no claim about
index-build speed or the earlier 5-second/8-second IK build variation.
The official benchmark, generator, SQL, scoring script and baseline are
unchanged. Platform compilation, regression and performance results are still
required to establish the submission's score.

On a dedicated Linux instance, use the existing mysqltest runner with
`fts_index.fts_score_free_filter`, then run the unmodified full benchmark with
`ROWS=20000`, `BATCH=500`, `ROUNDS=3000`, `QUERY_ROUNDS=200`, `SAMPLES=3`,
`WARMUP=30` and `SKIP_LOAD=0` into a fresh report file.
