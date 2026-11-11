"""Model-based randomised test: every operation is mirrored in Python.

The model tracks committed rows plus an optional transaction overlay, so it can
predict visibility inside a transaction, after COMMIT, after ROLLBACK and after
reopening the database.  Statements are generated from a small but realistic
grammar: multi-row INSERT, predicate UPDATE/DELETE, explicit transactions,
restarts and TopK queries compared against brute force.
"""

import math
import random
import struct
import tempfile
import unittest

from minivectordb import Database

DIM = 3
IDS = 240


def f32(values):
    return list(struct.unpack(f"<{len(values)}f", struct.pack(f"<{len(values)}f", *values)))


def literal(vector):
    # repr round-trips a float exactly, so the stored float32 value equals the
    # model value and distances can be compared without slack.
    return "[" + ",".join(repr(float(value)) for value in vector) + "]"


class Model:
    def __init__(self):
        self.rows = {}          # id -> (g, s, vector)
        self.pending = None     # id -> (g, s, vector) or None for delete

    def view(self):
        current = dict(self.rows)
        if self.pending is not None:
            for key, row in self.pending.items():
                if row is None:
                    current.pop(key, None)
                else:
                    current[key] = row
        return current

    def begin(self):
        self.pending = {}

    def commit(self):
        if self.pending is not None:
            for key, row in self.pending.items():
                if row is None:
                    self.rows.pop(key, None)
                else:
                    self.rows[key] = row
        self.pending = None

    def rollback(self):
        self.pending = None

    def upsert(self, key, row):
        if self.pending is not None:
            self.pending[key] = row
        else:
            self.rows[key] = row

    def delete(self, key):
        if self.pending is not None:
            self.pending[key] = None
        else:
            self.rows.pop(key, None)


