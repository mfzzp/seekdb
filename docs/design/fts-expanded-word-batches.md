# Batch expanded FTS word rows

Control: `f4d74fd48fd8b3ef2f7a1a48e357766ba2c8117c`.
Validation ran on Windows on 2026-11-08/09. This revision retains the prior
count-only, compact-decoding, token-output and construction-worker changes.

## Implementation

FTS post-building previously forced the table-scan batch size to zero. Each
expanded word row crossed the SQL operator boundary separately. The new path
returns batches of cached rows from one document and fills the four generated
output columns in vectors. Source documents continue to be fetched by the
existing scalar path, including its visibility and checksum handling.

A batch stops at a document boundary: fetching the next document resets the
cache arena, so its shallow output references cannot survive that transition.
Source evaluation uses batch index zero and size one. The row cache exposes
contiguous pointer ranges without copying the rows or changing their order.

Code generation checks the four-column FTS index/doc-word layout and requires
batch-capable expressions. The document-ID expression follows the existing
adjacency convention; it is not assumed to have the SYS_DOC_ID function type.
Other output shapes and non-vectorized plans retain scalar execution.

Batch sizing recognizes FTS auxiliary-table post-building by schema type. Its
source LOB is consumed one document at a time, so the generic LOB frame cap is
relaxed for expanded word output, up to 256 and the configured rowset limit.
The source-document small-scan heuristic is also inappropriate for this output:
one document can produce thousands of words. Normal expression-size checks,
explicit LOB/rowset hints and non-vectorizable-expression handling remain.
No table names, parser-specific inputs, result counts or benchmark values enter
the decision. Selection is logged once per scan specification.

## Verification

LLVM 21 Windows incremental compilation and linking passed. Fresh dedicated
instances bootstrapped and returned SELECT 1. This uses the existing development
build, not a clean Linux build; Linux and official platform checks are pending.

- New integration test passed on control and final candidate. It constructs six
  indexes across space/BENG/IK and explicit-PK/heap tables; documents contain
  0/1/15/255/256/257/511/512/1024 distinct words, NULL/blank text and 600 repeated
  occurrences of one word. Matching primary keys, deletion visibility and
  rollback are checked with independent expected sets.
- The same test passed on a fresh candidate configured with
  `_rowsets_max_rows=8`. Logs confirm actual eight-row selection, while the
  default final candidate selects 256 for both auxiliary-table types.
- Concurrent DDL tests passed, including ordinary and FTS indexes, transactions
  and a failed unique-index build followed by successful work.
- Four existing SQL regressions passed on both versions, 57 assertions each;
  another 200 differential cases across space/BENG/IK/ngram passed.
- All 56 statements in the original TOKENIZE suite and 56 extra parser/output
  cases matched the control, including error codes.
- Original 20,000-document queries: full matching IDs, numeric relevance scores
  and raw LIMIT rows match exactly, with counts 8001/11000/7332/20.

With PyMySQL and a dedicated instance, run:

`SEEKDB_TEST_PORT=2881 python3 unittest/sql/engine/test_fts_build_batches.py`

`SEEKDB_TEST_PORT=2881 python3 unittest/rootserver/test_ddl_build_concurrency.py`

The tests create and drop unique databases. Optional host/user/password settings
use the SEEKDB_TEST_ prefix. The smaller-batch check used a fresh instance with
`--parameter _rowsets_max_rows=8`; the normal tests require no configuration change.

## Local measurements

Three construction pairs alternate execution order. Each recreates the original
20,000-document dataset and three original index definitions. Both processes use
the same logical CPUs 0-3, cpu_count=4, memory_budget=4G, INFO logging, 2G datafiles
and 2G log disks. No compilation or other benchmark overlaps these samples.

| Construction | Control mean seconds | Candidate mean seconds | Reduction |
| --- | ---: | ---: | ---: |
| IK title + content | 11.22958 | 10.33606 | 7.96% |
| IK content | 8.42815 | 8.02174 | 4.82% |
| BENG title + content | 5.10065 | 4.79520 | 5.99% |

All three improve in every pair. These are local persistent-PyMySQL timings,
not the official Linux client, and do not predict a competition score.

Six further pairs use 30 warmups, 3,000 calls per tokenizer and 200 per query.
Elapsed reductions are: IK 9.14%, BENG -0.86%, Chinese query 2.80%, BENG query
13.76%, mixed query 9.76%, limited count 27.43%, SELECT 1 1.80%. Query and
tokenization code is unchanged in this revision; these variable differences are
retained without attributing them to new hot-path code. Both instances received
ANALYZE and plan-cache flush before exact comparisons and query timing.

## Rejected dictionary-lifetime experiment

An earlier experiment pinned dictionary handles/wrappers per parser helper and
SQL expression context. It passed correctness checks but improved construction
only 0.52%/2.31% for the IK definitions while standalone IK tokenization regressed
31.39% in six pairs. BENG construction and tokenization also had unfavorable
samples. That implementation is not included; its patch, tests and logs remain
in the local temporary experiment directory. Raw experiment summaries are
included in the results JSON.

During test development, direct TEXT arguments to TOKENIZE failed with 1210 on
both versions. The abandoned expression-lifetime test used supported VARCHAR
input and explicit UTF-8 casts for NULL parameters; it passed on both. This
revision makes no change to that pre-existing direct-TEXT behavior. The final
construction regression uses TEXT columns through the existing FTS DDL LOB path.

Raw samples, source hashes, exact-result hashes, active-batch log excerpts and
regression results are in `fts-expanded-word-local-results.json`. Benchmark,
scoring, baseline and workflow files were not modified.
