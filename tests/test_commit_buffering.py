"""Real-file buffering and recovery boundaries, including failed writes."""
import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from minivectordb import Database
from minivector_sql import SQLError
from minivector_store import COMMIT, LENGTH


class CountingRaw(io.RawIOBase):
    """Count unbuffered FileIO writes, rather than Python frame.write calls."""

    def __init__(self, path):
        super().__init__()
        self.file = open(path, "a+b", buffering=0)
        self.writes = []
        self.fail_next_write = False

    def readable(self):
        return True

    def writable(self):
        return True

    def seekable(self):
        return True

    def readinto(self, target):
        return self.file.readinto(target)

    def write(self, data):
        if self.fail_next_write:
            self.fail_next_write = False
            raise OSError("injected raw write failure")
        written = self.file.write(data)
        self.writes.append(written)
        return written

    def seek(self, offset, whence=os.SEEK_SET):
        return self.file.seek(offset, whence)

    def tell(self):
        return self.file.tell()

    def truncate(self, size=None):
        return self.file.truncate(size)

    def fileno(self):
        return self.file.fileno()

    def close(self):
        if not self.closed:
            try:
                super().close()
            finally:
                self.file.close()


class CommitBufferingTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.root = Path(temporary.name)
        self.addCleanup(temporary.cleanup)
        self.db = Database(self.root)
        self.addCleanup(lambda: self.db.close())
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY, payload VARCHAR(4096))")

    def sql(self, statement):
        return self.db.execute(statement, "exact")

    def instrument(self):
        self.db.writer.close()
        raw = CountingRaw(self.db.path)
        self.db.writer = io.BufferedRandom(raw, buffer_size=128 * 1024)
        return raw

    def restart(self):
        self.db.close()
        self.db = Database(self.root)

    def ids(self):
        return [row[0] for row in self.sql("SELECT id FROM t ORDER BY id")["rows"]]

    def test_small_autocommit_batch_has_one_raw_write_and_one_real_sync(self):
        raw = self.instrument()
        before = self.db.path.stat().st_size
        sync = os.fsync
        synced_sizes = []

        def observe_sync(fd):
            # At the durability boundary, both data and marker are on the raw
            # file, but the committed primary-key map must not be published yet.
            synced_sizes.append(os.fstat(fd).st_size)
            self.assertTrue(self.db.path.read_bytes().endswith(LENGTH.pack(1) + COMMIT))
            self.assertEqual(self.ids(), [])
            sync(fd)

        with patch("minivector_store.os.fsync", side_effect=observe_sync) as calls:
            self.sql("INSERT INTO t VALUES " + ",".join(f"({i},'row-{i}')" for i in range(100)))
        self.assertEqual(len(raw.writes), 1)
        self.assertEqual(calls.call_count, 1)
        self.assertEqual(sum(raw.writes), self.db.path.stat().st_size - before)
        self.assertEqual(synced_sizes, [self.db.committed_end])
        self.restart()
        self.assertEqual(self.ids(), list(range(100)))

    def test_autocommit_bulk_still_streams_through_bounded_buffer(self):
        raw = self.instrument()
        before = self.db.path.stat().st_size
        sync = os.fsync
        with patch("minivector_store.os.fsync", wraps=sync) as calls:
            self.db.mutate(("put", ("t", i, [i, "x" * 1024])) for i in range(600))
        self.assertGreater(len(raw.writes), 1)
        self.assertLess(len(raw.writes), 12)
        self.assertEqual(calls.call_count, 1)
        self.assertEqual(sum(raw.writes), self.db.path.stat().st_size - before)
        self.restart()
        self.assertEqual(self.ids(), list(range(600)))

    def test_explicit_transaction_reads_own_writes_before_one_commit_sync(self):
        raw = self.instrument()
        sync = os.fsync
        self.sql("BEGIN")
        with patch("minivector_store.os.fsync", wraps=sync) as calls:
            self.sql("INSERT INTO t VALUES (1,'before')")
            self.assertEqual(self.sql("SELECT payload FROM t WHERE id=1")["rows"], [["before"]])
            self.sql("UPDATE t SET payload='after' WHERE id=1")
            self.assertEqual(self.sql("SELECT payload FROM t WHERE id=1")["rows"], [["after"]])
            self.assertEqual(calls.call_count, 0)
            self.assertEqual(len(raw.writes), 2)
            self.sql("COMMIT")
            self.assertEqual(calls.call_count, 1)
        self.restart()
        self.assertEqual(self.sql("SELECT payload FROM t WHERE id=1")["rows"], [["after"]])

    def test_failed_sql_batch_keeps_earlier_transaction_writes(self):
        self.sql("INSERT INTO t VALUES (1,'committed')")
        self.sql("BEGIN")
        self.sql("INSERT INTO t VALUES (2,'pending')")
        before = self.db.path.read_bytes()
        with self.assertRaises(SQLError):
            self.sql("INSERT INTO t VALUES (3,'must disappear'),(1,'duplicate')")
        self.assertEqual(self.db.path.read_bytes(), before)
        self.assertEqual(self.ids(), [1, 2])
        self.sql("COMMIT")
        self.restart()
        self.assertEqual(self.ids(), [1, 2])

    def test_generator_failure_after_buffer_spill_restores_committed_prefix(self):
        self.sql("INSERT INTO t VALUES (1,'keep')")
        raw = self.instrument()
        before = self.db.path.read_bytes()

        def failing():
            for key in range(2, 602):
                yield "put", ("t", key, [key, "x" * 1024])
            raise ValueError("injected generator failure")

        with self.assertRaisesRegex(ValueError, "generator failure"):
            self.db.mutate(failing())
        self.assertGreater(len(raw.writes), 1)
        self.assertEqual(self.db.path.read_bytes(), before)
        self.assertEqual(self.ids(), [1])
        self.sql("INSERT INTO t VALUES (2,'replacement')")
        self.restart()
        self.assertEqual(self.sql("SELECT id,payload FROM t ORDER BY id")["rows"],
                         [[1, "keep"], [2, "replacement"]])

    def test_autocommit_flush_failure_does_not_publish_or_recover_rows(self):
        self.sql("INSERT INTO t VALUES (1,'keep')")
        raw = self.instrument()
        before = self.db.path.read_bytes()
        raw.fail_next_write = True
        with self.assertRaisesRegex(OSError, "raw write failure"):
            self.sql("INSERT INTO t VALUES (2,'rejected')")
        self.assertEqual(self.db.path.read_bytes(), before)
        self.assertEqual(self.ids(), [1])
        self.restart()
        self.assertEqual(self.ids(), [1])
        self.sql("INSERT INTO t VALUES (2,'retry')")
        self.restart()
        self.assertEqual(self.ids(), [1, 2])

    def test_autocommit_sync_failure_truncates_data_and_marker(self):
        self.sql("INSERT INTO t VALUES (1,'keep')")
        self.instrument()
        before = self.db.path.read_bytes()
        with patch("minivector_store.os.fsync", side_effect=OSError("injected sync failure")):
            with self.assertRaisesRegex(OSError, "sync failure"):
                self.sql("INSERT INTO t VALUES (2,'rejected')")
        self.assertEqual(self.db.path.read_bytes(), before)
        self.assertEqual(self.ids(), [1])
        self.restart()
        self.assertEqual(self.ids(), [1])

    def test_explicit_sync_failure_preserves_pending_rows_for_retry_or_rollback(self):
        for finish in ("COMMIT", "ROLLBACK"):
            with self.subTest(finish=finish):
                self.sql("BEGIN")
                self.sql("INSERT INTO t VALUES (7,'pending')")
                before_commit = self.db.path.read_bytes()
                with patch("minivector_store.os.fsync", side_effect=OSError("injected sync failure")):
                    with self.assertRaises(OSError):
                        self.sql("COMMIT")
                self.assertEqual(self.db.path.read_bytes(), before_commit)
                self.assertEqual(self.ids(), [7])
                self.sql(finish)
                self.restart()
                self.assertEqual(self.ids(), [7] if finish == "COMMIT" else [])
                if finish == "COMMIT":
                    self.sql("DELETE FROM t")

    def test_metric_bounds_follow_snapshot_commit_and_rollback(self):
        self.sql("INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c'),(4,'d')")
        table = self.db.tables["t"]
        table.index_snapshot()
        marker = object()
        table.metric_blocks = marker
        table.metric_queries = 3
        self.sql("BEGIN")
        self.sql("UPDATE t SET payload='pending' WHERE id=1")
        self.assertIs(table.metric_blocks, marker)
        self.sql("ROLLBACK")
        self.assertIs(table.metric_blocks, marker)
        self.sql("DELETE FROM t WHERE id=1")
        self.assertIs(table.metric_blocks, marker)
        self.assertEqual(table.metric_queries, 3)
        self.sql("INSERT INTO t VALUES (5,'e')")
        self.assertIsNone(table.metric_blocks)
        self.assertEqual(table.metric_queries, 0)
        table.index_snapshot()
        table.metric_blocks, table.metric_queries = marker, 3
        self.sql("UPDATE t SET payload='changed' WHERE id=2")
        self.assertIsNone(table.metric_blocks)
        self.assertEqual(table.metric_queries, 0)
        table.index_snapshot()
        table.metric_blocks, table.metric_queries = marker, 3
        self.sql("DELETE FROM t")
        self.assertIsNone(table.metric_blocks)
        self.assertEqual(table.metric_queries, 0)


if __name__ == "__main__":
    unittest.main()
