"""Competition-shaped benchmark: 10k rows x 128 dims, batched writes, 1k TopK.

Mirrors the published performance setup (10,000 records, 128 dimensions, 1,000
TopK queries with K=10, half of them filtered) while separating the write
phase, the unfiltered query phase and the filtered query phase, and reporting
medians instead of a single aggregate.

Usage: python3 tests/bench_competition.py SOURCE_DIR [--rows N] [--queries N]
                                                 [--index/--no-index] [--tag T]
"""

import argparse
import hashlib
import json
import random
import resource
import statistics
import struct
import sys
import tempfile
import time

sys.path.insert(0, sys.argv[1])
from minivectordb import Database  # noqa: E402

DIM = 128
K = 10
CATEGORIES = 20


def build_rows(rows, seed):
    rng = random.Random(seed)
    payload = []
    for index in range(rows):
        values = [rng.gauss(0.0, 0.5) for _ in range(DIM)]
        payload.append((index + 1, f"cat{index % CATEGORIES}", f"doc{index}",
                        struct.pack(f"<{DIM}f", *values)))
    return payload


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--index", action="store_true", default=True)
    parser.add_argument("--no-index", dest="index", action="store_false")
    parser.add_argument("--tag", default="run")
    parser.add_argument("--seed", type=int, default=20261111)
    args = parser.parse_args()

    rows = build_rows(args.rows, args.seed)
    rng = random.Random(args.seed + 1)
    targets = [struct.pack(f"<{DIM}f", *[rng.gauss(0.0, 0.5) for _ in range(DIM)])
               for _ in range(max(1, args.queries // 2))]

    with tempfile.TemporaryDirectory() as root:
        db = Database(root)
        try:
            start = time.perf_counter()
            db.execute("CREATE TABLE bench (id BIGINT PRIMARY KEY, category VARCHAR(16), "
                       "content VARCHAR(32), embedding VECTOR(128))", "exact")
            if args.index:
                db.execute("CREATE VECTOR INDEX bench_vec ON bench(embedding)", "exact")
            for base in range(0, len(rows), 100):
                batch = rows[base:base + 100]
                values = ",".join(
                    "(%d,'%s','%s','[%s]')" % (pk, category, content,
                                                ",".join("%.6g" % value for value in
                                                         struct.unpack(f"<{DIM}f", packed)))
                    for pk, category, content, packed in batch)
                db.execute(f"INSERT INTO bench VALUES {values}", "exact")
            write_seconds = time.perf_counter() - start

            unfiltered, filtered = [], []
            digests = []
            id_digests = []
            for step in range(args.queries // 2):
                target = targets[step % len(targets)]
                literal = "[" + ",".join("%.6g" % value for value in
                                         struct.unpack(f"<{DIM}f", target)) + "]"
                sql_plain = (f"SELECT id,L2_DISTANCE(embedding,'{literal}') AS d "
                             f"FROM bench ORDER BY d LIMIT {K}")
                sql_filtered = (f"SELECT id,L2_DISTANCE(embedding,'{literal}') AS d "
                                f"FROM bench WHERE category = 'cat{step % CATEGORIES}' "
                                f"ORDER BY d LIMIT {K}")
                start = time.perf_counter()
                result = db.execute(sql_plain, "exact")
                unfiltered.append(time.perf_counter() - start)
                digests.append(hashlib.sha256(json.dumps(result).encode()).hexdigest())
                id_digests.append(hashlib.sha256(
                    json.dumps([row[0] for row in result["rows"]]).encode()).hexdigest())
                start = time.perf_counter()
                result = db.execute(sql_filtered, "exact")
                filtered.append(time.perf_counter() - start)
                digests.append(hashlib.sha256(json.dumps(result).encode()).hexdigest())
                id_digests.append(hashlib.sha256(
                    json.dumps([row[0] for row in result["rows"]]).encode()).hexdigest())
            peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024
            print(json.dumps({
                "tag": args.tag, "source": args.source, "rows": args.rows,
                "queries": args.queries, "index": args.index,
                "write_seconds": write_seconds,
                "unfiltered_median_ms": statistics.median(unfiltered) * 1000,
                "unfiltered_total_s": sum(unfiltered),
                "filtered_median_ms": statistics.median(filtered) * 1000,
                "filtered_total_s": sum(filtered),
                "query_total_s": sum(unfiltered) + sum(filtered),
                "peak_rss_mib": peak,
                "digest": hashlib.sha256("".join(digests).encode()).hexdigest(),
                "ids_digest": hashlib.sha256("".join(id_digests).encode()).hexdigest(),
            }, ensure_ascii=False))
        finally:
            db.close()


if __name__ == "__main__":
    main()
