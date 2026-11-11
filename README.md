# MiniVectorDB

Single-process vector database for the `2026-competition-round2` task. It speaks
one-line JSON on stdin/stdout (see [STDIO_PROTOCOL.md](STDIO_PROTOCOL.md)) and
keeps all data under `/data`.

- Language: Python 3.12, standard library only.
- Optional acceleration: `numpy` is installed best-effort in the image; without
  it the pure-Python distance path runs and results are unchanged.

## Build and run

```sh
docker build -t minivectordb .
docker run --rm -i --network=none --cpus=2 --memory=2g \
  -v minivectordb-data:/data minivectordb
```

The container stays in the foreground, prints `{"type":"ready"}` after recovery,
then answers one response per request. `stderr` is diagnostics only.
Locally: `python3 minivectordb.py --data-dir /tmp/mvdb`.

## SQL scope

| Statement | Notes |
| --- | --- |
| `CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(n), v VECTOR(d))` | one signed BIGINT primary key, at most one vector column; column or table level `PRIMARY KEY` |
| `CREATE VECTOR INDEX name ON t(v)` | records index metadata; queries stay exact in both modes |
| `INSERT INTO t VALUES (...), (...)` | optional column list that may reorder the values but must name every column exactly once (no NULL or DEFAULT to fall back on); the whole statement is atomic |
| `SELECT ... FROM t [WHERE ...] [ORDER BY ...] [LIMIT n]` | projection, `*`, `= < <= > >=`, `AND`, `L2_DISTANCE(col,'[...]') AS alias` |
| `UPDATE t SET c = ... [WHERE ...]` | returns the number of rows whose value actually changed (MySQL semantics) |
| `DELETE FROM t [WHERE ...]` | returns the number of deleted rows |
| `BEGIN` / `COMMIT` / `ROLLBACK` | at most one explicit transaction per connection; `COMMIT`/`ROLLBACK` outside one are successful no-ops |

Semantics: statements outside a transaction commit automatically and are
`fsync`ed before the response; a transaction sees its own writes; a failed
statement leaves no partial effect; filtered TopK is computed inside the filter
and ties break on ascending primary key; vectors are stored as float32 and
distances use the stored values; `NaN`, `Infinity`, dimension mismatches and
float32 overflow are rejected. Without `ORDER BY`, rows come back in primary-key
insertion order — an `UPDATE` keeps a row's position, a delete followed by a
re-insert moves it to the end — which is what the pinned `extra_row_order` case
records, because the public results never compare a multi-row answer that lacks
`ORDER BY`. DDL inside a transaction joins it (the PostgreSQL reading): a
ROLLBACK also removes a table created in that transaction, where MySQL would
have committed it implicitly. Unsupported syntax (`NULL`, `JOIN`, aggregation,
subqueries, `ALTER`/`DROP`, `<>`, parenthesised predicates, qualified column
references, nested transactions, primary-key updates) returns `unsupported_sql`
instead of being silently reinterpreted.

Errors look like `{"id":3,"ok":false,"error":{"code":"duplicate_key","message":"..."}}`.
Codes: `invalid_request`, `unsupported_sql`, `unknown_table`, `table_exists`,
`unknown_column`, `duplicate_key`, `invalid_value`, `invalid_schema`,
`index_exists`, `transaction_active`.

## Example from an empty data directory

```jsonl
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), embedding VECTOR(2));"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'db','[3,4]'),(2,'sql','[0,0]');"}
{"id":3,"op":"execute","sql":"SELECT id, category FROM docs ORDER BY id;"}
{"id":4,"op":"execute","sql":"CREATE VECTOR INDEX docs_vec ON docs(embedding);"}
{"id":5,"op":"execute","sql":"SELECT id,L2_DISTANCE(embedding,'[0,0]') AS d FROM docs WHERE category='db' ORDER BY d LIMIT 1;"}
{"id":6,"op":"execute","sql":"BEGIN;"}
{"id":7,"op":"execute","sql":"UPDATE docs SET category='updated' WHERE id=1;"}
{"id":8,"op":"execute","sql":"COMMIT;"}
{"id":9,"op":"execute","sql":"BEGIN;"}
{"id":10,"op":"execute","sql":"DELETE FROM docs WHERE id=1;"}
{"id":11,"op":"execute","sql":"ROLLBACK;"}
{"id":12,"op":"shutdown"}
```

