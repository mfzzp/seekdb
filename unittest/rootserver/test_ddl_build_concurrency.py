"""Integration tests for concurrent local DDL workers on a dedicated seekdb.

Requires PyMySQL. Set SEEKDB_TEST_PORT (and optionally SEEKDB_TEST_HOST,
SEEKDB_TEST_USER, SEEKDB_TEST_PASSWORD). Each test uses a fresh temporary DB.
Run: python3 unittest/rootserver/test_ddl_build_concurrency.py
"""

import os
import threading
import time
import unittest
import uuid
from concurrent.futures import ThreadPoolExecutor

import pymysql


class DDLBuildConcurrencyTest(unittest.TestCase):
    def connect(self):
        conn = pymysql.connect(
            host=os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
            port=int(os.environ.get("SEEKDB_TEST_PORT", "2881")),
            user=os.environ.get("SEEKDB_TEST_USER", "root"),
            password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            charset="utf8mb4", autocommit=True, read_timeout=120,
        )
        with conn.cursor() as cur:
            cur.execute("SET ob_query_timeout=120000000")
            cur.execute("SET ob_trx_timeout=120000000")
        return conn

    def setUp(self):
        self.database = "fts_build_concurrency_" + uuid.uuid4().hex
        self.conn = self.connect()
        with self.conn.cursor() as cur:
            cur.execute("CREATE DATABASE " + self.database)
            cur.execute("USE " + self.database)

    def tearDown(self):
        try:
            with self.conn.cursor() as cur:
                cur.execute("DROP DATABASE " + self.database)
        finally:
            self.conn.close()

    def task_connection(self):
        conn = self.connect()
        with conn.cursor() as cur:
            cur.execute("USE " + self.database)
        return conn

    def execute_ddl(self, cur, sql):
        # Concurrent DML/schema publication can return 1205 in the existing
        # single-worker implementation too. Retry that transient conflict only;
        # the final DDL and all data assertions must still succeed.
        deadline = time.monotonic() + 30
        while True:
            try:
                return cur.execute(sql)
            except pymysql.MySQLError as error:
                if error.args[0] != 1205 or time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)

    def test_concurrent_fts_ordinary_index_and_transactions(self):
        parsers = ("space", "beng", "ik", "ngram")
        with self.conn.cursor() as cur:
            for parser in parsers:
                cur.execute(f"CREATE TABLE docs_{parser}(id INT PRIMARY KEY, body TEXT)")
                cur.executemany(f"INSERT INTO docs_{parser} VALUES(%s,%s)",
                                [(i, "alpha beta 数据库 " * 20) for i in range(500)])
            cur.execute("CREATE TABLE ordinary(id INT PRIMARY KEY, value INT)")
            cur.executemany("INSERT INTO ordinary VALUES(%s,%s)",
                            [(i, i % 10) for i in range(500)])
        start = threading.Barrier(6)

        def build(parser):
            with self.task_connection() as conn, conn.cursor() as cur:
                start.wait(timeout=30)
                self.execute_ddl(cur, f"ALTER TABLE docs_{parser} ADD FULLTEXT INDEX fti(body) WITH PARSER {parser}")

        def build_ordinary():
            with self.task_connection() as conn, conn.cursor() as cur:
                start.wait(timeout=30)
                self.execute_ddl(cur, "ALTER TABLE ordinary ADD INDEX vi(value)")

        def write_rows():
            with self.task_connection() as conn, conn.cursor() as cur:
                start.wait(timeout=30)
                for i in range(10):
                    conn.begin()
                    cur.execute("UPDATE docs_beng SET body='gamma' WHERE id >= %s AND id < %s",
                                (i * 5, i * 5 + 5))
                    cur.execute("INSERT INTO docs_beng VALUES(%s,'alpha')", (500 + i,))
                    if i % 2 == 0:
                        conn.commit()
                    else:
                        conn.rollback()

        with ThreadPoolExecutor(max_workers=6) as pool:
            jobs = [pool.submit(build, parser) for parser in parsers]
            jobs.extend((pool.submit(build_ordinary), pool.submit(write_rows)))
            for job in jobs:
                job.result(timeout=120)
        with self.conn.cursor() as cur:
            for parser in parsers:
                cur.execute(f"SELECT id FROM docs_{parser} WHERE body LIKE '%alpha%' ORDER BY id")
                expected = cur.fetchall()
                cur.execute(f"SELECT id FROM docs_{parser} WHERE MATCH(body) AGAINST('alpha') ORDER BY id")
                self.assertEqual(expected, cur.fetchall(), parser)
                self.assertEqual(480 if parser == "beng" else 500, len(expected))
            cur.execute("SELECT COUNT(*) FROM ordinary WHERE value=3")
            self.assertEqual(((50,),), cur.fetchall())

    def test_failed_unique_index_does_not_block_fts(self):
        with self.conn.cursor() as cur:
            cur.execute("CREATE TABLE duplicate_values(id INT PRIMARY KEY, value INT)")
            cur.execute("INSERT INTO duplicate_values VALUES(1,7),(2,7)")
            cur.execute("CREATE TABLE docs(id INT PRIMARY KEY, body TEXT)")
            cur.executemany("INSERT INTO docs VALUES(%s,'alpha beta')", [(i,) for i in range(100)])
        start = threading.Barrier(2)

        def build_unique():
            with self.task_connection() as conn, conn.cursor() as cur:
                start.wait(timeout=30)
                with self.assertRaises(pymysql.MySQLError) as error:
                    cur.execute("ALTER TABLE duplicate_values ADD UNIQUE INDEX ui(value)")
                self.assertEqual(1062, error.exception.args[0])

        def build_fts():
            with self.task_connection() as conn, conn.cursor() as cur:
                start.wait(timeout=30)
                cur.execute("ALTER TABLE docs ADD FULLTEXT INDEX fti(body) WITH PARSER beng")

        with ThreadPoolExecutor(max_workers=2) as pool:
            jobs = [pool.submit(build_unique), pool.submit(build_fts)]
            for job in jobs:
                job.result(timeout=120)
        with self.conn.cursor() as cur:
            cur.execute("SELECT COUNT(*),SUM(id) FROM docs WHERE MATCH(body) AGAINST('alpha')")
            self.assertEqual(((100, 4950),), cur.fetchall())
            cur.execute("ALTER TABLE duplicate_values ADD INDEX vi(value)")
            cur.execute("SELECT COUNT(*) FROM duplicate_values WHERE value=7")
            self.assertEqual(((2,),), cur.fetchall())


if __name__ == "__main__":
    unittest.main(verbosity=2)
