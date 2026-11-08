"""FTS short-list merge integration checks against an independent set oracle.

Requires PyMySQL and a dedicated running seekdb. Connection variables:
SEEKDB_TEST_HOST, SEEKDB_TEST_PORT, SEEKDB_TEST_USER, SEEKDB_TEST_PASSWORD.
Run: python3 unittest/sql/engine/test_fts_short_merge.py
"""

import os
import unittest
import uuid

import pymysql


class FTSShortMergeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.conn = pymysql.connect(
            host=os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
            port=int(os.environ.get("SEEKDB_TEST_PORT", "2881")),
            user=os.environ.get("SEEKDB_TEST_USER", "root"),
            password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            charset="utf8mb4", autocommit=True, read_timeout=120,
        )
        cls.db = "fts_short_merge_" + uuid.uuid4().hex
        cls.words = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta"]
        cls.rows = []
        # More than four 256-row batches; uneven and overlapping postings.
        for i in range(1, 1104):
            terms = [w for j, w in enumerate(cls.words) if i % (j + 2) == 0]
            cls.rows.append((i, " ".join(terms + terms[:i % 3])))
        cls.rows.extend([(1104, None), (1105, ""), (1106, "the is a")])
        with cls.conn.cursor() as c:
            c.execute("SET ob_query_timeout=120000000")
            c.execute("CREATE DATABASE " + cls.db)
            c.execute("USE " + cls.db)
            # Explicit PK uses string doc IDs; the heap exercises integer IDs.
            for table, pk in (("keyed", "PRIMARY KEY"), ("heap", "")):
                c.execute(f"CREATE TABLE {table}(id INT {pk}, body TEXT)")
                c.executemany(f"INSERT INTO {table} VALUES(%s,%s)", cls.rows)
                c.execute(f"ALTER TABLE {table} ADD FULLTEXT INDEX fti(body) WITH PARSER space")

    @classmethod
    def tearDownClass(cls):
        try:
            with cls.conn.cursor() as c:
                c.execute("DROP DATABASE " + cls.db)
        finally:
            cls.conn.close()

    def query(self, sql, args=None):
        with self.conn.cursor() as c:
            c.execute(sql, args)
            return c.fetchall()

    def expected(self, terms):
        wanted = set((terms or "").split()) - {"the", "is", "a"}
        return [i for i, body in self.rows if wanted.intersection((body or "").split())]

    def test_union_and_fallback_boundaries(self):
        terms = [" ".join(self.words[:n]) for n in range(1, 7)]
        terms += ["alpha alpha beta beta", "missing alpha", "alpha missing",
                  "alpha beta missing delta", "missing", "", None, "the is a"]
        for table in ("keyed", "heap"):
            for term in terms:
                with self.subTest(table=table, term=term):
                    expected = self.expected(term)
                    count = self.query(f"SELECT COUNT(*) FROM {table} WHERE MATCH(body) AGAINST(%s)", (term,))
                    self.assertEqual(count, ((len(expected),),))
                    # Numeric predicate retains scoring and index lookup. Verify
                    # every ID against the data oracle, not only its cardinality.
                    ids = self.query(f"SELECT id FROM {table} WHERE MATCH(body) AGAINST(%s)>0 ORDER BY id", (term,))
                    self.assertEqual(ids, tuple((i,) for i in expected))

    def test_fast_path_plan_and_ranked_fallback(self):
        for table in ("keyed", "heap"):
            plan = self.query(f"EXPLAIN SELECT COUNT(*) FROM {table} WHERE MATCH(body) AGAINST('alpha beta')")
            self.assertIn("calc_relevance=false", "\n".join(str(r[0]) for r in plan))
            for tail in ("> 0", "> 0.5"):
                plan = self.query(f"EXPLAIN SELECT COUNT(*) FROM {table} WHERE MATCH(body) AGAINST('alpha beta'){tail}")
                self.assertNotIn("calc_relevance=false", "\n".join(str(r[0]) for r in plan))
            plan = self.query(f"EXPLAIN SELECT id FROM {table} WHERE MATCH(body) AGAINST('alpha beta') LIMIT 20")
            self.assertNotIn("calc_relevance=false", "\n".join(str(r[0]) for r in plan))

    def test_prepared_query_reuse(self):
        self.query("PREPARE short_merge FROM 'SELECT COUNT(*) FROM keyed WHERE MATCH(body) AGAINST(?)'")
        try:
            for term in ("alpha beta", "missing", "alpha", "alpha beta gamma delta epsilon", "", "delta beta"):
                self.query("SET @terms=%s", (term,))
                self.assertEqual(self.query("EXECUTE short_merge USING @terms"), ((len(self.expected(term)),),))
        finally:
            self.query("DEALLOCATE PREPARE short_merge")

    def test_transaction_and_empty_reuse(self):
        q = "SELECT COUNT(*) FROM keyed WHERE MATCH(body) AGAINST('alpha beta')"
        expected = len(self.expected("alpha beta"))
        self.query("BEGIN")
        try:
            self.query("INSERT INTO keyed VALUES(2001,'alpha beta')")
            self.assertEqual(self.query(q), ((expected + 1,),))
            self.query("DELETE FROM keyed")
            self.assertEqual(self.query(q), ((0,),))
            self.query("INSERT INTO keyed VALUES(2002,'alpha beta'),(2003,'beta')")
            self.assertEqual(self.query(q), ((2,),))
        finally:
            self.query("ROLLBACK")
        self.assertEqual(self.query(q), ((expected,),))

    def test_boolean_and_scored_results(self):
        expected = [(i,) for i, b in self.rows if "alpha" in (b or "").split() and "beta" not in (b or "").split()]
        self.assertEqual(self.query("SELECT id FROM keyed WHERE MATCH(body) AGAINST('+alpha -beta' IN BOOLEAN MODE) ORDER BY id"), tuple(expected))
        rows = self.query("SELECT id,MATCH(body) AGAINST('alpha beta') s FROM keyed WHERE MATCH(body) AGAINST('alpha beta') ORDER BY s DESC,id LIMIT 20")
        self.assertEqual(len(rows), 20)
        self.assertTrue(all(score > 0 for _, score in rows))
        self.assertEqual(rows, tuple(sorted(rows, key=lambda r: (-r[1], r[0]))))


if __name__ == "__main__":
    unittest.main(verbosity=2)