Start the container again against the same volume and query; the row is still
`[1,"updated"]` because the rollback discarded only the delete.

## Design

### Row log

`/data/rows.log` is an append-only file of length-prefixed frames:

| tag | payload | meaning |
| --- | --- | --- |
| `r` | `(table, key)` header + marshalled row | one row version, addressable by its file offset |
| `m` | marshalled operation | `CREATE TABLE`, `CREATE VECTOR INDEX`, or a delete tombstone |
| `!` | — | commit marker |

Primary keys map to row offsets in an in-memory directory. Every statement
stages its writes; a failing statement truncates its own suffix. `COMMIT`
appends the commit marker, flushes, `fsync`s and only then answers. Recovery
replays frames up to the last commit marker and truncates the unfinished
suffix, so a forced kill keeps committed data and drops partial transactions.

### Queries

Costs are cut in four places, in order of effect:

1. **Dense matrix + BLAS** (numpy present, table ≥ 2048 rows, no pending
   transaction, LIMIT present): live float32 vectors are materialised once into
   a float64 matrix keyed by the stable snapshot; distances come from
   `|v|² − 2v·q + |q|²` in one mat-vec, and TopK is `argpartition` plus
   boundary tie expansion, then `lexsort` on `(distance, primary key)`.
   The identity is only used to *propose* candidates: its absolute error is
   bounded by `4·width·eps·(‖v‖+‖q‖)²`, so every row within twice that bound of
   the K-th threshold is recomputed with the direct difference kernel before the
   final order is taken, and the whole set is recomputed when that window grows
   past `max(8K, 512)`. Coordinates above 1000 bypass the identity entirely.
   `eps` is the epsilon of the dtype the matrix was built with: above
   `MATRIX_MAX_BYTES` the matrix is float32, whose epsilon is nine orders of
   magnitude larger, and a float64 bound would make the window far too narrow.
2. **Equality groups**: one array of snapshot slots per distinct value of a
   scalar column. A filtered TopK only computes distances for that group's
   slots — with or without a vector index.
3. **Planned paths**: primary-key-only predicates and orderings answer from the
   key directory without fetching payloads; scalar ordering ranks before
   fetching rows.
4. **Bounded memory**: a row cache with scan admission control; bulk index
   builds bypass it so vectors are not stored twice. A single `MemoryBudget`
   derived from the cgroup limit bounds the cache, every table matrix, every
   equality group and the scalar index together; when a growth does not fit,
   other tables' structures are dropped first, and if it still does not fit the
   matrix is not built and the query falls back to a chunked scan. Kernels work
   in 4096-row chunks so a gather or a difference never materialises a
   table-sized temporary.
5. **Scalar-first filtering**: a compact per-column index over the stable slots
   (int64 for BIGINT so comparisons above 2^53 stay exact, dictionary codes plus
   ranks for VARCHAR so ranges keep code-point order) resolves `=`, `<`, `<=`,
   `>`, `>=` and `AND` chains to candidate slots before any vector work. Up to
   256 candidates are read straight from the log, a set covering a quarter of
   the table is answered from the dense matrix with the distance array indexed,
   and the middle band gathers the subset in chunks.

### Measured behaviour

The full tables are in [VERIFICATION.md](VERIFICATION.md). At 10k rows x 128
dimensions with K=10 the headline results are:

- Resolving scalar predicates before any vector work takes 0.29 ms at 0.1%
  selectivity and 0.77 ms at 100%, against 3.2 ms and 16.9 ms for a plain scan.
- A TopK request costs 645 us inside the server — 89% execution, 7% SQL parse and
  4% JSON in both directions — plus about 43 us of pipe and scheduling.
- The dense matrix is built on the first query because it already wins there
  (50 ms against 68 ms for the scan). On 100k rows a single query is twice as fast
  and 3.5x lighter with chunking, and the matrix takes over from the second query.
