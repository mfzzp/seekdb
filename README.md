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
| `INSERT INTO t VALUES (...), (...)` | optional column list; the whole statement is atomic |
| `SELECT ... FROM t [WHERE ...] [ORDER BY ...] [LIMIT n]` | projection, `*`, `= <> < <= > >=`, `AND`, `L2_DISTANCE(col,'[...]') AS alias` |
| `UPDATE t SET c = ... [WHERE ...]` | returns the number of rows whose value actually changed (MySQL semantics) |
| `DELETE FROM t [WHERE ...]` | returns the number of deleted rows |
| `BEGIN` / `COMMIT` / `ROLLBACK` | at most one explicit transaction per connection |

Semantics: statements outside a transaction commit automatically and are
`fsync`ed before the response; a transaction sees its own writes; a failed
statement leaves no partial effect; filtered TopK is computed inside the filter
and ties break on ascending primary key; vectors are stored as float32 and
distances use the stored values; `NaN`, `Infinity`, dimension mismatches and
float32 overflow are rejected. Unsupported syntax (`NULL`, `JOIN`, aggregation,
subqueries, `ALTER`/`DROP`, nested transactions, primary-key updates) returns
`unsupported_sql` instead of being silently reinterpreted.

Errors look like `{"id":3,"ok":false,"error":{"code":"duplicate_key","message":"..."}}`.
Codes: `invalid_request`, `unsupported_sql`, `unknown_table`, `table_exists`,
`unknown_column`, `duplicate_key`, `invalid_value`, `invalid_schema`,
`index_exists`, `transaction_active`, `no_transaction`.

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
   Coordinates above 1000 switch back to the direct difference kernel so the
   identity cannot lose digits to cancellation.
2. **Equality groups**: one array of snapshot slots per distinct value of a
   scalar column. A filtered TopK only computes distances for that group's
   slots — with or without a vector index.
3. **Planned paths**: primary-key-only predicates and orderings answer from the
   key directory without fetching payloads; scalar ordering ranks before
   fetching rows.
4. **Bounded cache**: a 96 MiB LRU row cache with scan admission control;
   bulk index builds bypass it so vectors are not stored twice.

All paths return exact results, so `ann` and `exact` agree and ranking ties keep
the primary-key rule.

### Known limitations

- The log keeps historical row versions; there is no compaction, so a workload
  of many updates grows the file and the recovery time.
- The first filtered query on a large table pays a one-time group build
  (about 4 s at 300k rows) plus the dense matrix; later filtered queries are
  ~1.5 ms. Memory grows with the matrix (358 MiB peak at 300k rows × 128 dims)
  and is capped at a quarter of the detected container limit.
- Non-equality scalar filters combined with distance ordering still scan.
- The matrix is rebuilt after any committed mutation; a write/query interleaving
  pays that rebuild each time.
- Rows are decoded into Python objects; very large tables are bounded by the
  cache and the matrix cap (192 MiB) rather than by true streaming.

## Verification

Local Linux server (`/usr/bin/python3` 3.12.3, shared 128-core host):

```sh
python3 protocol_check/run.py --command '["python3","minivectordb.py","--data-dir","{data_dir}"]' \
    --output /tmp/public
python3 -m unittest discover -s tests -v
```

- 20/20 public protocol cases pass with and without numpy.
- 15 regression tests pass (matrix paths, bitmap deletion, recovery truncation,
  query paths, scan cache, model fuzz); 4 are skipped when numpy is absent.
- Model fuzz: 220 random operations (multi-row inserts, predicate updates and
  deletes, transactions, restarts, TopK queries) match a Python model exactly.
- Differential fuzz against the previous revision: 360/360 generated queries
  identical (ids exact, distances to 1e-9).
- Protocol-level SIGKILL suite: 11/11 checks (committed rows survive, an open
  transaction leaves nothing behind, a 2000-row transaction does not leak,
  repeated kills during startup still recover).

Paired medians over three rounds, 10,000 rows × 128 dimensions, 500 unfiltered
and 500 filtered TopK queries with K=10:

| metric | before | after |
| --- | ---: | ---: |
| unfiltered TopK | 17.37 ms | 0.62 ms |
| filtered TopK | 1.48 ms | 0.31 ms |
| 1,000 queries total | 9.32 s | 0.64 s |
| peak RSS | 80 MiB | 74 MiB |

These are local shared-host measurements, not the competition score. The agent
material in `agent/conversation.json` records each experiment with its raw
numbers and limitations.
