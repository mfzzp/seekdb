"""Cardinality-only limited FTS views; dedicated seekdb + PyMySQL required.

Run with SEEKDB_TEST_PORT and optional SEEKDB_TEST_HOST/USER/PASSWORD.
"""
import os
import unittest
import uuid

import pymysql


class FTSCountLimitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.conn = pymysql.connect(
            host=os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
            port=int(os.environ.get("SEEKDB_TEST_PORT", "2881")),
            user=os.environ.get("SEEKDB_TEST_USER", "root"),
            password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            charset="utf8mb4", autocommit=True, read_timeout=120,
        )
        cls.db = "fts_count_limit_" + uuid.uuid4().hex
        cls.rows = [(i, "alpha beta" if i % 3 == 0 else "alpha" if i % 2 == 0 else "gamma") for i in range(1, 1202)]
        with cls.conn.cursor() as c:
            c.execute("CREATE DATABASE " + cls.db)
            c.execute("USE " + cls.db)
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

    @staticmethod
    def count_sql(table="keyed"):
        return f"SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE MATCH(body) AGAINST(%s) LIMIT %s OFFSET %s) v"

    def expected(self, term, limit, offset):
        wanted = set(term.split())
        count = sum(bool(wanted.intersection(body.split())) for _, body in self.rows)
        return min(limit, max(0, count - offset))

    def test_limits_and_offsets(self):
        for table in ("keyed", "heap"):
            for term in ("alpha beta", "gamma", "missing", ""):
                for limit, offset in ((0, 0), (1, 0), (20, 0), (20, 13), (10000, 0), (20, 10000)):
                    with self.subTest(table=table, term=term, limit=limit, offset=offset):
                        self.assertEqual(self.query(self.count_sql(table), (term, limit, offset)), ((self.expected(term, limit, offset),),))

    def test_eligible_plan(self):
        for table in ("keyed", "heap"):
            plan = "\n".join(str(r[0]) for r in self.query("EXPLAIN " + self.count_sql(table), ("alpha beta", 20, 0)))
            self.assertIn("calc_relevance=false", plan)
            self.assertNotIn("sort_keys([MATCH", plan)
            self.assertNotIn("TOP-N SORT", plan)

    def test_rank_sensitive_fallbacks(self):
        inner = "SELECT id,body FROM keyed WHERE MATCH(body) AGAINST('alpha beta') LIMIT 20"
        queries = [inner,
                   "SELECT COUNT(body) FROM (" + inner + ") v",
                   "SELECT COUNT(*) FROM (" + inner + ") v WHERE id>100",
                   "SELECT COUNT(*) FROM (SELECT id FROM keyed WHERE MATCH(body) AGAINST('alpha beta') ORDER BY MATCH(body) AGAINST('alpha beta') DESC,id LIMIT 20) v"]
        for sql in queries:
            with self.subTest(sql=sql):
                plan = "\n".join(str(r[0]) for r in self.query("EXPLAIN " + sql))
                self.assertIn("calc_relevance=true", plan)
                self.query(sql)
        # DISTINCT/grouping affect cardinality and must not be bypassed.
        self.assertEqual(self.query("SELECT COUNT(*) FROM (SELECT DISTINCT body FROM keyed WHERE MATCH(body) AGAINST('alpha beta') LIMIT 20) v"), ((2,),))
        self.assertEqual(self.query("SELECT COUNT(*) FROM (SELECT body FROM keyed WHERE MATCH(body) AGAINST('alpha beta') GROUP BY body LIMIT 20) v"), ((2,),))

    def test_transaction_visibility(self):
        sql = self.count_sql()
        self.query("BEGIN")
        try:
            self.query("DELETE FROM keyed")
            self.assertEqual(self.query(sql, ("alpha beta", 20, 0)), ((0,),))
            self.query("INSERT INTO keyed VALUES(2001,'alpha beta'),(2002,'beta')")
            self.assertEqual(self.query(sql, ("alpha beta", 20, 0)), ((2,),))
            self.assertEqual(self.query(sql, ("alpha beta", 20, 1)), ((1,),))
        finally:
            self.query("ROLLBACK")
        self.assertEqual(self.query(sql, ("alpha beta", 20, 0)), ((20,),))

    def test_prepared_reuse(self):
        sql = self.count_sql().replace("%s", "?")
        self.query("PREPARE fts_count_limit FROM %s", (sql,))
        try:
            for term, limit, offset in (("alpha beta", 20, 0), ("missing", 20, 0), ("alpha", 3, 1), ("gamma", 5000, 18)):
                self.query("SET @term=%s,@lim=%s,@off=%s", (term, limit, offset))
                self.assertEqual(self.query("EXECUTE fts_count_limit USING @term,@lim,@off"), ((self.expected(term, limit, offset),),))
        finally:
            self.query("DEALLOCATE PREPARE fts_count_limit")


if __name__ == "__main__":
    unittest.main(verbosity=2)
