"""Mixed workload against a model: inserts, updates, deletes, transactions, restarts.

Forty rounds are enough to catch a lost update, a resurrected rolled-back row, a
count drift after restart and an unbounded heap: the row count is compared with a
model every round and the TopK ids every tenth round.  The long-running variant
(300 rounds) lives in scripts/soak.py; this keeps the model agreement in the
regular suite without making it slow.

The model ranks float32-rounded vectors because that is what the store keeps;
ranking the original Python floats would break near ties differently and produce
false failures.
"""

import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

SOURCE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIM = 16
K = 5
ROUNDS = 40


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def vector_for(key):
    return [f32(((key * 37 + index * 11) % 1000) / 1000.0) for index in range(DIM)]


def literal(vector):
    return "[" + ",".join(repr(value) for value in vector) + "]"


class Server:
    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.sequence = 0
        self.process = None
        self.start()

    def start(self):
        self.process = subprocess.Popen(
            [sys.executable, os.path.join(SOURCE, "minivectordb.py"),
             "--data-dir", self.data_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.process.stdout.readline()

    def ask(self, sql, mode="exact"):
        self.sequence += 1
        self.process.stdin.write((json.dumps(
            {"id": self.sequence, "op": "execute", "sql": sql,
             "search_mode": mode}) + "\n").encode())
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError(f"no answer to {sql!r}")
        return json.loads(line)

    def rss_mib(self):
        try:
            with open(f"/proc/{self.process.pid}/status", encoding="ascii") as handle:
                for line in handle:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1]) // 1024
        except OSError:
            return None
        return None

    def stop(self):
        self.sequence += 1
        self.process.stdin.write((json.dumps(
            {"id": self.sequence, "op": "shutdown"}) + "\n").encode())
        self.process.stdin.flush()
        self.process.stdout.readline()
        self.process.wait(timeout=60)
        for stream in (self.process.stdin, self.process.stdout):
            try:
                stream.close()
            except OSError:
                pass


def expected_top(model, target, limit, keep=None):
    ranked = sorted((math.dist(row["v"], target), key)
                    for key, row in model.items() if keep is None or keep(row))
    return [key for _, key in ranked[:limit]]


class SoakTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.data = os.path.join(self.root.name, "data")
        self.server = Server(self.data)
        self.addCleanup(shutil.rmtree, self.root.name, True)
        self.model = {}
        self.next_id = 0
        self.server.ask(f"CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,v VECTOR({DIM}));")
        self.server.ask("CREATE VECTOR INDEX vi ON t(v);")

    def tearDown(self):
        try:
            self.server.stop()
        except OSError:
            pass

    def test_mixed_workload_matches_model_and_stays_bounded(self):
        rss = []
        for round_index in range(1, ROUNDS + 1):
            rows = []
            for _ in range(15):
                key = self.next_id
                self.next_id += 1
                self.model[key] = {"g": key % 30, "v": vector_for(key)}
                rows.append(f"({key},{self.model[key]['g']},"
                            f"'{literal(self.model[key]['v'])}')")
            response = self.server.ask("INSERT INTO t VALUES " + ",".join(rows) + ";")
            self.assertEqual(response.get("affected_rows"), len(rows))

            for key in list(self.model)[::max(1, len(self.model) // 4)][:4]:
                wanted = (self.model[key]["g"] + 1) % 30
                if wanted == self.model[key]["g"]:
                    continue
                response = self.server.ask(f"UPDATE t SET g = {wanted} WHERE id = {key};")
                self.assertEqual(response.get("affected_rows"), 1, f"update {key}")
                self.model[key]["g"] = wanted

            if len(self.model) > 120:
                for key in list(self.model)[:4]:
                    response = self.server.ask(f"DELETE FROM t WHERE id = {key};")
                    self.assertEqual(response.get("affected_rows"), 1, f"delete {key}")
                    del self.model[key]

            if round_index % 8 == 0:
                self.server.ask("BEGIN;")
                ghost = self.next_id
                self.next_id += 1
                self.server.ask(f"INSERT INTO t VALUES ({ghost},{ghost % 30},"
                                f"'{literal(vector_for(ghost))}');")
                self.server.ask("ROLLBACK;")
                self.assertEqual(self.server.ask(
                    f"SELECT id FROM t WHERE id = {ghost};")["rows"], [])

            self.assertEqual(len(self.server.ask("SELECT id FROM t;")["rows"]),
                             len(self.model), f"count after round {round_index}")

            if round_index % 10 == 0:
                target = [0.5] * DIM
                plain = self.server.ask(
                    f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
                    f"ORDER BY d LIMIT {K};")
                self.assertEqual([row[0] for row in plain["rows"]],
                                 expected_top(self.model, target, K))
                ranged = self.server.ask(
                    f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
                    f"WHERE g < 5 ORDER BY d LIMIT {K};")
                self.assertEqual([row[0] for row in ranged["rows"]],
                                 expected_top(self.model, target, K,
                                              keep=lambda row: row["g"] < 5))

            if round_index == ROUNDS // 2:
                self.server.stop()
                self.server.start()
                self.assertEqual(len(self.server.ask("SELECT id FROM t;")["rows"]),
                                 len(self.model), "count after restart")

            sample = self.server.rss_mib()
            if sample is not None:
                rss.append(sample)

        if len(rss) >= 10:
            warm = sorted(rss[3:len(rss) // 2])[len(rss[3:len(rss) // 2]) // 2]
            tail = sorted(rss[len(rss) // 2:])[len(rss[len(rss) // 2:]) // 2]
            self.assertLess(tail - warm, 32,
                            f"resident memory grew from {warm} to {tail} MiB: {rss}")


if __name__ == "__main__":
    unittest.main()
