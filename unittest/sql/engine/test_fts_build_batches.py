"""Build FTS indexes from documents crossing expanded-word batch boundaries."""
import os
import unittest
import uuid
import pymysql


def word(i):
    return 'term' + ''.join(chr(97 + (i // (26 ** n)) % 26) for n in range(4))


class FTSBuildBatchesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.conn = pymysql.connect(host=os.getenv('SEEKDB_TEST_HOST', '127.0.0.1'),
            port=int(os.getenv('SEEKDB_TEST_PORT', '2881')), user=os.getenv('SEEKDB_TEST_USER', 'root'),
            password=os.getenv('SEEKDB_TEST_PASSWORD', ''), charset='utf8mb4', autocommit=True, read_timeout=180)
        cls.db = 'fts_build_batches_' + uuid.uuid4().hex
        cls.counts = (0, 1, 15, 255, 256, 257, 511, 512, 1024)
        cls.rows = [(i, ' '.join(word(n) for n in range(count))) for i, count in enumerate(cls.counts)]
        cls.rows += [(20, None), (21, ' '), (22, ' '.join([word(256)] * 600))]
        with cls.conn.cursor() as c:
            c.execute('CREATE DATABASE ' + cls.db)
            c.execute('USE ' + cls.db)

    @classmethod
    def tearDownClass(cls):
        try:
            with cls.conn.cursor() as c:
                c.execute('DROP DATABASE ' + cls.db)
        finally:
            cls.conn.close()

    def test_word_batch_boundaries(self):
        with self.conn.cursor() as c:
            for parser in ('space', 'beng', 'ik'):
                for keyed in (True, False):
                    table = parser + ('_keyed' if keyed else '_heap')
                    c.execute(f'CREATE TABLE {table}(id INT {"PRIMARY KEY" if keyed else ""},body TEXT)')
                    c.executemany(f'INSERT INTO {table} VALUES(%s,%s)', self.rows)
                    c.execute(f'ALTER TABLE {table} ADD FULLTEXT INDEX fti(body) WITH PARSER {parser}')
                    for index in (0, 14, 254, 255, 256, 510, 511, 1023, 1024):
                        term = word(index)
                        expected = tuple((row_id,) for row_id, text in self.rows if text and term in text.split())
                        with self.subTest(parser=parser, keyed=keyed, term=term):
                            c.execute(f'SELECT id FROM {table} WHERE MATCH(body) AGAINST(%s) ORDER BY id', (term,))
                            self.assertEqual(c.fetchall(), expected)
                    # Index maintenance after construction must preserve rollback.
                    c.execute('BEGIN')
                    c.execute(f'DELETE FROM {table}')
                    c.execute(f'SELECT COUNT(*) FROM {table} WHERE MATCH(body) AGAINST(%s)', (word(0),))
                    self.assertEqual(c.fetchall(), ((0,),))
                    c.execute('ROLLBACK')
                    c.execute(f'SELECT COUNT(*) FROM {table} WHERE MATCH(body) AGAINST(%s)', (word(0),))
                    self.assertEqual(c.fetchall(), ((sum(count > 0 for count in self.counts),),))


if __name__ == '__main__':
    unittest.main(verbosity=2)
