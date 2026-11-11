"""Numerical safety of the |v|^2 - 2v.q + |q|^2 identity (exact-rerank tests).

The identity loses absolute accuracy when the vectors are close: the computed
squared distance carries an error of about eps*(||v|| + ||q||)^2.  These tests
build the adversarial cases that a fixed coordinate threshold cannot cover -
float32 ULP neighbours at several magnitudes, duplicate vectors, a K/K+1 near
tie, and high-dimensional accumulation - and compare ids and distances against
math.dist on the stored float32 values.

Skipped when numpy is absent, because the accelerated path does not exist then.
"""

import math
import random
import struct
import tempfile
import unittest

import minivectordb
from minivectordb import Database

HAVE_NUMPY = minivectordb._numpy is not None
MAGNITUDES = (0.1, 1.0, 100.0, 999.0, 999.9)


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def step_f32(value, steps):
    """Move a positive float32 by whole ULPs."""
    bits = struct.unpack("<I", struct.pack("<f", f32(value)))[0]
    bits += steps
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def literal(vector):
    return "[" + ",".join(repr(float(value)) for value in vector) + "]"


def oracle(data, target, limit):
    ranked = sorted((math.dist(vector, target), key) for key, vector in data.items())
    return ranked[:limit]


@unittest.skipUnless(HAVE_NUMPY, "numpy acceleration not installed")
class NumericIdentityTest(unittest.TestCase):
    def build(self, rows, dimension):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        db = Database(root.name)
        self.addCleanup(db.close)
        db.execute(f"CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,"
                   f"v VECTOR({dimension}))", "exact")
        return db

    def insert(self, db, data):
        batch = []
        for key, vector in data.items():
            batch.append(f"({key},{key % 5},'{literal(vector)}')")
            if len(batch) == 500:
                db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
                batch = []
        if batch:
            db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")

    def check(self, db, data, target, limit, label):
        rows = db.execute(
            f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
            f"ORDER BY d LIMIT {limit}", "exact")["rows"]
        expected = oracle(data, target, limit)
        self.assertEqual([row[0] for row in rows], [key for _, key in expected],
                         f"{label}: ids differ")
        for row, (distance, _) in zip(rows, expected):
            self.assertLessEqual(abs(row[1] - distance), 1e-12,
                                 f"{label}: distance {row[1]!r} != {distance!r}")

    def test_ulp_neighbours_across_magnitudes(self):
        """Rows that differ by a few float32 ULPs must not tie spuriously."""
        for magnitude in MAGNITUDES:
            data = {}
            base = f32(magnitude)
            for index in range(3000):
                vector = [base] * 8
                # First 60 rows are ULP neighbours of the target, the rest are
                # spread out so the Top-K boundary sits inside the ULP cluster.
                if index < 60:
                    vector[0] = step_f32(base, index + 1)
                else:
                    vector[1] = f32(base * (1.0 + (index - 59) * 1e-3))
                data[index] = vector
            db = self.build(3000, 8)
            self.insert(db, data)
            self.check(db, data, [base] * 8, 10, f"magnitude {magnitude}")
            db.close()

    def test_duplicate_vectors_tie_on_primary_key(self):
        db = self.build(3000, 4)
        data = {}
        for index in range(3000):
            vector = [1.0, 2.0, 3.0, 4.0] if index % 2 else [0.0, 0.0, 0.0, 0.0]
            data[index] = vector
        self.insert(db, data)
        self.check(db, data, [1.0, 2.0, 3.0, 4.0], 10, "duplicates")
        db.close()

    def test_near_tie_at_the_k_boundary(self):
        """Distances that differ by ~1e-7 must still be ordered exactly."""
        db = self.build(3000, 4)
        base = f32(100.0)
        data = {}
        for index in range(3000):
            delta = 1e-7 * (index + 1)
            data[index] = [f32(base + delta), base, base, base]
        self.insert(db, data)
        self.check(db, data, [base, base, base, base], 20, "near tie")
        db.close()

    def test_high_dimension_accumulation(self):
        rng = random.Random(4242)
        for dimension in (128,):
            data = {}
            for index in range(3000):
                vector = [f32(rng.uniform(-200, 200)) for _ in range(dimension)]
                data[index] = vector
            target = [f32(rng.uniform(-200, 200)) for _ in range(dimension)]
            db = self.build(3000, dimension)
            self.insert(db, data)
            self.check(db, data, target, 10, f"dim {dimension}")
            db.close()

    def test_margin_bounds_the_identity_error(self):
        """The error bound used for the candidate window must actually hold."""
        rng = random.Random(99)
        numpy = minivectordb._numpy
        worst_ratio = 0.0
        for _ in range(200):
            dimension = rng.choice([2, 8, 128])
            magnitude = rng.choice(MAGNITUDES)
            vector = numpy.asarray([f32(rng.uniform(-magnitude, magnitude))
                                    for _ in range(dimension)], dtype=numpy.float64)
            target = numpy.asarray([f32(rng.uniform(-magnitude, magnitude))
                                    for _ in range(dimension)], dtype=numpy.float64)
            exact = float(numpy.einsum("i,i->", vector - target, vector - target))
            identity = (float(numpy.einsum("i,i->", vector, vector))
                        - 2.0 * float(vector @ target)
                        + float(target @ target))
            margin = minivectordb.Database.identity_error_margin(
                float(numpy.einsum("i,i->", vector, vector)),
                float(target @ target), dimension)
            error = abs(exact - identity)
            self.assertLessEqual(error, margin,
                                 f"error {error:.3e} exceeds margin {margin:.3e} "
                                 f"at magnitude {magnitude} dim {dimension}")
            if margin:
                worst_ratio = max(worst_ratio, error / margin)
        self.assertLess(worst_ratio, 1.0)

    def test_identity_still_beats_the_threshold_case(self):
        """A vector just below the identity threshold keeps exact ordering."""
        db = self.build(3000, 4)
        base = f32(999.0)
        data = {}
        for index in range(3000):
            vector = [base, base, base, base]
            vector[0] = step_f32(base, 1 + index % 8)
            vector[2] = f32(base + (index // 8) * 1e-2)
            data[index] = vector
        self.insert(db, data)
        self.check(db, data, [base] * 4, 10, "threshold neighbour")
        db.close()

    def test_identity_flip_regression_magnitude_30(self):
        """Regression for a real ranking failure found by search at magnitude 30.

        Rows are 1-4 ULP perturbations of a 128-dimensional vector with all
        coordinates 30.  The identity computed 0.0 for a large cluster while the
        true squared distances were 3.6e-12, so the old code returned key order
        instead of distance order.  With the error-bound rerank the ids match
        math.dist exactly.
        """
        numpy = minivectordb._numpy
        rng = numpy.random.default_rng(0)
        dimension, rows, k = 128, 3000, 10
        query = numpy.full(dimension, numpy.float32(30.0))
        vectors = numpy.repeat(query[None, :], rows, axis=0)
        for row in range(rows):
            for _ in range(int(rng.integers(1, 5))):
                column = int(rng.integers(0, dimension))
                bits = vectors[row, column].view(numpy.uint32)
                vectors[row, column] = numpy.uint32(
                    bits + int(rng.integers(1, 4))).view(numpy.float32)
        keys = rng.permutation(rows)

        db = self.build(rows, dimension)
        db.mutate(("put", ("t", int(keys[row]),
                           [int(keys[row]), row % 5, vectors[row].tobytes()]))
                  for row in range(rows))
        target = [float(value) for value in query]
        got = db.execute(f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t "
                         f"ORDER BY d LIMIT {k}", "exact")["rows"]
        ranked = sorted((math.dist([float(x) for x in vectors[row]],
                                   [float(x) for x in query]), int(keys[row]))
                        for row in range(rows))[:k]
        self.assertEqual([row[0] for row in got], [key for _, key in ranked])
        for row, (distance, _) in zip(got, ranked):
            self.assertLessEqual(abs(row[1] - distance), 1e-12)
        db.close()


if __name__ == "__main__":
    unittest.main()
