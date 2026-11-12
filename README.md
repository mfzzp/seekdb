# MiniVectorDB C++17

This branch is the native C++17 implementation for the `2026-competition-round2`
task. The database process is `/app/minivectordb`; it speaks one JSON object per
line on stdin and writes one response per line on stdout. The only persistent
state is kept under `/data`.

The previous Python database implementation is removed from this branch. The
container uses Python only as the base image because the supplied protocol
checker is written in Python; the runtime entrypoint and all SQL execution are
native C++.

## Build and run

```sh
docker build -t minivectordb:cpp .
docker run --rm -i --network=none --cpus=2 --memory=2g \
  -v minivectordb-data:/data minivectordb:cpp
```

The process prints `{"type":"ready"}` after loading `/data/state.bin`. It then
accepts JSONL requests until a `shutdown` request or EOF. Diagnostics go to
stderr and never mix with the protocol stream.

For a local native build:

```sh
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -o minivectordb minivectordb.cpp
./minivectordb --data-dir /tmp/minivectordb
```

## SQL surface

The implementation covers the competition protocol subset:

- `CREATE TABLE` with `BIGINT`, bounded `VARCHAR`, one `VECTOR`, and one
  `PRIMARY KEY` (inline or table-level).
- `CREATE VECTOR INDEX name ON table(vector)`; index metadata survives restart
  and exact semantics are retained in both `exact` and `ann` request modes.
- Multi-row `INSERT`, optional complete column lists, duplicate-key checking,
  `UPDATE`, and `DELETE`.
- `SELECT` projections and `*`, scalar comparisons joined by `AND`,
  `L2_DISTANCE(vector,'[...]') AS alias`, `ORDER BY`, and `LIMIT`.
- `BEGIN`, `COMMIT`, and `ROLLBACK`, including DDL in an explicit transaction.

Writes are staged before mutation, so a malformed or duplicate row cannot leave
half a statement visible. Autocommit publishes a new binary snapshot with a
temporary file, flush, `fsync`, and atomic rename. A transaction sees its own
writes and only publishes them at `COMMIT`; shutdown rolls back an open
transaction. Deleted rows use an in-memory live bitmap and are not returned or
resurrected by a restart.

Vectors are converted to float32 at write time. Distance calculations use the
stored float32 values in a native tight loop and return deterministic primary-key
tie ordering. Non-finite values, dimension mismatches, overflow, invalid SQL,
and unsupported constructs produce structured errors and keep the process alive.

## Protocol example

```jsonl
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), embedding VECTOR(2));"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'db','[3,4]'),(2,'sql','[0,0]');"}
{"id":3,"op":"execute","sql":"CREATE VECTOR INDEX docs_vec ON docs(embedding);"}
{"id":4,"op":"execute","sql":"SELECT id,L2_DISTANCE(embedding,'[0,0]') AS d FROM docs ORDER BY d LIMIT 1;"}
{"id":5,"op":"shutdown"}
```

## Validation

The native binary was compiled with `g++ -O2 -std=c++17` on the Linux test
host and with `g++ -O3 -DNDEBUG` inside the Docker image. The image
`minivectordb:cpp-20261112` passed all 20 public protocol cases and all 12
additional cases, including restart, rollback, batch atomicity, index metadata,
ANN-mode equivalence, deletes, updates, filtering, ordering, Unicode, and
vector mutation paths. The exact commands and result summaries are recorded in
[`agent/cpp-validation-20261112.json`](agent/cpp-validation-20261112.json).

The protocol checker remains Python because it is supplied test infrastructure;
it invokes the C++ process or the C++ Docker image and does not provide any
database functionality.

## Files

- `minivectordb.cpp`: JSON parser, SQL lexer/parser, transactional engine,
  float32 vector distance path, and binary persistence.
- `Dockerfile`: compiles and runs the native binary.
- `STDIO_PROTOCOL.md`: JSONL contract.
- `protocol_check/`, `mysqltest/`, `tests/`: supplied protocol and regression
  corpus; they are not runtime dependencies.
