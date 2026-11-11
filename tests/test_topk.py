"""Check optimized TopK against an independent float32-exact integer oracle."""
import math
import random
import tempfile
import unittest
from minivectordb import Database


class TopKTest(unittest.TestCase):
    def test_modes_filters_limits_and_transaction_visibility(self):
        for dimension in (2, 128):
            with tempfile.TemporaryDirectory() as root:
                db = Database(root)
                self.addCleanup(db.close)
                db.execute(f'CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, v VECTOR({dimension}))', 'exact')
                rng = random.Random(dimension)
                data = {}
                for key in range(-20, 21):
                    data[key] = [rng.randrange(-3, 4) for _ in range(dimension)]
                data[20] = data[-20]
                batch = ','.join(f"({key},{key%3},'{vec}')" for key, vec in data.items())
                db.execute('INSERT INTO t VALUES ' + batch, 'exact')
                db.execute('CREATE VECTOR INDEX vi ON t(v)', 'exact')
                target = [0] * dimension
                for mode in ('exact', 'ann'):
                    for group in (None, 0, 2, 9):
                        where = '' if group is None else f'WHERE g={group}'
                        truth = sorted((math.dist(vec, target), key) for key, vec in data.items()
                                       if group is None or key%3 == group)
                        for limit in (0, 1, 10, 100):
                            sql = f"SELECT id,L2_DISTANCE(v,'{target}') AS d FROM t {where} ORDER BY d LIMIT {limit}"
                            expected = [[key, distance] for distance, key in truth[:limit]]
                            self.assertEqual(db.execute(sql, mode)['rows'], expected)
                db.execute('BEGIN', 'exact')
                db.execute('DELETE FROM t WHERE id<=0', 'exact')
                sql = f"SELECT id,L2_DISTANCE(v,'{target}') AS d FROM t ORDER BY d LIMIT 10"
                expected = [[key, distance] for distance, key in sorted(
                    (math.dist(vec, target), key) for key, vec in data.items() if key > 0)[:10]]
                self.assertEqual(db.execute(sql, 'ann')['rows'], expected)
                db.execute('ROLLBACK', 'exact')
                expected = [[key, distance] for distance, key in sorted(
                    (math.dist(vec, target), key) for key, vec in data.items())[:10]]
                self.assertEqual(db.execute(sql, 'ann')['rows'], expected)
                db.close()


if __name__ == '__main__':
    unittest.main()
