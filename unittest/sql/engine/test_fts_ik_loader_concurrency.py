"""Concurrent IK loader handles; use SEEKDB_TEST_* against a dedicated seekdb.

Run first on a freshly bootstrapped instance to also exercise any lazy loader
creation. Each worker owns its connection and repeatedly releases/reacquires
loader handles through TOKENIZE. No shared PyMySQL connections are used.
"""

import json
import os
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor

import pymysql


def connect():
    return pymysql.connect(
        host=os.getenv("SEEKDB_TEST_HOST", "127.0.0.1"),
        port=int(os.getenv("SEEKDB_TEST_PORT", "2881")),
        user=os.getenv("SEEKDB_TEST_USER", "root"),
        password=os.getenv("SEEKDB_TEST_PASSWORD", ""),
        charset="utf8mb4", autocommit=True, connect_timeout=10, read_timeout=30,
    )


class IKLoaderConcurrencyTest(unittest.TestCase):
    def test_parallel_handles_and_reconnects(self):
        workers = 8
        barrier = threading.Barrier(workers, timeout=30)
        text = "研究生命起源 南京市长江大桥"

        def worker():
            results = []
            for reconnect in range(3):
                with connect() as connection, connection.cursor() as cursor:
                    if reconnect == 0:
                        barrier.wait()
                    for _ in range(20):
                        for mode, expected_length in (("smart", 5), ("max_word", 10)):
                            options = json.dumps([
                                {"output": "all"},
                                {"additional_args": [{"ik_mode": mode}]},
                            ])
                            cursor.execute("SELECT TOKENIZE(%s, 'ik', %s) AS tokens", (text, options))
                            result = json.loads(cursor.fetchone()[0])
                            self.assertEqual(result["doc_len"], expected_length)
                            results.append(result)
                        cursor.execute("SELECT TOKENIZE('!!! , 😀', 'ik') AS tokens")
                        self.assertEqual(json.loads(cursor.fetchone()[0]), [])
            return results

        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(worker) for _ in range(workers)]
            results = [future.result(timeout=90) for future in futures]
        for result in results[1:]:
            self.assertEqual(result, results[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
