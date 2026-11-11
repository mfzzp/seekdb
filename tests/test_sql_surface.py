"""Broad SQL surface smoke test.

Every statement must produce a well-formed response: either ok:true or an error
with a non-empty code that is not ``internal_error``.  This does not pin down
debatable semantics; it catches crashes, unhandled exceptions and silently
accepted nonsense across the whole grammar surface.
"""

import tempfile
import unittest

from minivectordb import Database

STATEMENTS = [
    # identifiers, quoting and comments
    "CREATE TABLE Mixed (Id BIGINT PRIMARY KEY, Name VARCHAR(8))",
    "INSERT INTO mixed VALUES (1,'a')",
    "INSERT INTO `mixed` (`Id`, `Name`) VALUES (2,'b')",
    "SELECT id, name FROM MIXED ORDER BY id",
    "-- leading comment\nSELECT id FROM mixed",
    "/* block */ SELECT id FROM mixed",
    "SELECT id FROM mixed;",
    "SELECT id FROM mixed;;",
    # values and types
    "INSERT INTO mixed VALUES (-9223372036854775808,'min')",
    "INSERT INTO mixed VALUES (9223372036854775807,'max')",
    "INSERT INTO mixed VALUES (9223372036854775808,'overflow')",
    "INSERT INTO mixed VALUES (1.5,'frac')",
    "INSERT INTO mixed VALUES (7,'12345678')",
    "INSERT INTO mixed VALUES (8,'123456789')",
    "INSERT INTO mixed VALUES (9,'o''brien')",
    "INSERT INTO mixed VALUES (10,'tab\\there')",
    "INSERT INTO mixed VALUES (11,'中文')",
    # comparisons
    "SELECT id FROM mixed WHERE id <> 1 ORDER BY id",
    "SELECT id FROM mixed WHERE id >= -1 AND id < 2 ORDER BY id",
    "SELECT id FROM mixed WHERE name > 'a' ORDER BY id",
    "SELECT id FROM mixed WHERE name = 'o''brien'",
    "SELECT id FROM mixed WHERE id = '1'",
    "SELECT id FROM mixed WHERE nosuch = 1",
    "SELECT id FROM mixed WHERE id",
    # projection and ordering
    "SELECT * FROM mixed ORDER BY id DESC LIMIT 3",
    "SELECT id AS k, name AS n FROM mixed ORDER BY k LIMIT 2",
    "SELECT id FROM mixed ORDER BY nosuch",
    "SELECT id FROM mixed ORDER BY name DESC, id ASC LIMIT 2",
    "SELECT id FROM mixed LIMIT 0",
    "SELECT 1 FROM mixed LIMIT 1",
    "SELECT id FROM mixed LIMIT 1, 2",
    # vector surface
    "CREATE TABLE vec (id BIGINT PRIMARY KEY, v VECTOR(3))",
    "INSERT INTO vec VALUES (1,'[1,2,3]')",
    "INSERT INTO vec VALUES (2,'[1e0,-2.5,3]')",
    "INSERT INTO vec VALUES (3,'[ 1 , 2 , 3 ]')",
    "INSERT INTO vec VALUES (4,'[1,2]')",
    "INSERT INTO vec VALUES (5,'[1,2,3,4]')",
    "INSERT INTO vec VALUES (6,'[nan,1,2]')",
    "INSERT INTO vec VALUES (7,'[inf,1,2]')",
    "INSERT INTO vec VALUES (8,'[1e39,1,2]')",
    "INSERT INTO vec VALUES (9,'not a vector')",
    "INSERT INTO vec VALUES (10,[1,2,3])",
    "SELECT id,L2_DISTANCE(v,'[0,0,0]') AS d FROM vec ORDER BY d LIMIT 3",
    "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM vec ORDER BY d LIMIT 3",
    "SELECT id,L2_DISTANCE(id,'[0,0,0]') AS d FROM vec",
    "SELECT id FROM vec ORDER BY v",
    "SELECT id FROM vec WHERE v = '[0,0,0]'",
    "SELECT id, L2_DISTANCE(v,'[0,0,0]') FROM vec ORDER BY d LIMIT 1",
    # unsupported / malformed
    "ALTER TABLE mixed ADD x BIGINT",
    "DROP TABLE mixed",
    "CREATE INDEX i ON mixed(id)",
    "SELECT COUNT(*) FROM mixed",
    "SELECT id FROM mixed JOIN vec ON mixed.id = vec.id",
    "SELECT id FROM mixed GROUP BY id",
    "INSERT INTO mixed VALUES (12, NULL)",
    "UPDATE mixed SET id = 5 WHERE id = 1",
    "UPDATE mixed SET nosuch = 5",
    "DELETE FROM nosuch",
    "SELECT id FROM nosuch",
    "BEGIN",
    "BEGIN",
    "COMMIT",
    "ROLLBACK",
    "COMMIT",
    "    ",
    "SELECT",
    "SELECT id FROM mixed WHERE id = 1 OR id = 2",
    "SELECT id FROM mixed WHERE id IN (1,2)",
    # statements that must keep the connection healthy
    "SELECT id FROM mixed ORDER BY id LIMIT 1",
    "CREATE VECTOR INDEX vi ON vec(v)",
    "CREATE VECTOR INDEX vi ON vec(v)",
    "CREATE VECTOR INDEX bad ON vec(id)",
    "SELECT id,L2_DISTANCE(v,'[1,2,3]') AS d FROM vec WHERE id >= 1 ORDER BY d LIMIT 2",
]


class SQLSurfaceTest(unittest.TestCase):
    def test_every_statement_yields_a_clean_response(self):
        root = tempfile.mkdtemp(prefix="surface-")
        db = Database(root)
        self.addCleanup(db.close)
        seen_codes = set()
        for sql in STATEMENTS:
            for mode in ("exact", "ann"):
                try:
                    result = db.execute(sql, mode)
                except Exception as exc:  # noqa: BLE001
                    code = getattr(exc, "code", type(exc).__name__)
                    self.assertNotEqual(code, "internal_error",
                                        f"{sql!r} raised an internal error: {exc}")
                    self.assertTrue(isinstance(code, str) and code,
                                    f"{sql!r} error without a code")
                    seen_codes.add(code)
                    continue
                # DDL and transaction control return None; the protocol layer
                # turns that into a bare {"ok": true} response.
                result = result or {}
                self.assertIsInstance(result, dict, f"{sql!r} returned {result!r}")
                if "rows" in result:
                    self.assertIsInstance(result["rows"], list)
                if "affected_rows" in result:
                    self.assertIsInstance(result["affected_rows"], int)
        # A few codes that the surface should have produced.
        self.assertIn("unsupported_sql", seen_codes)
        self.assertIn("duplicate_key", seen_codes)
        self.assertTrue(any(code in seen_codes for code in
                            ("invalid_value", "dimension_mismatch", "invalid_vector")),
                        seen_codes)

    def test_connection_still_works_after_errors(self):
        root = tempfile.mkdtemp(prefix="surface2-")
        db = Database(root)
        self.addCleanup(db.close)
        db.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VARCHAR(4))", "exact")
        db.execute("INSERT INTO t VALUES (1,'a')", "exact")
        for sql in ("SELECT * FROM nosuch", "INSERT INTO t VALUES (1,'b')",
                    "UPDATE t SET v = 'toolong' WHERE id = 1", "ALTER TABLE t ADD c BIGINT"):
            with self.assertRaises(Exception):
                db.execute(sql, "exact")
        self.assertEqual(db.execute("SELECT id,v FROM t", "exact")["rows"], [[1, "a"]])


if __name__ == "__main__":
    unittest.main()
