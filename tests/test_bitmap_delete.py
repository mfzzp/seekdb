import tempfile
import unittest

from minivectordb import Database


class BitmapDeleteTest(unittest.TestCase):
    def test_delete_retains_indexes_and_log_payloads(self):
        with tempfile.TemporaryDirectory() as root:
            db = Database(root)
            try:
                run = lambda sql: db.execute(sql, "ann")
                run("CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, v VECTOR(2))")
                run("INSERT INTO t VALUES " + ",".join(f"({i},1,'[{i},0]')" for i in range(10)))
                run("CREATE VECTOR INDEX vi ON t(v)")
                query = "SELECT id FROM t {where} ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 2"
                filtered = query.format(where="WHERE g=1")
                unfiltered = query.format(where="")
                self.assertEqual(run(filtered)["rows"], [[0], [1]])
                self.assertEqual(run(unfiltered)["rows"], [[0], [1]])
                table = db.table("t")
                snapshot, blocks, axis = table.snapshot, table.blocks, table.axis
                original = db.path.read_bytes()
                run("BEGIN")
                run("DELETE FROM t WHERE id=0")
                self.assertEqual(run(filtered)["rows"], [[1], [2]])
                self.assertEqual(snapshot.deleted_count, 0)
                run("ROLLBACK")
                self.assertEqual(run(unfiltered)["rows"], [[0], [1]])
                run("BEGIN")
                run("DELETE FROM t WHERE id=0")
                run("COMMIT")
                self.assertIs(table.snapshot, snapshot)
                self.assertIs(table.blocks, blocks)
                self.assertIs(table.axis, axis)
                self.assertEqual(snapshot.deleted_count, 1)
                self.assertEqual(len(snapshot.deleted), 2)
                self.assertTrue(db.path.read_bytes().startswith(original))
                self.assertEqual(run(filtered)["rows"], [[1], [2]])
                self.assertEqual(run(unfiltered)["rows"], [[1], [2]])
                run("INSERT INTO t VALUES (0,1,'[0,0]')")
                self.assertIsNone(table.snapshot)
                self.assertEqual(run(unfiltered)["rows"], [[0], [1]])
                run("DELETE FROM t WHERE id<6")
                self.assertIsNone(table.snapshot)
                self.assertEqual(run(filtered)["rows"], [[6], [7]])
            finally:
                db.close()

            db = Database(root)
            try:
                self.assertEqual(db.execute(unfiltered, "ann")["rows"], [[6], [7]])
                self.assertEqual(db.execute(filtered, "ann")["rows"], [[6], [7]])
            finally:
                db.close()
