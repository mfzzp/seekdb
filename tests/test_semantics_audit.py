"""Audit of the two behaviour decisions: UPDATE affected_rows and shutdown errors.

affected_rows follows "rows whose stored value actually changed", which is what
MySQL reports by default and what the wording 实际受影响记录数 suggests.  The
public package never exercises a no-op update, so the decision is pinned here in
both directions instead of being left implicit.

shutdown_quietly() swallows close failures.  That is only safe because every
commit is already fsynced and uncommitted frames are truncated during the next
recovery, so a failed close cannot lose acknowledged data; this test proves the
committed rows survive when close() raises.
"""

import tempfile
import unittest
from unittest.mock import patch

import minivector_store
from minivector_store import Store
import minivectordb
from minivectordb import Database


class AffectedRowsTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.db = Database(self.root.name)
        self.addCleanup(self.db.close)
        self.db.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,"
                        "s VARCHAR(8),v VECTOR(2))", "exact")
        self.db.execute("INSERT INTO t VALUES (1,1,'a','[0,0]'),(2,1,'b','[1,0]'),"
                        "(3,2,'c','[0,1]')", "exact")

    def test_matched_but_unchanged_reports_zero(self):
        response = self.db.execute("UPDATE t SET g = 1 WHERE g = 1", "exact")
        self.assertEqual(response["affected_rows"], 0)
        self.assertEqual([row[0] for row in
                          self.db.execute("SELECT id FROM t ORDER BY id", "exact")["rows"]],
                         [1, 2, 3])

    def test_partially_changed_counts_only_changes(self):
        response = self.db.execute("UPDATE t SET g = 9 WHERE g = 1", "exact")
        self.assertEqual(response["affected_rows"], 2)
        response = self.db.execute("UPDATE t SET g = 9 WHERE g >= 1", "exact")
        self.assertEqual(response["affected_rows"], 1, "only row 3 still differs")
        response = self.db.execute("UPDATE t SET g = 9 WHERE g >= 1", "exact")
        self.assertEqual(response["affected_rows"], 0)

    def test_vector_update_counts_only_real_changes(self):
        response = self.db.execute("UPDATE t SET v = '[0,0]' WHERE id = 1", "exact")
        self.assertEqual(response["affected_rows"], 0, "same vector, same stored bytes")
        response = self.db.execute("UPDATE t SET v = '[0.5,0]' WHERE id = 1", "exact")
        self.assertEqual(response["affected_rows"], 1)

    def test_repeated_update_inside_a_transaction(self):
        self.db.execute("BEGIN", "exact")
        first = self.db.execute("UPDATE t SET s = 'z' WHERE id = 2", "exact")
        second = self.db.execute("UPDATE t SET s = 'z' WHERE id = 2", "exact")
        self.assertEqual((first["affected_rows"], second["affected_rows"]), (1, 0))
        self.db.execute("COMMIT", "exact")
        self.assertEqual(
            self.db.execute("SELECT s FROM t WHERE id = 2", "exact")["rows"], [["z"]])

    def test_delete_reports_deleted_rows(self):
        self.assertEqual(self.db.execute("DELETE FROM t WHERE g = 99", "exact")["affected_rows"], 0)
        self.assertEqual(self.db.execute("DELETE FROM t WHERE id <= 2", "exact")["affected_rows"], 2)


class ShutdownFailureTest(unittest.TestCase):
    def test_cleanup_failure_keeps_committed_data(self):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        db = Database(root.name)
        db.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT)", "exact")
        db.execute("INSERT INTO t VALUES (1,1),(2,2)", "exact")
        db.execute("BEGIN", "exact")
        db.execute("INSERT INTO t VALUES (3,3)", "exact")   # never committed

        def boom(self):
            raise OSError("simulated close failure")

        with patch.object(Store, "close", boom):
            minivectordb.shutdown_quietly(db)     # must swallow the failure
        # The real close now runs; the uncommitted row must disappear and the
        # committed rows must survive.
        db.close()
        again = Database(root.name)
        try:
            rows = again.execute("SELECT id FROM t ORDER BY id", "exact")["rows"]
            self.assertEqual(rows, [[1], [2]])
        finally:
            again.close()

    def test_cleanup_failure_does_not_escape(self):
        class Broken:
            def close(self):
                raise OSError("boom")

        minivectordb.shutdown_quietly(Broken())   # must not raise


if __name__ == "__main__":
    unittest.main()
