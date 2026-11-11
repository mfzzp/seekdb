"""Replay the golden protocol transcript and fail on any behavioural drift.

tests/protocol_golden.json was captured from the implementation after each of its
decisions had been reviewed against the specification: bare COMMIT/ROLLBACK are
no-ops, "<>" and parenthesised predicates and qualified column references are
unsupported, ORDER BY <distance expression> is accepted, and two different
distance targets in one projection keep their own values.

This is a lock-in test, not an oracle: it cannot show that an expectation is
right, only that the behaviour did not change silently.  Floats compare with a
relative tolerance because the numpy and pure-Python kernels may differ in the
last bits.
"""

import json
import os
import tempfile
import unittest

from minivectordb import Database

GOLDEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "protocol_golden.json")


def close(left, right):
    if isinstance(left, bool) or isinstance(right, bool):
        return left == right
    if isinstance(left, float) or isinstance(right, float):
        try:
            return abs(float(left) - float(right)) <= 1e-9 * max(
                1.0, abs(float(left)), abs(float(right)))
        except (TypeError, ValueError):
            return False
    if isinstance(left, (list, tuple)) and isinstance(right, (list, tuple)):
        # The in-process result keeps vectors as tuples; JSON stores arrays.
        return len(left) == len(right) and all(close(a, b) for a, b in zip(left, right))
    return left == right


class GoldenProtocolTest(unittest.TestCase):
    def test_transcript_matches_golden_responses(self):
        with open(GOLDEN, encoding="utf-8") as handle:
            golden = json.load(handle)
        self.assertGreater(len(golden), 80, "the golden transcript looks truncated")
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        db = Database(root.name)
        self.addCleanup(db.close)
        for index, entry in enumerate(golden, 1):
            sql = entry["sql"]
            with self.subTest(step=index, sql=sql):
                try:
                    result = db.execute(sql, "exact")
                except Exception as exc:  # noqa: BLE001 - expected for error entries
                    self.assertFalse(entry["ok"],
                                     f"step {index} {sql!r} failed unexpectedly: {exc}")
                    self.assertTrue(getattr(exc, "code", None),
                                    f"step {index} raised without a code: {exc}")
                    continue
                self.assertTrue(entry["ok"], f"step {index} {sql!r} should have failed")
                if "columns" in entry:
                    self.assertEqual(result.get("columns"), entry["columns"],
                                     f"step {index} {sql!r} columns")
                if "rows" in entry:
                    self.assertTrue(close(result.get("rows"), entry["rows"]),
                                    f"step {index} {sql!r} rows: "
                                    f"{result.get('rows')!r} != {entry['rows']!r}")
                if "affected_rows" in entry:
                    self.assertEqual(result.get("affected_rows"), entry["affected_rows"],
                                     f"step {index} {sql!r} affected_rows")


if __name__ == "__main__":
    unittest.main()