- Under a 2 GiB container limit the matrix (146.5 MiB at 150k rows) is allowed and
  peak resident memory is 348 MiB; at 256 MiB and below the budget refuses it, the
  query falls back to the chunked path (32.8 ms against 2.1 ms warm, still exact)
  and peak memory drops to about 70 MiB.

### Known limitations

- The log keeps historical row versions; there is no compaction, so a workload
  of many updates grows the file and the recovery time.
- The first filtered query on a large table pays one-time builds: at 300k rows ×
  128 dimensions the equality path costs 3.0 s (groups plus a 146 MiB matrix) and
  the first range query costs 1.7 s more (the scalar index); afterwards equality
  queries take 3.1 ms and range queries 16.7 ms. Peak resident memory for that
  table is 358 MiB, governed by the shared budget derived from the container
  limit.
- `<>`, parenthesised predicates, qualified column references, NULL literals and
  aggregates are rejected as unsupported, matching the SQL subset the task lists.
- The matrix is rebuilt after any committed mutation; a write/query interleaving
  pays that rebuild each time, which is why a write-heavy soak slightly favours
  the pure-Python path.
- Rows are decoded into Python objects; very large tables are bounded by the
  cache and the matrix cap (192 MiB) rather than by true streaming.

## Verification

Local Linux server (`/usr/bin/python3` 3.12.3, shared 128-core host):

```sh
python3 protocol_check/run.py --command '["python3","minivectordb.py","--data-dir","{data_dir}"]' \
    --output /tmp/public
python3 -m unittest discover -s tests -v
```

- 20/20 public protocol cases pass with and without numpy, plus twelve extra cases
  through the same runner: range and AND predicates, restart with an index, two
  tables across a restart, a failed statement inside a transaction, deleting every
  row inside a transaction and rolling it back, LIMIT 0, non-ASCII values, ann vs
  exact, the row order of a multi-row answer without ORDER BY, and vector
  distances after an update and a delete.
- 124 regression tests pass: matrix paths, numeric bounds and float32 margins,
  memory budget, chunked TopK, scalar filters, ann/exact equivalence, bitmap
  deletion, recovery truncation, real SIGKILL recovery, mixed-workload soak, query
  paths, scan cache, SQL surface, model fuzz, semantics audit, startup
  diagnostics, the protocol-level shape/resilience/read-only suite, a golden
  transcript of 88 statements, the twelve extra judge cases, and the evidence
  document's own schema and size budget. 31 are skipped when numpy is absent.
- Model fuzz: 220 random operations (multi-row inserts, predicate updates and
  deletes, transactions, restarts, TopK queries) match a Python model exactly.
- Differential fuzz against the original baseline: 480/480 generated queries
  identical over four seeds at the current revision, and 720/720 over six seeds
  in an earlier run.
- SQL surface: ~75 statements × exact/ann all answer cleanly, and the connection
  survives every error.
- Crash recovery: six scenarios under a real SIGKILL — between commits, inside an
  open transaction, during a 100k-row statement, a 10k-row transaction killed
  before COMMIT, a kill that lands inside recovery itself (a 300k-row log, with the
  test asserting no ready line was produced first), and a torn log suffix.

### Timings through the real JSONL protocol

`scripts/protocol_bench.py` starts the program as a subprocess and talks to it
over stdin/stdout exactly like the judge, so the numbers include JSON
encode/decode, pipe traffic and per-response flushes. Measured again against the
original baseline at the final commit: 10,000 rows × 128 dimensions, 1,000 TopK
queries with K=10 (half filtered): **7.4 s baseline, 0.95 s with numpy and 7.2 s
without** (unfiltered median 11.8 ms → 1.13 ms, filtered 1.74 ms → 0.58 ms) — the
full phase-by-phase table is in [VERIFICATION.md](VERIFICATION.md).

A control pair (same code against itself) showed a 4% write noise floor, so the
~5% write difference with numpy is at the edge of measurable and comes from the
numpy runtime being loaded, not from the write path. These are local shared-host
measurements, not the competition score. The agent material in
`agent/conversation.json` records each experiment with its raw numbers and
limitations.
