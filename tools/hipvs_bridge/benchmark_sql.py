"""Diagnostic full-input SQL benchmark; never a substitute for the official runner.

The dataset prefix contains FP32 .f32/.queries.f32 files and uint32 .truth.u32
(100 exact neighbors per query), as emitted by benchmark_recall.cpp. Each run
creates a new database and retains it for --mode verify after a server restart.
Connection credentials come from SEEKDB_TEST_HOST/PORT/USER/PASSWORD.
"""
import argparse
import array
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import time


def values(path, kind):
    result = array.array(kind)
    with path.open("rb") as stream:
        result.fromfile(stream, path.stat().st_size // result.itemsize)
    return result


def vector(row):
    return "[" + ",".join(format(x, ".9g") for x in row) + "]"


def row_id(offset):
    return -(1 << 40) + offset * 97


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("prepare", "run", "verify"), required=True)
    parser.add_argument("--dataset-prefix", type=Path, required=True)
    parser.add_argument("--dimension", type=int, required=True)
    parser.add_argument("--rows", type=int, default=250000)
    parser.add_argument("--queries", type=int, default=4096)
    parser.add_argument("--database", default="cuvs_diagnostic")
    parser.add_argument("--label", default="candidate")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if sys.byteorder != "little" or array.array("f").itemsize != 4 or array.array("I").itemsize != 4:
        parser.error("requires little-endian FP32/uint32 files")
    if not re.fullmatch(r"[a-zA-Z][a-zA-Z0-9_]{0,63}", args.database):
        parser.error("invalid database identifier")
    if args.dimension < 1 or args.rows < 100 or args.queries < 1:
        parser.error("invalid shape")
    prefix = args.dataset_prefix.resolve()
    tsv = prefix.with_suffix(".tsv")
    if args.mode == "prepare":
        data = values(prefix.with_suffix(".f32"), "f")
        assert len(data) == args.rows * args.dimension
        with tsv.open("x") as stream:
            for i in range(args.rows):
                stream.write(f"{row_id(i)}\t{vector(data[i * args.dimension:(i + 1) * args.dimension])}\n")
        print(json.dumps({"prepared_rows": args.rows, "bytes": tsv.stat().st_size}))
        return
    if args.output is None or args.output.exists():
        parser.error("--output must name a new result file")
    import pymysql
    queries = values(prefix.with_suffix(".queries.f32"), "f")
    truth = values(prefix.with_suffix(".truth.u32"), "I")
    assert len(queries) == args.queries * args.dimension and len(truth) == args.queries * 100
    probes = [vector(queries[i * args.dimension:(i + 1) * args.dimension]) for i in range(args.queries)]
    result = dict(label=args.label, rows=args.rows, dimension=args.dimension,
                  query_count=args.queries, mode=args.mode, database=args.database,
                  queries={}, started=time.time())

    def save():
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    table = args.database + ".items"
    with pymysql.connect(host=os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
                         port=int(os.environ.get("SEEKDB_TEST_PORT", "2881")),
                         user=os.environ.get("SEEKDB_TEST_USER", "root"),
                         password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
                         autocommit=True, local_infile=True, read_timeout=600) as connection, connection.cursor() as cursor:
        cursor.execute("SET ob_query_timeout=600000000")
        if args.mode == "run":
            cursor.execute("CREATE DATABASE " + args.database)
            cursor.execute(f"CREATE TABLE {table}(id BIGINT PRIMARY KEY,v VECTOR({args.dimension}))")
            begin = time.perf_counter()
            cursor.execute(f"LOAD DATA LOCAL INFILE %s INTO TABLE {table} FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' (id,v)", (str(tsv),))
            result["load_seconds"] = time.perf_counter() - begin
            cursor.execute(f"SELECT COUNT(*),COUNT(v) FROM {table}")
            assert cursor.fetchone() == (args.rows, args.rows)
            save()
            begin = time.perf_counter()
            cursor.execute(f"CREATE VECTOR INDEX idx ON {table}(v) WITH (distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64)")
            result["ddl_seconds"] = time.perf_counter() - begin
            cursor.execute(f"SELECT id FROM {table} ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 100", (probes[0],))
            first = [row[0] for row in cursor.fetchall()]
            assert len(first) == 100 and len(set(first)) == 100
            result["build_including_first_query_seconds"] = time.perf_counter() - begin
            save()
        cursor.execute(f"EXPLAIN SELECT id FROM {table} ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 100", (probes[0],))
        result["plan"] = [list(row) for row in cursor.fetchall()]
        for k in (10, 100):
            sql = f"SELECT id FROM {table} ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT {k}"
            for query in probes[:30]:
                cursor.execute(sql, (query,)); cursor.fetchall()
            hits, latencies, digest = 0, [], hashlib.sha256()
            for i, query in enumerate(probes):
                begin = time.perf_counter()
                cursor.execute(sql, (query,))
                ids = [row[0] for row in cursor.fetchall()]
                latencies.append(time.perf_counter() - begin)
                assert len(ids) == k and len(set(ids)) == k
                assert all((rid + (1 << 40)) % 97 == 0 and 0 <= (rid + (1 << 40)) // 97 < args.rows for rid in ids)
                hits += len(set(ids) & {row_id(int(x)) for x in truth[i * 100:i * 100 + k]})
                digest.update(repr(ids).encode())
            latencies.sort()
            result["queries"][str(k)] = dict(recall=hits / (args.queries * k), qps=args.queries / sum(latencies),
                total_seconds=sum(latencies), p50_seconds=latencies[args.queries // 2],
                p95_seconds=latencies[args.queries * 95 // 100], max_seconds=latencies[-1],
                result_sha256=digest.hexdigest(), ids_unique_and_valid=True)
            save(); print(json.dumps(result["queries"][str(k)]), flush=True)
    result.update(completed=time.time(), exit_code=0)
    save()


if __name__ == "__main__":
    main()
