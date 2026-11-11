"""DDL inside a transaction, and index names across tables.

The specification fixes atomicity for statements but not what a transaction does
with a CREATE TABLE inside it, so this implementation takes the PostgreSQL reading:
DDL joins the transaction and a ROLLBACK removes the table as well as the rows.
The alternative (MySQL's implicit commit) was measured on a second implementation
of the same protocol and would leave the table behind; the choice is recorded here
and in the README so it is a contract rather than an accident.
"""

import os
import tempfile
import unittest

from minivectordb import Database
from minivector_sql import SQLError


def code_of(callable_):
    try:
        callable_()
    except SQLError as exc:
        return exc.code
    return None


class TransactionDDLTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.db = Database(self.root.name)
        self.addCleanup(self.db.close)

    def test_rollback_removes_a_table_created_inside_the_transaction(self):
        self.db.execute("CREATE TABLE keep (id BIGINT PRIMARY KEY)", "exact")
        self.db.execute("INSERT INTO keep VALUES (1)", "exact")
        self.db.execute("BEGIN", "exact")
        self.db.execute("CREATE TABLE made (id BIGINT PRIMARY KEY)", "exact")
        self.assertEqual(self.db.execute("INSERT INTO made VALUES (2)", "exact")
                         ["affected_rows"], 1)
        self.assertEqual(self.db.execute("SELECT id FROM made", "exact")["rows"], [[2]],
                         "a transaction sees its own DDL")
        self.db.execute("ROLLBACK", "exact")
        self.assertEqual(self.db.execute("SELECT id FROM keep", "exact")["rows"], [[1]],
                         "earlier committed rows survive")
        self.assertEqual(code_of(lambda: self.db.execute("SELECT id FROM made", "exact")),
                         "unknown_table",
                         "with the PostgreSQL reading the table is rolled back too")

    def test_commit_keeps_a_table_created_inside_the_transaction(self):
        self.db.execute("BEGIN", "exact")
        self.db.execute("CREATE TABLE made (id BIGINT PRIMARY KEY)", "exact")
        self.db.execute("COMMIT", "exact")
        self.assertEqual(self.db.execute("SELECT id FROM made", "exact")["rows"], [])
        self.db.execute("INSERT INTO made VALUES (1)", "exact")
        self.assertEqual(self.db.execute("SELECT id FROM made", "exact")["rows"], [[1]])

    def test_index_names_are_scoped_per_table(self):
        for name in ("a", "b"):
            self.db.execute(f"CREATE TABLE {name} (id BIGINT PRIMARY KEY, v VECTOR(2))",
                            "exact")
            self.db.execute(f"CREATE VECTOR INDEX vi ON {name}(v)", "exact")
        # MySQL scopes index names per table, and so do we: the same name is free
        # on a different table but not twice on one.
        self.assertEqual(code_of(lambda: self.db.execute(
            "CREATE VECTOR INDEX vi ON a(v)", "exact")), "index_exists")
        self.db.execute("INSERT INTO a VALUES (1,'[0,0]')", "exact")
        self.db.execute("INSERT INTO b VALUES (1,'[1,1]')", "exact")
        self.assertEqual(self.db.execute("SELECT id FROM a", "exact")["rows"], [[1]])
        self.assertEqual(self.db.execute("SELECT id FROM b", "exact")["rows"], [[1]])


if __name__ == "__main__":
    unittest.main()
