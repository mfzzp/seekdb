"""Verify the optional dense-matrix TopK path against a brute-force oracle.

The test is skipped when numpy is not importable, so the same suite passes in
the pure-Python deployment as well.
"""

import math
import random
import struct
import tempfile
import unittest

import minivectordb
from minivectordb import Database

HAVE_NUMPY = minivectordb._numpy is not None


def f32(vector):
    """Round through float32 so the oracle sees stored values, not inputs."""
    return list(struct.unpack(f"<{len(vector)}f",
                              struct.pack(f"<{len(vector)}f", *vector)))


def oracle(data, target, limit, keep=None):
    rows = [(math.dist(vec, target), key) for key, vec in data.items()
            if keep is None or keep(key)]
    rows.sort()
    return [[key, distance] for distance, key in rows[:limit]]


@unittest.skipUnless(HAVE_NUMPY, "numpy acceleration not installed")
class MatrixPathTest(unittest.TestCase):
    def build(self, count=3000, dimension=8, seed=7):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        db = Database(root.name)
        self.addCleanup(db.close)
        db.execute(f'CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,'
                   f'v VECTOR({dimension}))', 'exact')
        rng = random.Random(seed)
        data = {}
        batch = []
        for key in range(count):
            vector = f32([round(rng.gauss(0, 1), 6) for _ in range(dimension)])
            data[key] = vector
            literal = "[" + ",".join(str(value) for value in vector) + "]"
            batch.append(f"({key},{key % 7},'{literal}')")
            if len(batch) == 500:
                db.execute('INSERT INTO t VALUES ' + ','.join(batch), 'exact')
                batch = []
        if batch:
            db.execute('INSERT INTO t VALUES ' + ','.join(batch), 'exact')
        db.execute('CREATE VECTOR INDEX vi ON t(v)', 'exact')
        return db, data

    def test_matrix_matches_oracle_and_tracks_mutations(self):
        db, data = self.build()
        target = [0.25] * 8
        literal = "[" + ",".join(str(value) for value in target) + "]"
        sql = f"SELECT id,L2_DISTANCE(v,'{literal}') AS d FROM t ORDER BY d LIMIT 10"
        result = db.execute(sql, 'exact')['rows']
        self.assertIsNotNone(db.tables['t'].matrix, "dense matrix should be built")
        expected = oracle(data, target, 10)
        self.assertEqual([row[0] for row in result], [row[0] for row in expected])
        for got, want in zip(result, expected):
            self.assertLessEqual(abs(got[1] - want[1]), 1e-9)
        self.assertEqual(db.execute(sql, 'ann')['rows'], result,
                         "exact and ann must agree")

        db.execute('DELETE FROM t WHERE id < 900', 'exact')
        survivors = {key: vec for key, vec in data.items() if key >= 900}
        result = db.execute(sql, 'exact')['rows']
        expected = oracle(survivors, target, 10)
        self.assertEqual([row[0] for row in result], [row[0] for row in expected])

        db.execute("UPDATE t SET v = '[9,9,9,9,9,9,9,9]' WHERE id < 1000", 'exact')
        for key in survivors:
            if key < 1000:
                survivors[key] = [9.0] * 8
        result = db.execute(sql, 'exact')['rows']
        expected = oracle(survivors, target, 10)
        self.assertEqual([row[0] for row in result], [row[0] for row in expected])

        db.execute('BEGIN', 'exact')
        db.execute("INSERT INTO t VALUES (999999,0,'[0,0,0,0,0,0,0,0]')", 'exact')
        pending = db.execute(sql, 'exact')['rows']
        self.assertEqual(pending[0][0], 999999, "transaction overlay must win")
        self.assertEqual([row[0] for row in pending[1:]], [row[0] for row in result][:9])
        db.execute('ROLLBACK', 'exact')
        self.assertEqual(db.execute(sql, 'exact')['rows'], result)

    def test_large_coordinates_keep_the_direct_kernel(self):
        """The BLAS identity must not be used where cancellation would bite."""
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        db = Database(root.name)
        self.addCleanup(db.close)
        db.execute('CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2))', 'exact')
        rows = []
        for key in range(3000):
            literal = '[16777216,0]' if key < 5 else '[0,16777216]'
            rows.append(f"({key},'{literal}')")
            if len(rows) == 1000:
                db.execute('INSERT INTO t VALUES ' + ','.join(rows), 'exact')
                rows = []
        sql = "SELECT id,L2_DISTANCE(v,'[16777216,0]') AS d FROM t ORDER BY d LIMIT 5"
        result = db.execute(sql, 'exact')['rows']
        self.assertEqual([row[0] for row in result], [0, 1, 2, 3, 4])
        for row in result:
            self.assertEqual(row[1], 0.0, "float32 boundary must give exact zero")
        # 16777217 rounds to 16777216 in float32, so this row is one unit away.
        db.execute("INSERT INTO t VALUES (9999,'[16777217,1]')", 'exact')
        result = db.execute("SELECT id,L2_DISTANCE(v,'[16777216,0]') AS d FROM t "
                            "ORDER BY d LIMIT 6", 'exact')['rows']
        self.assertEqual([row[0] for row in result], [0, 1, 2, 3, 4, 9999])
        self.assertAlmostEqual(result[-1][1], 1.0, places=6)

    def test_filtered_group_path_matches_oracle(self):
        db, data = self.build(count=3000, dimension=4, seed=11)
        target = [0.5, -0.5, 0.0, 1.0]
        literal = "[" + ",".join(str(value) for value in target) + "]"
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal}') AS d FROM t "
               f"WHERE g = 3 ORDER BY d LIMIT 10")
        result = db.execute(sql, 'exact')['rows']
        expected = sorted((math.dist(vec, target), key)
                          for key, vec in data.items() if key % 7 == 3)[:10]
        self.assertEqual([row[0] for row in result], [key for _, key in expected])
        for got, (distance, _) in zip(result, expected):
            self.assertLessEqual(abs(got[1] - distance), 1e-9)
        self.assertIsNotNone(db.tables['t'].blocks, "equality groups must be cached")
        empty = db.execute("SELECT id,L2_DISTANCE(v,'[0,0,0,0]') AS d FROM t "
                           "WHERE g = 99 ORDER BY d LIMIT 5", 'exact')['rows']
        self.assertEqual(empty, [])
        self.assertEqual(db.execute("SELECT id,L2_DISTANCE(v,'[0,0,0,0]') AS d FROM t "
                                    "WHERE g = 3 ORDER BY d LIMIT 0", 'exact')['rows'], [])

        db.execute('DELETE FROM t WHERE id < 900', 'exact')
        survivors = {key: vec for key, vec in data.items() if key >= 900}
        result = db.execute(sql, 'exact')['rows']
        expected = sorted((math.dist(vec, target), key)
                          for key, vec in survivors.items() if key % 7 == 3)[:10]
        self.assertEqual([row[0] for row in result], [key for _, key in expected])

    def test_limit_and_ties(self):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        db = Database(root.name)
        self.addCleanup(db.close)
        db.execute('CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2))', 'exact')
        rows = []
        for key in range(4000):
            literal = '[1,0]' if key % 2 else '[0,1]'
            rows.append(f"({key},'{literal}')")
            if len(rows) == 1000:
                db.execute('INSERT INTO t VALUES ' + ','.join(rows), 'exact')
                rows = []
        sql = "SELECT id,L2_DISTANCE(v,'[0,1]') AS d FROM t ORDER BY d LIMIT 10"
        result = db.execute(sql, 'exact')['rows']
        self.assertEqual([row[0] for row in result], [0, 2, 4, 6, 8, 10, 12, 14, 16, 18])
        self.assertEqual(db.execute(sql, 'ann')['rows'], result)
        self.assertEqual(db.execute("SELECT id,L2_DISTANCE(v,'[0,1]') AS d FROM t "
                                    "ORDER BY d LIMIT 0", 'exact')['rows'], [])


if __name__ == '__main__':
    unittest.main()
