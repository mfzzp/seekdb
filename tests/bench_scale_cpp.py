#!/usr/bin/env python3
"""Large-scale JSONL benchmark for the native C++ MiniVectorDB.

The workload deliberately uses unique random vector literals so the query
cache cannot turn the run into a cache-only benchmark.  It measures batched
inserts, exact/ANN Top-K queries, restart recovery, RSS and on-disk bytes.

Examples (run on Linux):
  python3 tests/bench_scale_cpp.py --binary ./minivectordb --rows 100000
  python3 tests/bench_scale_cpp.py --binary ./minivectordb --rows 1000000 \
      --queries 1000 --search-mode mixed
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import shutil
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def rss_mib(pid: int) -> float | None:
    status = Path(f"/proc/{pid}/status")
    try:
        for line in status.read_text(encoding="ascii").splitlines():
            if line.startswith("VmRSS:"):
                return float(line.split()[1]) / 1024.0
    except (FileNotFoundError, OSError, ValueError):
        return None
    return None


def tree_bytes(root: Path) -> int:
    total = 0
    for path in root.rglob("*"):
        try:
            if path.is_file():
                total += path.stat().st_size
        except FileNotFoundError:
            pass
    return total


class Client:
    def __init__(self, binary: str, data_dir: Path):
        self.proc = subprocess.Popen(
            [binary, "--data-dir", str(data_dir)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.ident = 1
        ready = self.proc.stdout.readline() if self.proc.stdout else ""
        if json.loads(ready).get("type") != "ready":
            raise RuntimeError(f"database did not become ready: {ready!r}")

    def execute(self, sql: str, mode: str = "exact") -> dict:
        if not self.proc.stdin or not self.proc.stdout:
            raise RuntimeError("database pipes are closed")
        request = {
            "id": self.ident,
            "op": "execute",
            "sql": sql,
            "search_mode": mode,
        }
        self.ident += 1
        self.proc.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            error = self.proc.stderr.read() if self.proc.stderr else ""
            raise RuntimeError(f"database exited while executing {sql[:80]!r}: {error}")
        response = json.loads(line)
        if not response.get("ok", False):
            raise RuntimeError(f"SQL failed: {response}")
        return response

    def shutdown(self) -> None:
        if self.proc.poll() is None:
            if not self.proc.stdin or not self.proc.stdout:
                raise RuntimeError("database pipes are closed")
            self.proc.stdin.write(
                json.dumps({"id": self.ident, "op": "shutdown"}, separators=(",", ":"))
                + "\n"
            )
            self.ident += 1
            self.proc.stdin.flush()
            line = self.proc.stdout.readline()
            if not line or not json.loads(line).get("ok", False):
                error = self.proc.stderr.read() if self.proc.stderr else ""
                raise RuntimeError(f"database shutdown failed: {line!r} {error}")
            self.proc.wait(timeout=60)
        if self.proc.returncode != 0:
            error = self.proc.stderr.read() if self.proc.stderr else ""
            raise RuntimeError(f"database exited with {self.proc.returncode}: {error}")


def vector_literal(values: list[float]) -> str:
    return "[" + ",".join(f"{value:.5f}" for value in values) + "]"


def build_vector(rng: random.Random, dimension: int) -> list[float]:
    return [rng.uniform(-1.0, 1.0) for _ in range(dimension)]


def run(args: argparse.Namespace) -> dict:
    root = Path(args.data_dir) if args.data_dir else Path(
        tempfile.mkdtemp(prefix="minivectordb-scale-")
    )
    if args.data_dir:
        if root.exists():
            shutil.rmtree(root)
        root.mkdir(parents=True)

    rng = random.Random(args.seed)
    queries_rng = random.Random(args.seed ^ 0x5EED)
    category_count = max(8, args.categories)
    rss_peak = 0.0
    query_times: list[float] = []
    query_rows = 0
    digest = hashlib.sha256()
    client = Client(args.binary, root)
    try:
        client.execute(
            f"CREATE TABLE bench (id BIGINT PRIMARY KEY, "
            f"category VARCHAR(16), embedding VECTOR({args.dimension}));"
        )
        if args.index:
            client.execute("CREATE VECTOR INDEX bench_vec ON bench(embedding);")

        write_start = time.perf_counter()
        for base in range(0, args.rows, args.batch):
            values = []
            for row_id in range(base + 1, min(args.rows, base + args.batch) + 1):
                category = f"cat{row_id % category_count}"
                literal = vector_literal(build_vector(rng, args.dimension))
                values.append(f"({row_id},'{category}','{literal}')")
            client.execute("INSERT INTO bench VALUES " + ",".join(values) + ";")
            current_rss = rss_mib(client.proc.pid)
            if current_rss is not None:
                rss_peak = max(rss_peak, current_rss)
        write_seconds = time.perf_counter() - write_start
        bytes_after_write = tree_bytes(root)

        for index in range(args.queries):
            query = vector_literal(build_vector(queries_rng, args.dimension))
            category = f"cat{index % category_count}"
            where = f" WHERE category = '{category}'" if index % 2 else ""
            sql = (
                "SELECT id,L2_DISTANCE(embedding,'"
                + query
                + "') AS d FROM bench"
                + where
                + " ORDER BY d LIMIT "
                + str(args.limit)
                + ";"
            )
            mode = args.search_mode
            if mode == "mixed":
                mode = "ann" if index % 2 else "exact"
            start = time.perf_counter()
            response = client.execute(sql, mode)
            query_times.append(time.perf_counter() - start)
            rows = response.get("rows", [])
            query_rows += len(rows)
            digest.update(json.dumps(rows, separators=(",", ":")).encode())
            current_rss = rss_mib(client.proc.pid)
            if current_rss is not None:
                rss_peak = max(rss_peak, current_rss)

        before_restart = time.perf_counter()
        client.shutdown()
        client = Client(args.binary, root)
        restart_seconds = time.perf_counter() - before_restart
        # Cheap correctness probes after recovery: first, last and one middle key.
        for row_id in (1, max(1, args.rows // 2), args.rows):
            response = client.execute(
                f"SELECT id FROM bench WHERE id = {row_id} LIMIT 1;"
            )
            if response.get("rows") != [[row_id]]:
                raise RuntimeError(f"recovery probe failed for id={row_id}: {response}")
        current_rss = rss_mib(client.proc.pid)
        if current_rss is not None:
            rss_peak = max(rss_peak, current_rss)
        return {
            "rows": args.rows,
            "dimension": args.dimension,
            "batch": args.batch,
            "queries": args.queries,
            "index": args.index,
            "search_mode": args.search_mode,
            "write_seconds": round(write_seconds, 6),
            "write_rows_per_sec": round(args.rows / write_seconds, 2),
            "query_p50_ms": round(percentile(query_times, 0.50) * 1000, 4),
            "query_p95_ms": round(percentile(query_times, 0.95) * 1000, 4),
            "query_p99_ms": round(percentile(query_times, 0.99) * 1000, 4),
            "query_mean_ms": round(statistics.mean(query_times) * 1000, 4),
            "query_total_seconds": round(sum(query_times), 6),
            "query_rows_returned": query_rows,
            "restart_seconds": round(restart_seconds, 6),
            "rss_peak_mib": round(rss_peak, 2) if rss_peak else None,
            "disk_bytes": tree_bytes(root),
            "disk_bytes_after_write": bytes_after_write,
            "result_digest": digest.hexdigest(),
        }
    finally:
        if client.proc.poll() is None:
            client.shutdown()
        if not args.data_dir:
            shutil.rmtree(root, ignore_errors=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--rows", type=int, default=100000)
    parser.add_argument("--dimension", type=int, default=128)
    parser.add_argument("--batch", type=int, default=500)
    parser.add_argument("--queries", type=int, default=200)
    parser.add_argument("--limit", type=int, default=10)
    parser.add_argument("--categories", type=int, default=32)
    parser.add_argument("--seed", type=int, default=20261113)
    parser.add_argument("--search-mode", choices=("exact", "ann", "mixed"), default="mixed")
    parser.add_argument("--index", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--data-dir")
    args = parser.parse_args()
    print(json.dumps(run(args), ensure_ascii=False, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
