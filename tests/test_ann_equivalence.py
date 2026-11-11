"""ann must agree with exact in every configuration.

The three ann routes are: the dense matrix (numpy, any dimension), the
first-coordinate branch-and-bound (<= 8 dimensions, vector index present, at most
50k rows) and the per-row scan.  The branch-and-bound is exact because it visits
rows in order of a lower bound on the distance and stops only when that bound
exceeds the current K-th distance, so every route must return the oracle answer.
"""

import math
import random
import struct
import tempfile
import unittest

import minivectordb
from minivectordb import Database

K = 10
CASES = [(4, 2500, True), (4, 2500, False), (8, 3000, True), (16, 2500, True),
         (16, 2500, False)]


def literal(vector):
    return "[" + ",".join(repr(float(value)) for value in vector) + "]"


class AnnEquivalenceTest(unittest.TestCase):
    def build(self, dim, rows, index, seed):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        db = Database(root.name)
        self.addCleanup(db.close)
        rng = random.Random(seed)
        db.execute(f"CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,"
                   f"v VECTOR({dim}))", "exact")
        data = {}
        batch = []
        for key in range(rows):
            vector = [struct.unpack("<f", struct.pack("<f", rng.uniform(-1, 1)))[0]
                      for _ in range(dim)]
            data[key] = vector
            batch.append(f"({key},{key % 20},'{literal(vector)}')")
            if len(batch) == 500:
                db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
                batch = []
        if batch:
            db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
        if index:
            db.execute("CREATE VECTOR INDEX vi ON t(v)", "exact")
        return db, data

    def check(self, dim, rows, index, seed=11):
        db, data = self.build(dim, rows, index, seed)
        rng = random.Random(seed + 1)
        for _ in range(3):
            target = [struct.unpack("<f", struct.pack("<f", rng.uniform(-1, 1)))[0]
                      for _ in range(dim)]
            sql = (f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
                   f"ORDER BY d LIMIT {K}")
            exact = db.execute(sql, "exact")["rows"]
            ann = db.execute(sql, "ann")["rows"]
            truth = [key for _, key in sorted(
                (math.dist(vector, target), key) for key, vector in data.items())[:K]]
            label = f"dim={dim} rows={rows} index={index}"
            self.assertEqual([row[0] for row in exact], truth, label)
            self.assertEqual([row[0] for row in ann], truth, label)
            self.assertEqual([row[1] for row in ann], [row[1] for row in exact], label)

    def test_all_configurations(self):
        for dim, rows, index in CASES:
            self.check(dim, rows, index)

    def test_filtered_ann_agrees_with_filtered_exact(self):
        db, data = self.build(8, 3000, True, seed=21)
        target = [0.0] * 8
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
               f"WHERE g = 7 ORDER BY d LIMIT {K}")
        self.assertEqual([row[0] for row in db.execute(sql, "ann")["rows"]],
                         [row[0] for row in db.execute(sql, "exact")["rows"]])
        truth = [key for _, key in sorted(
            (math.dist(vector, target), key)
            for key, vector in data.items() if key % 20 == 7)[:K]]
        self.assertEqual([row[0] for row in db.execute(sql, "ann")["rows"]], truth)

    def test_ann_after_delete_and_insert_matches_exact(self):
        db, data = self.build(8, 3000, True, seed=31)
        db.execute("DELETE FROM t WHERE id < 500", "exact")
        for key in range(500, 520):
            vector = [0.25] * 8
            data[key] = vector
        db.execute("UPDATE t SET v = '" + literal([0.25] * 8) + "' WHERE id < 520", "exact")
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal([0.25] * 8)}') AS d FROM t "
               f"ORDER BY d LIMIT {K}")
        self.assertEqual([row[0] for row in db.execute(sql, "ann")["rows"]],
                         [row[0] for row in db.execute(sql, "exact")["rows"]])


if __name__ == "__main__":
    unittest.main()
