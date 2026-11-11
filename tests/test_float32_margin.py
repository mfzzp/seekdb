"""The rerank window must use the epsilon of the matrix dtype.

matrix_top_k stores float64 only while the table is small enough; above
MATRIX_MAX_BYTES it stores float32, and the identity kernel then computes in
float32.  A float64 error bound on a float32 computation is thousands of times too
narrow, so a row whose approximate value looks one rounding step worse than the
threshold would never be recomputed - and could be the true neighbour.

The first test pins the margin scale, the second reproduces the exclusion directly
on select_exact, and the third pins the dtype decision itself.
"""

import tempfile
import unittest
from unittest.mock import patch

try:                       # the accelerated path is optional
    import numpy
except ImportError:        # pragma: no cover - only in the pure-Python run
    numpy = None

import minivectordb
from minivectordb import Database

HAVE_NUMPY = minivectordb._numpy is not None


@unittest.skipUnless(HAVE_NUMPY, "numpy acceleration not installed")
class Float32MarginTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.db = Database(self.root.name)
        self.addCleanup(self.db.close)

    def test_margin_scales_with_the_dtype_epsilon(self):
        eps64 = float(numpy.finfo(numpy.float64).eps)
        eps32 = float(numpy.finfo(numpy.float32).eps)
        narrow = Database.identity_error_margin(128.0, 128.0, 128, eps64)
        wide = Database.identity_error_margin(128.0, 128.0, 128, eps32)
        self.assertGreater(wide, narrow)
        self.assertAlmostEqual(wide / narrow, eps32 / eps64, places=6)
        self.assertGreater(wide, 1e-5, "a float32 window must be far wider than 1e-9")
        self.assertLess(narrow, 1e-9, "the float64 window stays tiny")

    def test_a_float32_window_is_needed_to_keep_the_true_neighbour(self):
        # One dimension keeps the arithmetic obvious: true squared distances are
        # 0.0, 1e-9 and 2e-9, but the approximate values have the first two
        # swapped, as a float32 identity could produce.
        values = numpy.asarray([[0.0], [numpy.sqrt(1e-9)], [numpy.sqrt(2e-9)]])
        query = numpy.asarray([0.0])
        keys = numpy.asarray([1, 2, 3])
        approximate = numpy.asarray([1e-9, 0.0, 2e-9])

        narrow_rows, _ = self.db.select_exact(values, query, keys, approximate,
                                              None, 1, 1e-12)
        self.assertEqual(int(narrow_rows[0]), 1,
                         "with a float64-sized window the true nearest row is lost")

        wide_rows, wide_distances = self.db.select_exact(values, query, keys,
                                                         approximate, None, 1, 1e-3)
        self.assertEqual(int(wide_rows[0]), 0,
                         "a float32-sized window recovers the true nearest row")
        self.assertEqual(float(wide_distances[0]), 0.0)

    def test_build_matrix_uses_float32_only_past_the_cap(self):
        self.db.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(4))", "exact")
        self.db.execute("INSERT INTO t VALUES (1,'[0,0,0,0]'),(2,'[1,1,1,1]')", "exact")
        table = self.db.tables["t"]
        column = table.column("v")
        snapshot = table.index_snapshot()
        matrix = self.db.build_matrix(table, snapshot, column)
        self.assertEqual(matrix[2].dtype, numpy.float64)

        table.matrix = None
        original = minivectordb.MATRIX_MAX_BYTES
        minivectordb.MATRIX_MAX_BYTES = 1
        try:
            matrix = self.db.build_matrix(table, snapshot, column)
        finally:
            minivectordb.MATRIX_MAX_BYTES = original
        self.assertEqual(matrix[2].dtype, numpy.float32)

    def test_matrix_top_k_asks_for_the_matching_epsilon(self):
        """The plumbing, not just the formula: a float32 matrix must ask for eps32."""
        self.db.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(4))", "exact")
        self.db.execute("INSERT INTO t VALUES (1,'[0,0,0,0]'),(2,'[1,1,1,1]')", "exact")
        table = self.db.tables["t"]
        column = table.column("v")
        target = (0.0, 0.0, 0.0, 0.0)
        seen = []
        original_margin = Database.identity_error_margin

        def spy(norms_max, query_norm_sq, width, eps=2.220446049250313e-16):
            seen.append(eps)
            return original_margin(norms_max, query_norm_sq, width, eps)

        limited = minivectordb.MATRIX_MAX_BYTES
        with patch.object(Database, "identity_error_margin",
                                        staticmethod(spy)):
            self.db.matrix_top_k(table, column, target, 1)
            table.matrix = None
            minivectordb.MATRIX_MAX_BYTES = 1
            try:
                self.db.matrix_top_k(table, column, target, 1)
            finally:
                minivectordb.MATRIX_MAX_BYTES = limited

        self.assertEqual(len(seen), 2)
        self.assertAlmostEqual(seen[0], float(numpy.finfo(numpy.float64).eps))
        self.assertAlmostEqual(seen[1], float(numpy.finfo(numpy.float32).eps))


if __name__ == "__main__":
    unittest.main()
