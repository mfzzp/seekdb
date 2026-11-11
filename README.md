# MiniVectorDB

The submission runs on Python 3.12 with only the standard library. The container starts the database on stdin/stdout and stores its data under `/data`. Each request and response occupies one line of UTF-8 JSON. See [STDIO_PROTOCOL.md](STDIO_PROTOCOL.md) for the wire format.

```sh
docker build -t minivectordb .
docker volume create minivectordb-data
docker run --rm -i -v minivectordb-data:/data minivectordb
```

After the ready response, send the following lines against an empty data volume.

```jsonl
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), embedding VECTOR(2));"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'db','[3,4]'), (2,'sql','[0,0]');"}
{"id":3,"op":"execute","sql":"SELECT id, category FROM docs ORDER BY id;"}
{"id":4,"op":"execute","sql":"CREATE VECTOR INDEX docs_vec ON docs(embedding);"}
{"id":5,"op":"execute","search_mode":"ann","sql":"SELECT id, L2_DISTANCE(embedding,'[0,0]') AS d FROM docs WHERE category='db' ORDER BY d LIMIT 1;"}
{"id":6,"op":"execute","sql":"BEGIN;"}
{"id":7,"op":"execute","sql":"UPDATE docs SET category='updated' WHERE id=1;"}
{"id":8,"op":"execute","sql":"COMMIT;"}
{"id":9,"op":"execute","sql":"BEGIN;"}
{"id":10,"op":"execute","sql":"DELETE FROM docs WHERE id=1;"}
{"id":11,"op":"execute","sql":"ROLLBACK;"}
{"id":12,"op":"shutdown"}
```

Start another container with the same run command and send the following query. The returned row should be `[1,"updated"]`.

```jsonl
{"id":13,"op":"execute","sql":"SELECT id, category FROM docs WHERE id=1;"}
```

## SQL and storage

- A tokenizer and parser extract command parameters. The dispatcher sends `CREATE TABLE`, `CREATE VECTOR INDEX`, CRUD, and transaction commands to separate handlers.
- Tables require one signed `BIGINT` primary key and support `VARCHAR(n)` and one `VECTOR(n)`. Inserts can specify a column order and include multiple rows. Queries support projection, `*`, the five scalar comparisons, `AND`, `ORDER BY`, and `LIMIT`. Invalid or unsupported SQL returns an error object and leaves the process available.
- TopK computes Euclidean distance from stored float32 coordinates after applying all filters. Ordering ties use the primary key. An optional vector index groups row offsets by a scalar equality component and primary key, with a range for each component. The compact offset blocks are derived from the durable row log and are invalidated on commit. For unfiltered ANN requests, a sorted first-coordinate index can skip records whose lower bound exceeds the current TopK distance. Both paths preserve exact results. Unsupported shapes, transactional row overlays and large tables fall back to a scan.
- A length-framed append log stores schema and binary float32 row payloads. Primary keys map to disk offsets. A bounded row cache avoids loading every payload into memory. Each statement stages its changes and truncates its log suffix on failure. COMMIT flushes and syncs the commit marker before acknowledging. Recovery replays complete committed transactions and truncates the unfinished suffix.
- The log currently retains historical versions and has no compaction. The primary-key directory grows with row count. Large result sets can consume memory proportional to the returned rows.

## Checks

Run the official public tests against a Linux process or against the Docker image.

```sh
python3 protocol_check/run.py --command '["python3","minivectordb.py","--data-dir","{data_dir}"]' --output /tmp/minivectordb-public-process
python3 protocol_check/run.py --image minivectordb --output /tmp/minivectordb-public-container
```

The Linux process passed all 20 public cases. A separate 500-row test compared 48 pairs of exact and indexed queries across four limits, with and without scalar filters. It also verified that a forced termination kept committed changes and discarded uncommitted changes, and that a failed multi-row insert preserved an earlier successful statement in the same transaction.

## Server optimization verification

Development and tests ran under /mnt/HDD2/zzp on the requested server using its existing /usr/bin/python3 (3.12.3). Dependencies were unchanged. A specialized heap path avoids per-row ranking allocations for single-distance ascending TopK. ANN uses the first-coordinate index only up to eight dimensions; higher-dimensional queries use the exact heap scan. Scalar filters still run before selection.

Three alternating process runs used identical seeded data, 10,000 rows, 128 dimensions, 200 queries, K=10, half with scalar equality filters. Query times in seconds were baseline [2.336006, 2.417962, 2.506663] and optimized [1.694702, 1.452181, 1.320672]. Medians were 2.417962 and 1.452181, about 40% less time. Process peak RSS remained around 68 MiB. These local measurements include query execution and exclude loading, indexing and the ten warm-up queries. They are separate from the competition's end-to-end performance score.

All 200 complete results matched the baseline. All 20 official public cases passed. The regression test checks exact/ann, 2/128 dimensions, ties, filters, empty results, limits 0/1/10/100, and transactional deletion/rollback against an independent oracle.

```sh
python3 -m unittest discover -s tests -v
```
