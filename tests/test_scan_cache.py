import tempfile
import unittest
from unittest.mock import patch
from minivectordb import Database
import minivector_store


class AdaptiveBudgetTest(unittest.TestCase):
    def test_budget_scales_with_the_container_limit(self):
        budget = minivector_store.adaptive_budget
        self.assertEqual(budget(None, 96 << 20, 16 << 20), 96 << 20)
        self.assertEqual(budget(2 << 30, 96 << 20, 16 << 20), 96 << 20)
        self.assertEqual(budget(256 << 20, 96 << 20, 16 << 20), 64 << 20)
        self.assertEqual(budget(32 << 20, 96 << 20, 16 << 20), 16 << 20)

    def test_limit_probe_is_optional(self):
        limit = minivector_store.memory_limit_bytes()
        self.assertTrue(limit is None or (isinstance(limit, int) and limit > 0))


class ScanCacheTest(unittest.TestCase):
    def test_large_scan_preserves_hot_rows_and_small_table_can_warm(self):
        with tempfile.TemporaryDirectory() as root, patch.object(minivector_store, 'CACHE_BUDGET', 6000):
            db = Database(root)
            try:
                run = lambda sql: db.execute(sql, 'exact')
                run('CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(16))')
                run('INSERT INTO t VALUES ' + ','.join(f"({k},'{[k]*16}')" for k in range(30)))
                query = f"SELECT id FROM t ORDER BY L2_DISTANCE(v,'{[0]*16}') LIMIT 3"
                self.assertEqual(run(query)['rows'], [[0], [1], [2]])
                self.assertTrue(db.table('t').large_scan())
                run('SELECT * FROM t WHERE id=29')
                hot = db.table('t').rows[29]
                self.assertIn(hot, db.cache)
                cache_order = list(db.cache)
                self.assertEqual(run(query)['rows'], [[0], [1], [2]])
                self.assertEqual(list(db.cache), cache_order)
                run('CREATE TABLE b (id BIGINT PRIMARY KEY,v VECTOR(16))')
                run(f"INSERT INTO b VALUES (100,'{[1]*16}'),(101,'{[2]*16}')")
                self.assertFalse(db.table('b').large_scan())
                run('SELECT * FROM b')
                self.assertTrue(set(db.table('b').rows.values()) <= db.cache.keys())
                with patch.object(db.reader, 'read', wraps=db.reader.read) as read:
                    run('SELECT * FROM b')
                    self.assertEqual(read.call_count, 0)
                run(query)
                self.assertTrue(set(db.table('b').rows.values()) <= db.cache.keys())
                self.assertLessEqual(db.cache_bytes, 6000)
                self.assertEqual(db.cache_bytes, sum(weight for weight, _ in db.cache.values()))
            finally:
                db.close()

    def test_update_delete_rollback_and_reused_offsets(self):
        with tempfile.TemporaryDirectory() as root:
            db = Database(root)
            try:
                run = lambda sql: db.execute(sql, 'exact')
                run('CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT)')
                run('INSERT INTO t VALUES (1,10),(2,20)')
                run('SELECT * FROM t')
                old = db.table('t').rows[1]
                run('UPDATE t SET g=11 WHERE id=1')
                self.assertNotIn(old, db.cache)
                run('SELECT * FROM t')
                db.begin()
                old = db.table('t').rows[1]
                run('UPDATE t SET g=12 WHERE id=1')
                self.assertNotIn(old, db.cache)
                self.assertEqual(run('SELECT g FROM t WHERE id=1')['rows'], [[12]])
                first = db.changes('t')[1]
                run('UPDATE t SET g=13 WHERE id=1')
                self.assertNotIn(first, db.cache)
                self.assertEqual(run('SELECT g FROM t WHERE id=1')['rows'], [[13]])
                second = db.changes('t')[1]
                run('DELETE FROM t WHERE id=1')
                self.assertNotIn(second, db.cache)
                db.rollback()
                self.assertEqual(db.cache_bytes, 0)
                self.assertEqual(run('SELECT g FROM t WHERE id=1')['rows'], [[11]])
                db.begin()
                run('UPDATE t SET g=99 WHERE id=1')
                self.assertEqual(run('SELECT g FROM t WHERE id=1')['rows'], [[99]])
                db.commit()
                self.assertEqual(run('SELECT g FROM t WHERE id=1')['rows'], [[99]])
                old = db.table('t').rows[1]
                run('DELETE FROM t WHERE id=1')
                self.assertNotIn(old, db.cache)
                self.assertEqual(db.cache_bytes, sum(weight for weight, _ in db.cache.values()))
            finally:
                db.close()

    def test_new_table_pending_versions_and_delete_reinsert(self):
        with tempfile.TemporaryDirectory() as root:
            db = Database(root)
            try:
                run = lambda sql: db.execute(sql, 'exact')
                db.begin()
                run('CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT)')
                run('INSERT INTO t VALUES (1,10)')
                for value in (11, 12):
                    run('SELECT * FROM t')
                    old = db.changes('t')[1]
                    run(f'UPDATE t SET g={value} WHERE id=1')
                    self.assertNotIn(old, db.cache)
                    self.assertEqual(run('SELECT g FROM t')['rows'], [[value]])
                old = db.changes('t')[1]
                run('DELETE FROM t WHERE id=1')
                self.assertNotIn(old, db.cache)
                run('INSERT INTO t VALUES (1,99)')
                self.assertEqual(run('SELECT g FROM t')['rows'], [[99]])
                db.rollback()
                self.assertFalse(db.cache)
                self.assertEqual(db.cache_bytes, 0)
            finally:
                db.close()
