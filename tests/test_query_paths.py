import math
import tempfile
import unittest
from unittest.mock import patch
from minivectordb import Database


class QueryPathsTest(unittest.TestCase):
    def check_queries(self, db, expected):
        for mode in ('exact', 'ann'):
            for descending in (False, True):
                for limit in (0, 1, 4, 100):
                    keys = sorted((k for k in expected if k >= -5), reverse=descending)[:limit]
                    rows = [[k, expected[k][0], math.dist(expected[k][1], (0, 0))] for k in keys]
                    sql = ("SELECT id AS key,g,L2_DISTANCE(v,'[0,0]') AS d FROM t "
                           "WHERE id>=-5 ORDER BY key " + ('DESC' if descending else 'ASC') + f' LIMIT {limit}')
                    self.assertEqual(db.execute(sql, mode)['rows'], rows)
            for order, keyfunc in [('g,id DESC', lambda k: (expected[k][0], -k)),
                                   ('g DESC', lambda k: (-expected[k][0], k))]:
                keys = sorted(expected, key=keyfunc)[:4]
                rows = [[k, math.dist(expected[k][1], (0, 0)), math.dist(expected[k][1], (1, 1))] for k in keys]
                sql = ("SELECT id,L2_DISTANCE(v,'[0,0]') AS d,L2_DISTANCE(v,'[1,1]') AS e "
                       f'FROM t ORDER BY {order} LIMIT 4')
                self.assertEqual(db.execute(sql, mode)['rows'], rows)
            keys = sorted(expected, key=lambda k: (math.dist(expected[k][1], (0, 0)), k))[:4]
            self.assertEqual(db.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 4", mode)['rows'],
                             [[k, math.dist(expected[k][1], (0, 0))] for k in keys])
            self.assertEqual(db.execute('SELECT * FROM t WHERE id>2 AND id<0', mode)['rows'], [])

    def test_projection_sorting_and_transaction_overlay(self):
        initial = {k: [k % 3, (float(k), 1.0)] for k in (-8, -5, 0, 2, 7, 9223372036854775807)}
        with tempfile.TemporaryDirectory() as root:
            db = Database(root)
            try:
                db.execute('CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(2))', 'exact')
                # Keep vectors exactly representable even for extreme primary keys.
                initial[9223372036854775807][1] = (3.0, 1.0)
                sql = 'INSERT INTO t VALUES ' + ','.join(f"({k},{g},'[{v[0]},{v[1]}]')" for k,(g,v) in initial.items())
                db.execute(sql, 'exact')
                self.check_queries(db, initial)
                db.begin()
                db.execute('UPDATE t SET g=20 WHERE id>=2', 'exact')
                db.execute('DELETE FROM t WHERE id=0', 'exact')
                db.execute("INSERT INTO t VALUES (4,0,'[2,1]')", 'exact')
                changed = {k: [20 if k >= 2 else g, v] for k,(g,v) in initial.items() if k != 0}
                changed[4] = [0, (2.0, 1.0)]
                self.check_queries(db, changed)
                db.rollback()
                self.check_queries(db, initial)
                db.begin()
                db.execute('DELETE FROM t WHERE id=-5', 'exact')
                db.commit()
                del initial[-5]
            finally:
                db.close()
            db = Database(root)
            try:
                self.check_queries(db, initial)
            finally:
                db.close()

    def test_narrow_queries_do_not_fetch_unmatched_payloads(self):
        with tempfile.TemporaryDirectory() as root:
            db = Database(root)
            try:
                db.execute('CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT)', 'exact')
                db.execute('INSERT INTO t VALUES ' + ','.join(f'({k},{k})' for k in range(100)), 'exact')
                with patch.object(db, 'read_row', wraps=db.read_row) as read:
                    self.assertEqual(db.execute('SELECT * FROM t ORDER BY id DESC LIMIT 3', 'exact')['rows'], [[99,99],[98,98],[97,97]])
                    self.assertEqual(read.call_count, 3)
                with patch.object(db, 'read_row', wraps=db.read_row) as read:
                    self.assertEqual(db.execute('UPDATE t SET g=0 WHERE id>=98', 'exact')['affected_rows'], 2)
                    self.assertEqual(read.call_count, 2)
            finally:
                db.close()

    def test_new_table_overlay_and_multiple_distance_projections(self):
        with tempfile.TemporaryDirectory() as root:
            db = Database(root)
            try:
                db.begin()
                db.execute('CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2))', 'exact')
                db.execute("INSERT INTO t VALUES (2,'[3,4]'),(1,'[0,0]')", 'exact')
                result = db.execute("SELECT id AS k,L2_DISTANCE(v,'[0,0]') AS a,"
                                    "L2_DISTANCE(v,'[3,4]') AS b FROM t ORDER BY k LIMIT 1", 'exact')
                self.assertEqual(result, {'columns': ['k', 'a', 'b'], 'rows': [[1, 0.0, 5.0]]})
                db.rollback()
                db.execute('CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2))', 'exact')
                self.assertEqual(db.execute('SELECT * FROM t ORDER BY id LIMIT 1', 'exact')['rows'], [])
            finally:
                db.close()
