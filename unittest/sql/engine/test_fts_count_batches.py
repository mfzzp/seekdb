"""Count-only FTS and general aggregate batch/skip-mask regression."""
import os
import unittest
import uuid
from collections import Counter
import pymysql


class CountBatchesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.conn = pymysql.connect(host=os.getenv('SEEKDB_TEST_HOST', '127.0.0.1'),
            port=int(os.getenv('SEEKDB_TEST_PORT', '2881')), user=os.getenv('SEEKDB_TEST_USER', 'root'),
            password=os.getenv('SEEKDB_TEST_PASSWORD', ''), charset='utf8mb4', autocommit=True, read_timeout=120)
        cls.db = 'fts_count_batches_' + uuid.uuid4().hex
        cls.rows = [(i, i % 7, None if i % 3 == 0 else i % 11,
                     'alpha beta' if i % 3 == 0 else 'alpha' if i % 2 == 0 else 'gamma') for i in range(1, 4098)]
        with cls.conn.cursor() as c:
            c.execute('CREATE DATABASE ' + cls.db)
            c.execute('USE ' + cls.db)
            c.execute('CREATE TABLE docs(id INT PRIMARY KEY,g INT,v INT,body TEXT)')
            c.executemany('INSERT INTO docs VALUES(%s,%s,%s,%s)', cls.rows)
            c.execute('ALTER TABLE docs ADD FULLTEXT INDEX fti(body) WITH PARSER space')

    @classmethod
    def tearDownClass(cls):
        try:
            with cls.conn.cursor() as c:
                c.execute('DROP DATABASE ' + cls.db)
        finally:
            cls.conn.close()

    def query(self, sql):
        with self.conn.cursor() as c:
            c.execute(sql)
            return c.fetchall()

    def test_aggregate_skip_masks(self):
        for divisor in (1, 2, 7, 63, 64, 65, 255, 256, 257, 10000):
            rows = [r for r in self.rows if r[0] % divisor == 0]
            with self.subTest(divisor=divisor):
                expected = (len(rows), sum(r[2] is not None for r in rows), len({r[2] for r in rows if r[2] is not None}))
                self.assertEqual(self.query(f'SELECT COUNT(*),COUNT(v),COUNT(DISTINCT v) FROM docs WHERE MOD(id,{divisor})=0'), (expected,))
                groups = Counter(r[1] for r in rows)
                self.assertEqual(self.query(f'SELECT g,COUNT(*) FROM docs WHERE MOD(id,{divisor})=0 GROUP BY g ORDER BY g'), tuple(sorted(groups.items())))

    def test_fts_count_and_observed_columns(self):
        for term in ('alpha beta', 'gamma', 'missing'):
            matched = [r for r in self.rows if set(term.split()).intersection(r[3].split())]
            predicate = f"MATCH(body) AGAINST('{term}')"
            self.assertEqual(self.query('SELECT COUNT(*) FROM docs WHERE ' + predicate), ((len(matched),),))
            self.assertEqual(self.query('SELECT COUNT(*),SUM(id) FROM docs WHERE ' + predicate), ((len(matched), sum(r[0] for r in matched) if matched else None),))
            self.assertEqual(self.query('SELECT COUNT(*) FROM docs WHERE ' + predicate + ' AND id>2000'), ((sum(r[0] > 2000 for r in matched),),))
            self.assertEqual(self.query('SELECT id FROM docs WHERE ' + predicate + ' ORDER BY id'), tuple((r[0],) for r in matched))
            for limit, offset in ((0, 0), (1, 0), (255, 1), (256, 255), (257, 256), (5000, 0), (10, 10000)):
                expected = min(limit, max(0, len(matched) - offset))
                self.assertEqual(self.query(f'SELECT COUNT(*) FROM (SELECT id FROM docs WHERE {predicate} LIMIT {limit} OFFSET {offset}) d'), ((expected,),))

    def test_window_count_and_empty(self):
        counts = Counter(r[1] for r in self.rows)
        self.assertEqual(self.query('SELECT id,COUNT(*) OVER(PARTITION BY g) FROM docs ORDER BY id'), tuple((r[0], counts[r[1]]) for r in self.rows))
        self.assertEqual(self.query('SELECT COUNT(*) FROM docs WHERE id<0'), ((0,),))


if __name__ == '__main__':
    unittest.main(verbosity=2)
