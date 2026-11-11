"""Shared memory budget and chunked kernels.

The matrix, the equality groups and the row cache are rebuildable structures
that used to be bounded independently, so their sum could exceed the container
limit.  These tests pin the shared accounting, the fallback when a structure does
not fit, the eviction of another table's matrix, and the chunked kernels that
bound temporaries during a gather.
"""

import math
import tempfile
import unittest

import minivectordb
import minivector_store
from minivector_store import MemoryBudget
from minivectordb import Database

HAVE_NUMPY = minivectordb._numpy is not None


class MemoryBudgetUnitTest(unittest.TestCase):
    def test_capacity_reserves_baseline_and_slack(self):
        limit = 2 << 30
        budget = MemoryBudget(limit)
        expected = max(8 << 20, int(limit * minivector_store.USABLE_FRACTION)
                       - minivector_store.BASELINE_BYTES)
        self.assertEqual(budget.capacity, expected)

    def test_unlimited_container_has_no_capacity(self):
        budget = MemoryBudget(None)
        self.assertIsNone(budget.capacity)
        self.assertTrue(budget.fits(10 << 30, 10 << 30))
        self.assertEqual(budget.share(0.4, 96 << 20, 16 << 20), 96 << 20)

    def test_small_limit_shrinks_shares(self):
        budget = MemoryBudget(256 << 20)
        self.assertLess(budget.share(0.4, 96 << 20, 16 << 20), 96 << 20)
        self.assertFalse(budget.fits(0, budget.capacity + 1))
        self.assertTrue(budget.fits(0, budget.capacity))


@unittest.skipUnless(HAVE_NUMPY, "numpy acceleration not installed")
class ChunkedKernelTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.db = Database(self.root.name)
        self.addCleanup(self.db.close)
        self.numpy = minivectordb._numpy
        rng = self.numpy.random.default_rng(3)
        self.values = rng.standard_normal((9000, 16)).astype(self.numpy.float32).astype(
            self.numpy.float64)
        self.query = rng.standard_normal(16).astype(self.numpy.float64)
        self.norms = self.numpy.einsum("ij,ij->i", self.values, self.values)

    def test_direct_kernel_matches_full_computation(self):
        numpy = self.numpy
        expected = numpy.einsum("ij,ij->i", self.values - self.query,
                                self.values - self.query)
        got = self.db._direct_squared(self.values, self.query)
        numpy.testing.assert_allclose(got, expected, rtol=1e-12, atol=0)
        indices = numpy.array([0, 4095, 4096, 8999], dtype=numpy.int64)
        subset = self.db._direct_squared(self.values, self.query, indices)
        numpy.testing.assert_allclose(subset, expected[indices], rtol=1e-12, atol=0)

    def test_identity_kernel_matches_full_computation(self):
        numpy = self.numpy
        qq = float(self.query @ self.query)
        expected = numpy.maximum(self.norms - 2.0 * (self.values @ self.query) + qq, 0.0)
        got = self.db._identity_squared(self.values, self.norms, self.query)
        numpy.testing.assert_allclose(got, expected, rtol=1e-12,
                                      atol=1e-9 * max(1.0, float(expected.max())))
        indices = numpy.arange(0, 9000, 37, dtype=numpy.int64)
        subset = self.db._identity_squared(self.values, self.norms, self.query, indices)
        numpy.testing.assert_allclose(subset, expected[indices], rtol=1e-12,
                                      atol=1e-9 * max(1.0, float(expected.max())))


@unittest.skipUnless(HAVE_NUMPY, "numpy acceleration not installed")
class BudgetEnforcementTest(unittest.TestCase):
    @staticmethod
    def value(key):
        """The value as it is actually stored: rounded through float32."""
        return float(minivectordb._numpy.float32((key % 97) / 97.0))

    def make_table(self, db, name, rows=3000, dimension=8):
        db.execute(f"CREATE TABLE {name} (id BIGINT PRIMARY KEY,"
                   f"g BIGINT,v VECTOR({dimension}))", "exact")
        batch = []
        for key in range(rows):
            vector = [self.value(key)] * dimension
            literal = "[" + ",".join(repr(v) for v in vector) + "]"
            batch.append(f"({key},{key % 5},'{literal}')")
            if len(batch) == 500:
                db.execute(f"INSERT INTO {name} VALUES " + ",".join(batch), "exact")
                batch = []
        if batch:
            db.execute(f"INSERT INTO {name} VALUES " + ",".join(batch), "exact")

    @staticmethod
    def oracle(rows, dimension, limit, target):
        ranked = sorted((math.dist([BudgetEnforcementTest.value(index)] * dimension, target),
                         index) for index in range(rows))
        return [key for _, key in ranked[:limit]]

    def test_refused_matrix_falls_back_but_stays_correct(self):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        db = Database(root.name)
        self.addCleanup(db.close)
        self.make_table(db, "t")
        db.budget.capacity = 1 << 10          # far below one matrix
        target = [0.5] * 8
        literal = "[" + ",".join(repr(v) for v in target) + "]"
        rows = db.execute(f"SELECT id,L2_DISTANCE(v,'{literal}') AS d FROM t "
                          f"ORDER BY d LIMIT 10", "exact")["rows"]
        self.assertEqual([row[0] for row in rows],
                         self.oracle(3000, 8, 10, target))
        self.assertIsNone(db.tables["t"].matrix,
                          "the matrix must be refused when the budget is tiny")

    def test_two_tables_do_not_each_keep_a_full_budget(self):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        db = Database(root.name)
        self.addCleanup(db.close)
        self.make_table(db, "a")
        self.make_table(db, "b")
        # Room for roughly one matrix of 3000 x 8 float64 (~192 KiB).
        db.budget.capacity = 400 << 10
        target = [0.5] * 8
        literal = "[" + ",".join(repr(v) for v in target) + "]"
        for name in ("a", "a", "b", "b", "a"):
            rows = db.execute(f"SELECT id,L2_DISTANCE(v,'{literal}') AS d FROM {name} "
                              f"ORDER BY d LIMIT 10", "exact")["rows"]
            self.assertEqual([row[0] for row in rows],
                             self.oracle(3000, 8, 10, target))
            self.assertLessEqual(db._memory_used(), db.budget.capacity)
        matrices = [table.matrix is not None for table in db.tables.values()]
        self.assertLessEqual(sum(matrices), 1,
                             "at most one table may hold a matrix under this budget")


if __name__ == "__main__":
    unittest.main()
