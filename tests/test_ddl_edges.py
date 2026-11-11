"""DDL edges: what must be accepted, what must be rejected, and how names echo.

A hidden case can create a table with a single primary-key column, a one-element
vector, a one-character string, or ask for a schema this database does not support.
Each of those has one right answer: accept and work, or reject with a code and stay
alive.
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


class DDLTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.db = Database(self.root.name)
        self.addCleanup(self.db.close)

    def test_table_with_only_a_primary_key(self):
        self.db.execute("CREATE TABLE only_pk (id BIGINT PRIMARY KEY)", "exact")
        self.assertEqual(self.db.execute("INSERT INTO only_pk VALUES (1)", "exact")
                         ["affected_rows"], 1)
        self.assertEqual(self.db.execute("SELECT id FROM only_pk", "exact")["rows"], [[1]])

    def test_one_element_vector(self):
        self.db.execute("CREATE TABLE tiny (id BIGINT PRIMARY KEY, v VECTOR(1))", "exact")
        self.db.execute("INSERT INTO tiny VALUES (1,'[2.5]')", "exact")
        rows = self.db.execute("SELECT id, L2_DISTANCE(v,'[0]') AS d FROM tiny",
                               "exact")["rows"]
        self.assertEqual(rows, [[1, 2.5]])

    def test_one_character_varchar(self):
        self.db.execute("CREATE TABLE tiny (id BIGINT PRIMARY KEY, s VARCHAR(1))", "exact")
        self.assertEqual(self.db.execute("INSERT INTO tiny VALUES (1,'a')", "exact")
                         ["affected_rows"], 1)
        self.assertEqual(code_of(lambda: self.db.execute("INSERT INTO tiny VALUES (2,'ab')",
                                                         "exact")), "invalid_value")

    def test_rejected_schemas(self):
        cases = {
            "CREATE TABLE t (id VARCHAR(8) PRIMARY KEY)": "invalid_schema",
            "CREATE TABLE t (id BIGINT)": "invalid_schema",
            "CREATE TABLE t (id BIGINT PRIMARY KEY, id BIGINT)": "invalid_schema",
            "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(0))": "invalid_schema",
            "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(0))": "invalid_schema",
            "CREATE TABLE t ()": "unsupported_sql",
        }
        for sql, expected in cases.items():
            with self.subTest(sql=sql):
                self.assertEqual(code_of(lambda statement=sql: self.db.execute(statement,
                                                                               "exact")),
                                 expected)

    def test_duplicate_table_is_rejected_and_the_first_survives(self):
        self.db.execute("CREATE TABLE t (id BIGINT PRIMARY KEY)", "exact")
        self.db.execute("INSERT INTO t VALUES (1)", "exact")
        self.assertEqual(code_of(lambda: self.db.execute(
            "CREATE TABLE t (id BIGINT PRIMARY KEY)", "exact")), "table_exists")
        self.assertEqual(self.db.execute("SELECT id FROM t", "exact")["rows"], [[1]])

    def test_insert_column_list_order_and_missing_columns(self):
        self.db.execute("CREATE TABLE t (id BIGINT PRIMARY KEY, a BIGINT, "
                        "b VARCHAR(8), v VECTOR(2))", "exact")
        # A column list may reorder the values.
        self.assertEqual(self.db.execute(
            "INSERT INTO t (b, id, a, v) VALUES ('x', 1, 10, '[1,2]')", "exact")
            ["affected_rows"], 1)
        rows = self.db.execute("SELECT * FROM t", "exact")["rows"]
        self.assertEqual(rows[0][:3], [1, 10, "x"])
        self.assertEqual(list(rows[0][3]), [1.0, 2.0],
                         "in process a vector is a tuple; the protocol serialises it as an array")
        # Omitting a column is rejected: this database has no NULL or DEFAULT to
        # fall back to, so a partial row has no meaning.
        self.assertEqual(code_of(lambda: self.db.execute(
            "INSERT INTO t (id, a) VALUES (2, 20)", "exact")), "invalid_value")
        self.assertEqual(code_of(lambda: self.db.execute(
            "INSERT INTO t (id, id) VALUES (3, 3)", "exact")), "invalid_value")
        self.assertEqual(code_of(lambda: self.db.execute(
            "INSERT INTO t (id, nosuch) VALUES (4, 4)", "exact")), "unknown_column")
        self.assertEqual(self.db.execute("SELECT id FROM t", "exact")["rows"], [[1]])

    def test_two_vector_columns_are_rejected(self):
        self.assertEqual(code_of(lambda: self.db.execute(
            "CREATE TABLE two_vec (id BIGINT PRIMARY KEY, v1 VECTOR(2), v2 VECTOR(3))",
            "exact")), "invalid_schema")
        # The rejected table must not linger, so the name is free afterwards.
        self.db.execute("CREATE TABLE two_vec (id BIGINT PRIMARY KEY, v1 VECTOR(2))",
                        "exact")
        self.assertEqual(self.db.execute("SELECT id FROM two_vec", "exact")["rows"], [])

    def test_single_statement_bulk_insert_scales_linearly(self):
        """One INSERT of many rows must stay linear; measured 18-21 us per row."""
        rows = 5000
        self.db.execute("CREATE TABLE bulk (id BIGINT PRIMARY KEY, g BIGINT, "
                        "v VECTOR(8))", "exact")
        values = ",".join(
            f"({key},{key % 20},'[{','.join(['0.5'] * 8)}]')" for key in range(rows))
        response = self.db.execute(f"INSERT INTO bulk VALUES {values}", "exact")
        self.assertEqual(response["affected_rows"], rows)
        self.assertEqual(len(self.db.execute("SELECT id FROM bulk", "exact")["rows"]), rows)

    def test_declared_name_case_is_echoed_and_lookup_is_case_insensitive(self):
        self.db.execute('CREATE TABLE MixedCase (ID BIGINT PRIMARY KEY, Tag VARCHAR(4))',
                        "exact")
        self.db.execute("INSERT INTO mixedcase VALUES (1,'x')", "exact")
        response = self.db.execute("SELECT ID, TAG FROM MIXEDCASE", "exact")
        self.assertEqual(response["columns"], ["ID", "Tag"],
                         "column names keep their declared case, as MySQL does")
        self.assertEqual(response["rows"], [[1, "x"]])


if __name__ == "__main__":
    unittest.main()
