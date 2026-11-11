"""Mutation visibility, packed deletion masks and bounded matrix reuse."""
import math
import struct
import tempfile
import unittest
from unittest.mock import patch

import minivectordb as module
from minivectordb import Database


@unittest.skipUnless(module._numpy is not None, "numpy acceleration not installed")
class IncrementalMatrixTest(unittest.TestCase):
    def setUp(self):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        self.db = Database(root.name)
        self.addCleanup(self.db.close)
        self.db.execute('CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(4))', 'exact')
        self.data = {i: (i % 3, [float(i % 11), float(i % 7), 0.0, 1.0]) for i in range(3000)}
        self.db.mutate(('put', ('t', i, [i, g, struct.pack('<4f', *v)]))
                       for i, (g, v) in self.data.items())
        self.sql = "SELECT id,L2_DISTANCE(v,'[0,0,0,1]') AS d FROM t {where} ORDER BY d LIMIT 12"

    def check(self, where='', keep=lambda g: True):
        wanted = sorted((math.dist(v, [0, 0, 0, 1]), i)
                        for i, (g, v) in self.data.items() if keep(g))[:12]
        for mode in ['exact', 'ann']:
            rows = self.db.execute(self.sql.format(where=where), mode)['rows']
            self.assertEqual([r[0] for r in rows], [i for _, i in wanted])
            for actual, (distance, _) in zip(rows, wanted):
                self.assertAlmostEqual(actual[1], distance, places=12)

    def test_non_byte_aligned_deletions_are_invisible_in_all_paths(self):
        self.check()
        self.check('WHERE g>=0')
        self.check('WHERE g=1', lambda g: g == 1)
        for key in [0, 1, 6, 7, 8, 15, 77, 154, 231, 308]:
            self.db.execute(f'DELETE FROM t WHERE id={key}', 'exact')
            del self.data[key]
            self.check()
            self.check('WHERE g>=0')
            self.check('WHERE g=1', lambda g: g == 1)

    def test_changed_rows_are_decoded_once_instead_of_rescanning(self):
        self.check()
        self.db.execute('UPDATE t SET g=9 WHERE id=100', 'exact')
        self.data[100] = (9, self.data[100][1])
        with patch.object(self.db, 'read_row', wraps=self.db.read_row) as read:
            self.check()
            packed = [call for call in read.call_args_list if call.kwargs.get('packed')]
            self.assertEqual(len(packed), 1)

    def test_transactions_repeated_updates_delete_and_reinsert(self):
        self.check()
        self.db.execute('BEGIN', 'exact')
        self.db.execute("UPDATE t SET v='[0,0,0,1]' WHERE id=10", 'exact')
        self.db.execute('ROLLBACK', 'exact')
        self.check()
        for key in [10, 11, 12]:
            self.db.execute('BEGIN', 'exact')
            self.db.execute(f'DELETE FROM t WHERE id={key}', 'exact')
            self.db.execute(f"INSERT INTO t VALUES ({key},9,'[0,0,0,1]')", 'exact')
            self.db.execute('COMMIT', 'exact')
            self.data[key] = (9, [0.0, 0.0, 0.0, 1.0])
        self.check()
        self.check('WHERE g=9', lambda g: g == 9)
        root = self.db.path.parent
        self.db.close()
        self.db = Database(root)
        self.addCleanup(self.db.close)
        self.check()

    def test_old_matrix_is_discarded_if_two_copies_exceed_budget(self):
        self.check()
        self.db.cache.clear()
        self.db.cache_bytes = 0
        self.db.budget.capacity = 400000
        self.db.execute('UPDATE t SET g=9 WHERE id=100', 'exact')
        self.data[100] = (9, self.data[100][1])
        self.check()
        self.assertLessEqual(self.db._memory_used(), self.db.budget.capacity)

    def test_rebuild_with_dead_slots_has_finite_norms(self):
        self.check()
        self.db.execute('DELETE FROM t WHERE id=0', 'exact')
        del self.data[0]
        table = self.db.table('t')
        table.matrix = None
        self.check()
        self.assertTrue(module._numpy.isfinite(table.matrix[4]).all())

    def test_dtype_change_and_new_offset_do_not_reuse_old_value(self):
        self.check()
        self.db.execute("UPDATE t SET v='[0,0,0,1]' WHERE id=2", 'exact')
        self.data[2] = (2, [0.0, 0.0, 0.0, 1.0])
        table = self.db.table('t')
        with patch.object(module, 'MATRIX_MAX_BYTES', 3000 * 4 * 4):
            self.check()
            self.assertEqual(table.matrix[2].dtype, module._numpy.float32)


if __name__ == '__main__':
    unittest.main()
