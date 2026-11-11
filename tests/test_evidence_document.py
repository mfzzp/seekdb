"""agent/conversation.json is a judged deliverable, so its shape is tested.

The file mixes two generations of experiment records (early ones carry id/source/
output, later ones add problem/method/results/limitations), so the checks are the
invariants that hold for all of them: valid UTF-8 JSON without a BOM, the required
top-level sections, unique ids, no placeholder text, an explicit non-synthetic
flag, verbatim participant quotes where they are claimed, and a size budget.

The size budget is self-imposed. The format allows any size, so the budget exists
only to force a deliberate decision when the file grows; it was 128 KiB when the
file was about 99 KiB and was raised to 160 KiB at 49 experiments, because the
recorded measurements and their limitations are worth more than an arbitrary cap.
Trim or merge entries before raising it again.
"""

import json
import os
import unittest

PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "agent", "conversation.json")
SIZE_BUDGET = 160 * 1024
PLACEHOLDERS = ("TODO", "TBD", "FIXME", "XXX", "lorem ipsum")


class EvidenceDocumentTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(PATH, "rb") as handle:
            cls.raw = handle.read()
        cls.data = json.loads(cls.raw.decode("utf-8"))

    def test_encoding_and_framing(self):
        self.assertFalse(self.raw.startswith(b"\xef\xbb\xbf"), "file must not start with a BOM")
        self.assertTrue(self.raw.endswith(b"\n"), "file must end with a newline")
        self.assertNotIn(b"\r\n", self.raw,
                         "the file must use LF; a CRLF rewrite adds about a kilobyte "
                         "and makes diffs noisy")
        self.raw.decode("utf-8")          # raises if the file is not valid UTF-8

    def test_required_sections(self):
        for key in ("scope", "synthetic", "conversations", "student_statements",
                    "experiments", "limitations", "learning_cases"):
            self.assertIn(key, self.data, f"missing section {key}")

    def test_attribution_flag_is_explicit(self):
        self.assertIs(self.data["synthetic"], False,
                      "the submission must declare whether the material is synthetic")
        self.assertTrue(self.data["scope"].strip(), "scope must explain the attribution")

    def test_experiments_are_identified_and_sourced(self):
        ids = []
        for index, entry in enumerate(self.data["experiments"]):
            self.assertTrue(entry.get("id"), f"experiment {index} has no id")
            self.assertTrue(entry.get("source"), f"{entry['id']} has no source")
            # The records span two generations of schema, so the invariant is that
            # an entry carries at least one non-empty content field, not that it
            # uses a particular key name.
            substantive = [key for key in entry
                           if key not in ("id", "source")
                           and entry[key] not in (None, "", [], {})]
            self.assertTrue(substantive,
                            f"{entry['id']} records no substance beyond id/source")
            ids.append(entry["id"])
        duplicates = {value for value in ids if ids.count(value) > 1}
        self.assertEqual(duplicates, set(), f"duplicate experiment ids: {duplicates}")
        self.assertGreaterEqual(len(ids), 40, "the evidence file should keep every experiment")

    def test_participant_quotes_are_verbatim_and_present(self):
        for statement in self.data["student_statements"]:
            self.assertTrue(statement.get("verbatim", "").strip(),
                            f"{statement.get('id')} claims a participant statement "
                            f"without the verbatim text")
        for case in self.data["learning_cases"]:
            contribution = case.get("student_contribution") or {}
            self.assertTrue(str(contribution.get("verbatim", "")).strip(),
                            f"{case.get('id')} learning case has no verbatim contribution")

    def test_no_placeholder_text(self):
        text = json.dumps(self.data, ensure_ascii=False)
        for marker in PLACEHOLDERS:
            self.assertNotIn(marker, text, f"placeholder marker {marker!r} in the evidence file")

    def test_size_budget(self):
        self.assertLess(len(self.raw), SIZE_BUDGET,
                        f"conversation.json is {len(self.raw)} bytes; the budget is "
                        f"{SIZE_BUDGET}. Trim or merge entries rather than raising it.")


if __name__ == "__main__":
    unittest.main()
