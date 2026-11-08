# FTS count batches and compact string-diff decoding

Control: `adc0704eb4f716659f721dc808ad18bc6a5f6463`, reported by the user as
35.8816/50 on the competition platform. This revision keeps its construction
workers, short-list merge and TOKENIZE binary output.

## Changes

The code generator marks a covering, score-free FTS scan as count-only when
it exposes no nonconstant output and has no residual scan filters or ranking.
Function lookup, ES matching, vector lookup and index merge are excluded.
An existing reserved bit carries the property in the already serialized IR
flags byte; no new wire field or structure-size change is introduced there.
The text DAAT iterator additionally checks natural-language mode, no score
projection/filter, no function lookup and no top-k before using the property.

Internal posting-list document IDs still drive ordering and duplicate removal;
the collector still applies match acceptance, and the original storage scans
still check transaction visibility. Accepted rows advance the existing
LIMIT/OFFSET counters without copying document IDs/scores into result buffers
or projecting those unused values. An observable ID, relevance or residual
filter retains the normal path. This applies to general count-only queries,
not a particular table, input text, hit count or LIMIT value.

COUNT(*) aggregation now uses the selector's known count, or batch-slice length
minus a population count of its skip bits. Other aggregates, COUNT(column),
DISTINCT and window semantics retain their existing handling.

For fixed-length, non-hex STRING_DIFF columns of length 16, the decoder builds
the common-byte template and difference positions once per batch, then patches
each requested row into a packed 16-byte output slot. NULL handling is retained;
other lengths, variable-length cells and hex packing use the original decoder.
The format/type conditions are independent of table and column names. No
on-disk format changes and no new architecture-specific instructions are used.

## Validation on 2026-11-08

Windows LLVM 21 incremental compile/link succeeded. Two fresh dedicated
instances bootstrapped and returned SELECT 1. This uses the existing development
build and is not a clean Linux build; Linux and platform evaluation are pending.

- Native standalone decoder differential: 65,576 batches passed. Every one of
  the 65,536 difference masks for 16-byte strings is checked against scalar
  decoding and independently constructed bytes. Cases cover NULLs, reordered
  and duplicate row IDs, all-common/all-different strings and generic fallback
  lengths 1/15/17/31. Compilation, linking and execution each exited 0.
- New count-batch integration: 3 tests passed, covering sparse/all/empty skip
  masks, grouped and distinct aggregates, NULL values, window counts, observed
  IDs, SUM, residual filters and limits/offsets across batch boundaries.
- Existing count-limit (5), short-merge (5) and concurrent DDL (2) integration
  tests passed, including prepared reuse, rollback and failed unique-index DDL.
- Four SQL regressions passed on both versions, 57 assertions per version;
  200 additional differential cases across space/BENG/IK/ngram passed.
- Original 20,000-document queries: all matching IDs, numeric relevance scores
  and raw limited rows matched exactly. Hit counts remain 8001/11000/7332/20.

With PyMySQL and a dedicated running instance:

`SEEKDB_TEST_PORT=2881 python3 unittest/sql/engine/test_fts_count_batches.py`

`SEEKDB_TEST_PORT=2881 python3 unittest/sql/engine/test_fts_count_limit.py`

`SEEKDB_TEST_PORT=2881 python3 unittest/sql/engine/test_fts_short_merge.py`

`SEEKDB_TEST_PORT=2881 python3 unittest/rootserver/test_ddl_build_concurrency.py`

The tests create and drop unique databases. Optional host/user/password
variables use the SEEKDB_TEST_ prefix. The standalone C++ decoder test under
unittest/sql/engine uses the engine compile/link dependencies in place of the
observer main; it is not a registered CMake target.

## Local timing

Six pairs alternate control/candidate order. Each process uses the same CPUs
0-3, cpu_count=4, memory_budget=4G, INFO logging, 2G data and 2G log settings.
Original data and SQL are unchanged; both instances run ANALYZE and flush their
plan cache before comparison. Each metric has 30 warmups, 3,000 tokenization
calls or 200 query calls. The persistent PyMySQL harness differs from the
official Linux client. No compilation or other benchmark overlaps final timing.

| Metric | Control ms | Candidate ms | Elapsed reduction |
| --- | ---: | ---: | ---: |
| Chinese count | 1.56568 | 1.05478 | 32.63% |
| BENG count | 2.41703 | 1.71386 | 29.09% |
| Mixed count | 1.85345 | 1.36321 | 26.45% |
| Count over limited view | 0.35553 | 0.35914 | -1.02% |
| IK TOKENIZE | 0.10859 | 0.11014 | -1.43% |
| BENG TOKENIZE | 0.05985 | 0.06087 | -1.69% |
| SELECT 1 | 0.02028 | 0.01996 | 1.56% |

The three full count queries improve in every pair, with process CPU reductions
31.67%/31.09%/27.89%. Small differences in unaffected tokenization, LIMIT and
SELECT 1 are retained rather than attributed to this revision. These are
combined changes, not isolated estimates of each change's contribution.
Background OS/database variability remains; no platform score is predicted.

One sequential construction smoke run recorded control 8.122/6.313/3.681 s
and candidate 8.451/6.499/3.777 s for IK-all/IK-content/BENG. Construction code
is unchanged, no repeated construction comparison was performed, and no
construction gain is claimed. Raw unfavorable samples are preserved.

No benchmark, scoring, baseline or workflow file was changed. Raw timing,
source hashes, complete-result hashes and verification summaries are in
fts-count-batches-local-results.json. IK dictionary lifetime changes and
word-row construction batching are separate future experiments and are not
included in this revision.
