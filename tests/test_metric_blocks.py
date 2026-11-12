"""Exact block pruning: independently ranked SQL results and cache lifetimes."""
import json
import math
import random
import struct
import tempfile
import unittest
from unittest.mock import patch

import minivectordb as module
from minivectordb import Database


def f32(values):
    return struct.unpack(f'<{len(values)}f', struct.pack(f'<{len(values)}f', *values))


class MetricFixture:
    rows = 6144
    width = 16

    def make(self, distribution='clustered', width=None):
        self.width = width or self.width
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        self.root = root.name
        self.db = Database(self.root)
        self.addCleanup(self.db.close)
        self.db.execute(f'CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(32),'
                        f'score BIGINT,v VECTOR({self.width}))', 'exact')
        rng = random.Random(847203)
        self.data = {}
        for key in range(self.rows):
            group = key % 8
            if distribution == 'isotropic':
                vector = [rng.gauss(0.0, 1.0) for _ in range(self.width)]
            elif distribution == 'ties':
                vector = [0.0] * self.width
                vector[group % self.width] = -1.0 if key % 2 else 1.0
            else:
                vector = [24.0 * group + rng.uniform(-0.05, 0.05)
                          for _ in range(self.width)]
            self.data[key] = (f'c{group % 2}', key % 100, f32(vector))
        # Reverse primary-key order so a traversal order cannot accidentally
        # satisfy the tie-breaking oracle.
        self.db.mutate(('put', ('t', key, [key, category, score,
            struct.pack(f'<{self.width}f', *vector)]))
            for key, (category, score, vector) in reversed(list(self.data.items())))
        self.db.execute('CREATE VECTOR INDEX vi ON t(v)', 'exact')
        self.query = f32([72.0] * self.width)

    def check_query(self, query=None, where='', keep=lambda row: True, limit=10,
                    modes=('exact', 'ann'), repeats=1):
        query = f32(query if query is not None else self.query)
        expected = sorted((math.dist(row[2], query), key)
                          for key, row in self.data.items() if keep(row))[:limit]
        sql = ("SELECT id,L2_DISTANCE(v,'" + json.dumps(query) + "') AS d "
               f'FROM t {where} ORDER BY d LIMIT {limit}')
        for mode in modes:
            for _ in range(repeats):
                actual = self.db.execute(sql, mode)['rows']
                self.assertEqual([row[0] for row in actual], [key for _, key in expected])
                for row, (distance, _) in zip(actual, expected):
                    self.assertTrue(math.isclose(row[1], distance, rel_tol=2e-12,
                                                 abs_tol=1e-300), (row, distance))
        return actual

    def warm(self):
        self.check_query(modes=('exact',), repeats=4)

    def replace(self, key, category=None, score=None, vector=None):
        old_category, old_score, old_vector = self.data[key]
        value = (old_category if category is None else category,
                 old_score if score is None else score,
                 old_vector if vector is None else f32(vector))
        sql = (f"UPDATE t SET category='{value[0]}',score={value[1]},"
               "v='" + json.dumps(value[2]) + f"' WHERE id={key}")
        self.db.execute(sql, 'exact')
        self.data[key] = value