class ModelFuzzTest(unittest.TestCase):
    def test_random_operations_match_model(self):
        rng = random.Random(20261111)
        root = tempfile.mkdtemp(prefix="model-fuzz-")
        db = Database(root)
        self.addCleanup(lambda: db.close() if not db.writer.closed else None)
        db.execute(f"CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,"
                   f"s VARCHAR(8),v VECTOR({DIM}))", "exact")
        db.execute("CREATE VECTOR INDEX vi ON t(v)", "exact")
        model = Model()
        target = [0.25, -0.5, 1.0]
        stats = {"insert": 0, "update": 0, "delete": 0, "query": 0, "reopen": 0,
                 "commit": 0, "rollback": 0, "error": 0}

        def random_row(key):
            return (rng.randrange(5), rng.choice("abc"),
                    f32([round(rng.gauss(0, 1), 6) for _ in range(DIM)]))

        def sql_row(key, row):
            g, s, vector = row
            return f"({key},{g},'{s}','{literal(vector)}')"

        def predicates():
            kind = rng.randrange(3)
            if kind == 0:
                value = rng.randrange(5)
                return f"g = {value}", lambda key, row, v=value: row[0] == v
            if kind == 1:
                low = rng.randrange(-20, IDS)
                high = low + rng.randrange(1, 40)
                return (f"id >= {low} AND id < {high}",
                        lambda key, row, lo=low, hi=high: lo <= key < hi)
            value = rng.choice("abc")
            return f"s = '{value}'", lambda key, row, v=value: row[1] == v

        for step in range(220):
            roll = rng.random()
            view = model.view()
            if roll < 0.30:
                keys = [key for key in rng.sample(range(IDS), 6) if key not in view]
                if not keys:
                    continue
                rows = {key: random_row(key) for key in keys}
                sql = "INSERT INTO t VALUES " + ",".join(
                    sql_row(key, row) for key, row in rows.items())
                db.execute(sql, "exact")
                for key, row in rows.items():
                    model.upsert(key, row)
                stats["insert"] += 1
            elif roll < 0.42 and view:
                where, test = predicates()
                keys = [key for key, row in view.items() if test(key, row)]
                column = rng.choice(["g", "s", "v"])
                if not keys:
                    continue
                if column == "g":
                    new = rng.randrange(5)
                    sql = f"UPDATE t SET g = {new} WHERE {where}"
                elif column == "s":
                    new = rng.choice("abc")
                    sql = f"UPDATE t SET s = '{new}' WHERE {where}"
                else:
                    new = f32([round(rng.gauss(0, 1), 6) for _ in range(DIM)])
                    sql = f"UPDATE t SET v = '{literal(new)}' WHERE {where}"
                response = db.execute(sql, "exact")
                changed = 0
                for key in keys:
                    g, s, vector = view[key]
                    updated = (new if column == "g" else g,
                               new if column == "s" else s,
                               new if column == "v" else vector)
                    if updated != view[key]:
                        changed += 1
                        model.upsert(key, updated)
                self.assertEqual(response["affected_rows"], changed,
                                 f"affected_rows for {sql}")
                stats["update"] += 1
            elif roll < 0.52 and view:
                where, test = predicates()
                keys = [key for key, row in view.items() if test(key, row)]
                response = db.execute(f"DELETE FROM t WHERE {where}", "exact")
                self.assertEqual(response["affected_rows"], len(keys),
                                 f"affected_rows for DELETE WHERE {where}")
                for key in keys:
                    model.delete(key)
                stats["delete"] += 1
            elif roll < 0.60 and model.pending is None:
                db.execute("BEGIN", "exact")
                model.begin()
            elif roll < 0.70 and model.pending is not None:
                db.execute("COMMIT", "exact")
                model.commit()
                stats["commit"] += 1
            elif roll < 0.76 and model.pending is not None:
                db.execute("ROLLBACK", "exact")
                model.rollback()
                stats["rollback"] += 1
            elif roll < 0.82:
                db.close()
                db = Database(root)
                model.rollback()          # an open transaction dies with the process
                stats["reopen"] += 1
                rows = db.execute("SELECT * FROM t ORDER BY id", "exact")["rows"]
                expected = [[key, row[0], row[1], row[2]]
                            for key, row in sorted(model.rows.items())]
                self.assertEqual(len(rows), len(expected))
                for got, want in zip(rows, expected):
                    self.assertEqual(got[0], want[0])
                    self.assertEqual(got[1], want[1])
                    self.assertEqual(got[2], want[2])
                    for a, b in zip(got[3], want[3]):
                        self.assertAlmostEqual(a, b, places=6)
            else:
                view = model.view()
                if not view:
                    continue
                limit = rng.choice([0, 1, 3, 5, 20])
                where = ""
                if rng.random() < 0.4:
                    value = rng.randrange(5)
                    where = f" WHERE g = {value}"
                    predicted = {key: row for key, row in view.items() if row[0] == value}
                else:
                    predicted = view
                mode = "ann" if rng.random() < 0.5 else "exact"
                sql = (f"SELECT id,L2_DISTANCE(v,'{literal(target)}') AS d FROM t{where} "
                       f"ORDER BY d LIMIT {limit}")
                got = db.execute(sql, mode)["rows"]
                ranked = sorted((math.dist(row[2], target), key)
                                for key, row in predicted.items())[:limit]
                self.assertEqual([row[0] for row in got], [key for _, key in ranked],
                                 f"ids for {sql}")
                for row, (distance, _) in zip(got, ranked):
                    self.assertAlmostEqual(row[1], distance, places=9)
                stats["query"] += 1

        # Final state must survive one more restart.
        db.close()
        db = Database(root)
        rows = db.execute("SELECT * FROM t ORDER BY id", "exact")["rows"]
        self.assertEqual(len(rows), len(model.rows))
        for got, (key, row) in zip(rows, sorted(model.rows.items())):
            self.assertEqual(got[0], key)
            self.assertEqual(got[1], row[0])
            self.assertEqual(got[2], row[1])
            for a, b in zip(got[3], row[2]):
                self.assertAlmostEqual(a, b, places=6)
        db.close()
        self.assertGreater(stats["insert"], 20, stats)
        self.assertGreater(stats["update"], 5, stats)
        self.assertGreater(stats["delete"], 5, stats)
        self.assertGreater(stats["query"], 20, stats)
        self.assertGreater(stats["reopen"], 3, stats)


if __name__ == "__main__":
    unittest.main()
