"""Several tables sharing one budget: matrices are evicted, answers stay exact.

Each table wants its own dense matrix, and the budget must drop another table's
matrix to make room rather than exceed the container limit.  This pins that path
with a capacity small enough to force eviction, and checks every answer against an
oracle recomputed from the deterministic fixture (the test never holds the vectors,
so a memory failure here belongs to the database).

The scenario was measured end to end at eight tables of 15000 x 128 rows under a
simulated 256 MiB limit: 2-4 matrices held, no mismatches, peak RSS 154 MiB.
"""

import math
import os
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import minivector_store
import minivectordb
from minivectordb import Database

HAVE_NUMPY = minivectordb._numpy is not None
TABLES = 4
ROWS = 3000
DIM = 128
K = 5
TARGET = [0.5] * DIM
# A limit whose derived capacity is the 8 MiB floor, so four 2.9 MiB matrices
# cannot coexist and the eviction branch has to run.
LIMIT_MIB = 32


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def vector_for(table, key):
    return [f32(((key * 17 + table * 5 + column * 3) % 1000) / 1000.0)
            for column in range(DIM)]


def oracle(table):
    ranked = sorted((math.dist(vector_for(table, key), TARGET), key) for key in range(ROWS))
    return [key for _, key in ranked[:K]]


@unittest.skipUnless(HAVE_NUMPY, "numpy acceleration not installed")
class MultiTableBudgetTest(unittest.TestCase):
    def test_matrices_are_evicted_and_answers_stay_exact(self):
        limit = LIMIT_MIB * 2 ** 20
        with patch.object(minivector_store, "memory_limit_bytes", lambda: limit):
            db = Database(tempfile.mkdtemp(prefix="multi-budget-"))
            self.addCleanup(db.close)
            expected = {}
            for table in range(TABLES):
                name = f"t{table}"
                db.execute(f"CREATE TABLE {name} (id BIGINT PRIMARY KEY, g BIGINT, "
                           f"v VECTOR({DIM}))", "exact")
                batch = []
                for key in range(ROWS):
                    batch.append(("put", (name, key, [key, key % 7,
                                                      struct.pack(f"<{DIM}f",
                                                                  *vector_for(table, key))])))
                    if len(batch) == 3000:
                        db.mutate(batch)
                        batch = []
                if batch:
                    db.mutate(batch)
                expected[name] = oracle(table)

            self.assertIsNotNone(db.budget.capacity)
            one_matrix = ROWS * DIM * 8
            self.assertGreater(TABLES * one_matrix, db.budget.capacity,
                               "the fixture must not fit, otherwise nothing is evicted")

            literal = "[" + ",".join(repr(value) for value in TARGET) + "]"
            for attempt in (1, 2):
                for name, wanted in expected.items():
                    got = db.execute(f"SELECT id,L2_DISTANCE(v,'{literal}') AS d "
                                     f"FROM {name} ORDER BY d LIMIT {K}", "exact")["rows"]
                    self.assertEqual([row[0] for row in got], wanted,
                                     f"attempt {attempt}: {name} answer changed")
                self.assertLessEqual(db._memory_used(), db.budget.capacity,
                                     f"attempt {attempt}: budget exceeded")
            held = sum(1 for table in db.tables.values() if table.matrix is not None)
            self.assertLess(held, TABLES,
                            "at least one matrix must have been evicted under this budget")


if __name__ == "__main__":
    unittest.main()