@unittest.skipUnless(module._numpy is not None, 'numpy acceleration not installed')
class MetricBlocksTest(MetricFixture, unittest.TestCase):
    def setUp(self):
        # Keep unit-test fixtures small while exercising the index. Its actual
        # production size threshold is checked independently below.
        self.production_gate = module.METRIC_MIN_COORDINATES
        gate = patch.object(module, 'METRIC_MIN_COORDINATES', 0)
        gate.start()
        self.addCleanup(gate.stop)

    def test_production_size_gate_skips_small_matrix(self):
        self.make()
        with patch.object(module, 'METRIC_MIN_COORDINATES', self.production_gate):
            self.warm()
        self.assertIsNone(self.db.table('t').metric_blocks)

    def test_clustered_prunes_rows_and_matches_exhaustive(self):
        self.make()
        self.warm()
        blocks = self.db.table('t').metric_blocks
        self.assertIsInstance(blocks, dict, 'the clustered workload must exercise the new index')
        self.assertGreater(blocks['last_pruned_blocks'], 0)
        self.assertLess(blocks['last_scanned_rows'], len(self.data))
        self.check_query()

    def test_isotropic_data_rejects_unhelpful_index(self):
        self.make('isotropic')
        self.query = f32([0.15] * self.width)
        self.warm()
        self.assertIs(self.db.table('t').metric_blocks, False)
        self.check_query()

    def test_where_equality_range_and_preserve_hard_filters(self):
        self.make()
        self.warm()
        self.check_query(where="WHERE category='c0'", keep=lambda row: row[0] == 'c0')
        self.check_query(where='WHERE score>=50', keep=lambda row: row[1] >= 50)
        self.check_query(where="WHERE category='c0' AND score>=20 AND score<80",
                         keep=lambda row: row[0] == 'c0' and 20 <= row[1] < 80)
        self.check_query(where="WHERE category='absent'", keep=lambda row: False)

    def test_equal_distances_use_primary_key_across_blocks(self):
        self.make('ties', width=8)
        self.query = f32([0.0] * self.width)
        self.warm()
        self.check_query(limit=300)

    def test_deletion_bits_and_deleted_nearest_rows(self):
        self.make()
        self.warm()
        nearest = sorted(self.data, key=lambda key: (math.dist(self.data[key][2], self.query), key))[:15]
        for key in set(nearest + [0, 1, 6, 7, 8, 15, 77]):
            self.db.execute(f'DELETE FROM t WHERE id={key}', 'exact')
            del self.data[key]
        self.check_query(repeats=4)
        self.check_query(where="WHERE category='c1'", keep=lambda row: row[0] == 'c1')

    def test_update_insert_category_change_and_reinsert(self):
        self.make()
        self.warm()
        self.replace(0, vector=self.query, category='c1', score=99)
        key = self.rows + 7
        value = ('c0', 88, self.query)
        self.db.execute(f"INSERT INTO t VALUES ({key},'c0',88,'" + json.dumps(self.query) + "')", 'exact')
        self.data[key] = value
        self.check_query(repeats=4)
        self.check_query(where="WHERE category='c0'", keep=lambda row: row[0] == 'c0')
        self.db.execute(f'DELETE FROM t WHERE id={key}', 'exact')
        self.db.execute(f"INSERT INTO t VALUES ({key},'c1',1,'" + json.dumps(self.query) + "')", 'exact')
        self.data[key] = ('c1', 1, self.query)
        self.check_query(where="WHERE category='c1'", keep=lambda row: row[0] == 'c1', repeats=4)

    def test_rollback_visibility_and_restart(self):
        self.make()
        self.warm()
        original = self.data.copy()
        self.db.execute('BEGIN', 'exact')
        self.replace(0, vector=self.query, category='new')
        self.check_query()
        self.db.execute('ROLLBACK', 'exact')
        self.data = original
        self.check_query(repeats=4)
        self.db.close()
        self.db = Database(self.root)
        self.addCleanup(self.db.close)
        self.check_query(repeats=4)

    def test_small_budget_refuses_index_and_remains_exact(self):
        self.make()
        self.db.cache.clear()
        self.db.cache_bytes = 0
        self.db.budget.capacity = 1024
        self.check_query(repeats=4)
        self.assertIsNone(self.db.table('t').matrix)
        self.assertFalse(self.db.table('t').metric_blocks)

    def test_index_reservation_failure_does_not_keep_partial_cache(self):
        self.make()
        table = self.db.table('t')
        self.db.build_matrix(table, table.index_snapshot(), table.column('v'))
        with patch.object(self.db, '_reserve', return_value=False):
            result = self.db._build_metric_blocks(table, table.matrix)
        self.assertIs(result, False)
        self.assertFalse(table.metric_blocks)
        self.check_query(repeats=4)

    def test_zero_large_and_above_row_count_limits(self):
        self.make()
        self.warm()
        for limit in (0, 1000, self.rows + 10):
            self.check_query(limit=limit, modes=('exact',))
        self.check_query()

    def test_float32_large_small_and_near_duplicate_coordinates(self):
        self.make()
        for scale in (1e-30, 1e20):
            for key in range(32):
                value = [scale * (1.0 + ((key + axis) % 7) * 2e-7)
                         for axis in range(self.width)]
                self.replace(key, vector=value)
            target = f32([scale] * self.width)
            # Force the same float32 matrix representation selected when the
            # usual float64 cache is too large; raw stored values are unchanged.
            self.db.table('t').matrix = None
            self.db.table('t').metric_blocks = None
            with patch.object(module, 'MATRIX_MAX_BYTES', self.rows * self.width * 4):
                self.check_query(query=target, repeats=4)

    def test_large_query_cannot_overflow_small_float32_matrix(self):
        self.make()
        with patch.object(module, 'MATRIX_MAX_BYTES', self.rows * self.width * 4):
            self.check_query(query=[1e30] * self.width, repeats=4)


class MetricWithoutNumpyTest(MetricFixture, unittest.TestCase):
    rows = 96
    width = 4

    def test_optional_numpy_absence_keeps_correct_sql_path(self):
        self.make()
        with patch.object(module, '_numpy', None):
            self.check_query()
            self.check_query(where="WHERE category='c1'", keep=lambda row: row[0] == 'c1')


if __name__ == '__main__':
    unittest.main()
