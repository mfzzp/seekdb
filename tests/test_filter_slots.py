"""Scalar predicates are resolved to candidate slots before any vector work.

Covers the selectivity sweep the review asked for (0.1%, 1%, 10%, 50%, 100%),
every comparison operator on BIGINT and VARCHAR, AND chains, deletes, and the
2^53 boundary where a float conversion would lose integer precision.
"""

import math
import struct
import tempfile
import unittest

import minivectordb
from minivectordb import Database

HAVE_NUMPY = minivectordb._numpy is not None
ROWS = 9000
DIMENSION = 8
BIG = 2 ** 53


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def literal(vector):
    return "[" + ",".join(repr(float(value)) for value in vector) + "]"


@unittest.skipUnless(HAVE_NUMPY, "numpy acceleration not installed")
class FilterSlotsTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.db = Database(self.root.name)
        self.addCleanup(self.db.close)
        self.db.execute(f"CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,"
                        f"s VARCHAR(8),v VECTOR({DIMENSION}))", "exact")
        self.data = {}
        batch = []
        for key in range(ROWS):
            vector = [f32(((key * 13 + column * 7) % 200) / 200.0)
                      for column in range(DIMENSION)]
            self.data[key] = (key % 1000, f"v{key % 20}", vector)
            g, s, _ = self.data[key]
            batch.append(f"({key},{g},'{s}','{literal(vector)}')")
            if len(batch) == 500:
                self.db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
                batch = []
        if batch:
            self.db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
        self.db.execute("CREATE VECTOR INDEX vi ON t(v)", "exact")

    def oracle(self, target, limit, keep):
        ranked = sorted((math.dist(self.data[key][2], target), key)
                        for key in self.data if keep(key))
        return [key for _, key in ranked[:limit]]

    def check(self, where, keep, limit=10, label=""):
        target = [0.5] * DIMENSION
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
               f"WHERE {where} ORDER BY d LIMIT {limit}")
        rows = self.db.execute(sql, "exact")["rows"]
        expected = self.oracle(target, limit, keep)
        self.assertEqual([row[0] for row in rows], expected, f"{label}: {where}")
        for got, key in zip(rows, expected):
            self.assertAlmostEqual(got[1], math.dist(self.data[key][2], target), places=9)

    def test_selectivity_sweep_bigint(self):
        for fraction in (0.001, 0.01, 0.10, 0.50, 1.00):
            threshold = max(1, int(1000 * fraction))
            self.check(f"g < {threshold}",
                       lambda key, t=threshold: self.data[key][0] < t,
                       label=f"{fraction:.3f}")
        self.assertIsNotNone(self.db.tables["t"].scalar_cache,
                             "the scalar index should be cached after use")

    def test_all_comparison_operators(self):
        # The task only requires = < <= > >=; "<>" is outside the grammar.
        cases = [
            ("g = 7", lambda key: self.data[key][0] == 7),
            ("g < 7", lambda key: self.data[key][0] < 7),
            ("g <= 7", lambda key: self.data[key][0] <= 7),
            ("g > 7", lambda key: self.data[key][0] > 7),
            ("g >= 7", lambda key: self.data[key][0] >= 7),
        ]
        for where, keep in cases:
            self.check(where, keep, label="bigint")

    def test_varchar_equality_and_ranges(self):
        cases = [
            ("s = 'v3'", lambda key: self.data[key][1] == "v3"),
            ("s < 'v3'", lambda key: self.data[key][1] < "v3"),
            ("s >= 'v17'", lambda key: self.data[key][1] >= "v17"),
            ("s = 'missing'", lambda key: False),
        ]
        for where, keep in cases:
            self.check(where, keep, label="varchar")

    def test_and_chain(self):
        self.check("g >= 100 AND g < 110",
                   lambda key: 100 <= self.data[key][0] < 110, label="and-bigint")
        self.check("s = 'v5' AND g < 100",
                   lambda key: self.data[key][1] == "v5" and self.data[key][0] < 100,
                   label="and-mixed")

    def test_bigint_beyond_float64_precision(self):
        """2^53+1 and 2^53+2 must stay distinct; a float comparison would not."""
        keys = []
        batch = []
        for offset in range(3000):
            key = 10_000_000 + offset
            g = BIG + offset
            vector = [0.5] * DIMENSION
            self.data[key] = (g, "big", vector)
            keys.append(key)
            batch.append(f"({key},{g},'big','{literal(vector)}')")
            if len(batch) == 500:
                self.db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
                batch = []
        if batch:
            self.db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
        self.check(f"g = {BIG + 1}", lambda key: self.data[key][0] == BIG + 1, label="2^53")
        self.check(f"g > {BIG}", lambda key: self.data[key][0] > BIG, label="2^53 range")

    def test_filter_after_deletes_and_inside_transaction(self):
        self.db.execute("DELETE FROM t WHERE id < 1000", "exact")
        self.check("g < 10", lambda key: key >= 1000 and self.data[key][0] < 10,
                   label="after delete")
        self.db.execute("BEGIN", "exact")
        self.db.execute("UPDATE t SET g = 5 WHERE id < 1200", "exact")
        for key in range(1000, 1200):
            self.data[key] = (5, self.data[key][1], self.data[key][2])
        self.check("g = 5", lambda key: key >= 1000 and self.data[key][0] == 5,
                   label="txn overlay")
        self.db.execute("ROLLBACK", "exact")
        for key in range(1000, 1200):
            self.data[key] = (key % 1000, self.data[key][1], self.data[key][2])
        self.check("g = 5", lambda key: key >= 1000 and self.data[key][0] == 5,
                   label="after rollback")


if __name__ == "__main__":
    unittest.main()
