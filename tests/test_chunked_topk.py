"""Chunked exact TopK: the memory-bounded fallback must stay exact.

The chunked path is what runs when the shared budget refuses a dense matrix (and
it is the strategy measured against always-matrix and always-scan).  It decodes
rows in chunks and only keeps the running K best, so its result must be
identical to the matrix path and to a math.dist oracle.
"""

import math
import tempfile
import unittest
from unittest.mock import patch

import minivectordb
from minivectordb import Database

HAVE_NUMPY = minivectordb._numpy is not None
ROWS = 9000
DIMENSION = 8


def f32(value):
    import struct
    return struct.unpack("<f", struct.pack("<f", value))[0]


def literal(vector):
    return "[" + ",".join(repr(float(value)) for value in vector) + "]"


@unittest.skipUnless(HAVE_NUMPY, "numpy acceleration not installed")
class ChunkedTopKTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.db = Database(self.root.name)
        self.addCleanup(self.db.close)
        self.db.execute(f"CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,"
                        f"v VECTOR({DIMENSION}))", "exact")
        self.data = {}
        batch = []
        for key in range(ROWS):
            vector = [f32(((key * 31 + column * 17) % 1000) / 1000.0)
                      for column in range(DIMENSION)]
            self.data[key] = vector
            batch.append(f"({key},{key % 5},'{literal(vector)}')")
            if len(batch) == 500:
                self.db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
                batch = []
        if batch:
            self.db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
        self.db.execute("CREATE VECTOR INDEX vi ON t(v)", "exact")

    def oracle(self, target, limit, keep=None):
        ranked = sorted((math.dist(vector, target), key)
                        for key, vector in self.data.items()
                        if keep is None or keep(key))
        return [key for _, key in ranked[:limit]]

    def run_query(self, sql):
        return self.db.execute(sql, "exact")["rows"]

    def test_chunked_matches_matrix_and_oracle(self):
        target = [0.5] * DIMENSION
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
               f"ORDER BY d LIMIT 10")
        matrix_rows = self.run_query(sql)
        self.assertIsNotNone(self.db.tables["t"].matrix)
        with patch.object(Database, "build_matrix", lambda *args, **kwargs: None):
            self.db.tables["t"].matrix = None
            chunked_rows = self.run_query(sql)
        expected = self.oracle(target, 10)
        self.assertEqual([row[0] for row in matrix_rows], expected)
        self.assertEqual([row[0] for row in chunked_rows], expected)
        for got, want in zip(chunked_rows, matrix_rows):
            self.assertAlmostEqual(got[1], want[1], places=9)

    def test_chunked_filtered_matches_oracle(self):
        target = [0.25] * DIMENSION
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
               f"WHERE g = 3 ORDER BY d LIMIT 10")
        with patch.object(Database, "build_matrix", lambda *args, **kwargs: None):
            rows = self.run_query(sql)
        expected = self.oracle(target, 10, keep=lambda key: key % 5 == 3)
        self.assertEqual([row[0] for row in rows], expected)
        for got, (distance, _) in zip(rows, sorted(
                (math.dist(self.data[key], target), key) for key in expected)):
            self.assertAlmostEqual(got[1], distance, places=9)

    def test_chunked_after_deletes_and_updates(self):
        self.db.execute("DELETE FROM t WHERE id < 300", "exact")
        for key in range(300, 500):
            self.data[key] = [0.125] * DIMENSION
        self.db.execute("UPDATE t SET v = '" + literal([0.125] * DIMENSION) +
                        "' WHERE id < 500", "exact")
        target = [0.125] * DIMENSION
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
               f"ORDER BY d LIMIT 5")
        with patch.object(Database, "build_matrix", lambda *args, **kwargs: None):
            rows = self.run_query(sql)
        expected = self.oracle(target, 5, keep=lambda key: key >= 300)
        self.assertEqual([row[0] for row in rows], expected)

    def test_chunked_limit_larger_than_table(self):
        target = [0.9] * DIMENSION
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
               f"ORDER BY d LIMIT 20000")
        with patch.object(Database, "build_matrix", lambda *args, **kwargs: None):
            rows = self.run_query(sql)
        self.assertEqual(len(rows), ROWS)

    def test_budget_refusal_uses_chunked_path(self):
        """A budget smaller than one matrix must still answer exactly."""
        target = [0.5] * DIMENSION
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
               f"ORDER BY d LIMIT 10")
        self.db.budget.capacity = 1 << 10
        self.db.tables["t"].matrix = None
        rows = self.run_query(sql)
        self.assertIsNone(self.db.tables["t"].matrix)
        self.assertEqual([row[0] for row in rows], self.oracle(target, 10))


if __name__ == "__main__":
    unittest.main()
