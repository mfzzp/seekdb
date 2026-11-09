"""SQL regression for offline dense-vector batching; requires a running seekdb.

Set SEEKDB_TEST_PORT and optionally SEEKDB_TEST_HOST/USER/PASSWORD. Use a dedicated
GPU-enabled test instance. This checks SQL semantics, not GPU use or full Recall;
pair it with the existing profiler/trace and persistence checks.
"""
import json
import os
import random
import struct
import unittest
import uuid

import pymysql


class CuvsDDLBatchTest(unittest.TestCase):
    def setUp(self):
        self.conn = pymysql.connect(
            host=os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
            port=int(os.environ["SEEKDB_TEST_PORT"]),
            user=os.environ.get("SEEKDB_TEST_USER", "root"),
            password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            autocommit=True, read_timeout=300,
        )
        self.database = "cuvs_batch_" + uuid.uuid4().hex
        with self.conn.cursor() as cur:
            cur.execute("CREATE DATABASE " + self.database)
            cur.execute("USE " + self.database)
            cur.execute("SET ob_query_timeout=300000000")

    def tearDown(self):
        try:
            with self.conn.cursor() as cur:
                cur.execute("DROP DATABASE " + self.database)
        finally:
            self.conn.close()

    def test_offline_batch_ids_nulls_tail_and_cpu_path(self):
        rng = random.Random(9183)
        rows, probes = [], []
        for i in range(2057):
            # Signed, non-contiguous IDs ensure GPU row offsets cannot be
            # mistaken for SQL IDs. Include holes and a partial final batch.
            row_id = -(1 << 40) + i * 97
            vector = [struct.unpack("f", struct.pack("f", rng.uniform(-1, 1)))[0] for _ in range(32)]
            value = json.dumps(vector) if i % 19 != 0 else None
            rows.append((row_id, value))
            if value is not None:
                probes.append((row_id, value))
        valid_ids = {row[0] for row in probes}
        with self.conn.cursor() as cur:
            for backend in ("cuvs", "vsag"):
                table = "items_" + backend
                cur.execute(f"CREATE TABLE {table}(id BIGINT PRIMARY KEY, v VECTOR(32))")
                cur.executemany(f"INSERT INTO {table} VALUES(%s,%s)", rows)
                cur.execute(f"CREATE VECTOR INDEX idx ON {table}(v) WITH (distance=l2,type=hnsw,lib={backend},m=16,ef_construction=200,ef_search=64)")
                cur.execute(f"SELECT COUNT(*),COUNT(v) FROM {table}")
                self.assertEqual((len(rows), len(probes)), cur.fetchone())
                for row_id, value in (probes[0], probes[255], probes[256], probes[-1]):
                    for k in (10, 100):
                        cur.execute(f"SELECT id FROM {table} ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT {k}", (value,))
                        ids = [row[0] for row in cur.fetchall()]
                        self.assertEqual(k, len(ids))
                        self.assertEqual(k, len(set(ids)))
                        self.assertTrue(set(ids) <= valid_ids)
                        self.assertIn(row_id, ids)
                cur.execute(f"DROP TABLE {table}")
            cur.execute("CREATE TABLE empty_items(id BIGINT PRIMARY KEY,v VECTOR(32))")
            cur.execute("CREATE VECTOR INDEX idx ON empty_items(v) WITH (distance=l2,type=hnsw,lib=cuvs)")
            cur.execute("SELECT COUNT(*) FROM empty_items")
            self.assertEqual((0,), cur.fetchone())


if __name__ == "__main__":
    unittest.main(verbosity=2)
