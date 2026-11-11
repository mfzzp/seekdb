"""README, VERIFICATION.md and conversation.json must agree with each other.

Three documents quote the same measurements, and the test count has been updated
many times, so a stale figure is likely to appear and hard to notice by eye.  This
test counts the tests in this directory, checks that both documents quote that
number, checks that no older count is still claimed, and checks that a curated set
of headline measurements appears in all three documents.

The count is derived from discovery rather than hard-coded, so adding a test
without updating the prose fails here.
"""

import json
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tests")
README = os.path.join(ROOT, "README.md")
VERIFICATION = os.path.join(ROOT, "VERIFICATION.md")
EVIDENCE = os.path.join(ROOT, "agent", "conversation.json")

# Headline measurements, spelled per document: README keeps one rounded figure
# while VERIFICATION.md and the evidence file carry the exact median, so the table
# records the spelling each document is allowed to use.
SHARED_FIGURES = [
    ("1000-query total, baseline", "7.4", "7.432", "7.432"),
    ("1000-query total, numpy", "0.95", "0.947", "0.947"),
    ("1000-query total, fallback", "7.2", "7.198", "7.198"),
    ("unfiltered query, baseline", "11.8", "11.79", "11.787"),
    ("unfiltered query, numpy", "1.13", "1.13", "1.132"),
    ("filtered query, numpy", "0.58", "0.58", "0.582"),
    ("scalar filter at 0.1%", "0.29", "0.29", "0.29"),
    ("matrix at 150k rows (MiB)", "146.5", "146.5", "146.5"),
    ("peak RSS at 2 GiB limit", "348", "348", "348"),
    ("peak RSS at 300k rows", "358", "358", "358"),
    ("threads without the BLAS limit", "64", "64", "64"),
    ("execution share of a request (us)", "645", "645", "645"),
]


def discover_count():
    loader = unittest.defaultTestLoader
    suite = loader.discover(TESTS, pattern="test_*.py", top_level_dir=TESTS)
    return suite.countTestCases()


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


class DocumentConsistencyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.readme = read(README)
        cls.verification = read(VERIFICATION)
        cls.evidence_text = read(EVIDENCE)
        cls.evidence = json.loads(cls.evidence_text)
        cls.count = discover_count()

    def test_documents_quote_the_current_test_count(self):
        needle = f"{self.count} regression tests"
        self.assertIn(needle, self.readme,
                      f"README must say {needle!r} (discovered {self.count})")
        self.assertIn(needle, self.verification,
                      f"VERIFICATION.md must say {needle!r} (discovered {self.count})")

    def test_no_stale_test_count_is_still_claimed(self):
        pattern = re.compile(r"(\d+) regression tests")
        for name, text in (("README", self.readme), ("VERIFICATION", self.verification)):
            for match in pattern.finditer(text):
                self.assertEqual(int(match.group(1)), self.count,
                                 f"{name} still claims {match.group(1)} regression tests")

    def test_headline_figures_appear_in_all_three_documents(self):
        missing = []
        for label, in_readme, in_verification, in_evidence in SHARED_FIGURES:
            for name, text, wanted in (("README", self.readme, in_readme),
                                       ("VERIFICATION", self.verification, in_verification),
                                       ("conversation", self.evidence_text, in_evidence)):
                if wanted not in text:
                    missing.append(f"{label}: {wanted} missing from {name}")
        self.assertEqual(missing, [], "figures out of sync: " + "; ".join(missing))

    def test_evidence_file_names_the_workspace_paths_consistently(self):
        server_dir = self.evidence["experiments"]
        ids = [entry["id"] for entry in server_dir]
        self.assertEqual(len(ids), len(set(ids)), "duplicate experiment ids")
        self.assertIn("float32-matrix-margin", ids,
                      "the latest fix should be recorded in the evidence file")

    def test_documents_do_not_promise_unverified_scores(self):
        # A submission must not claim platform scores or hidden-case results.
        forbidden = ("competition score:", "hidden case passed", "guaranteed to pass")
        for name, text in (("README", self.readme), ("VERIFICATION", self.verification)):
            lowered = text.lower()
            for phrase in forbidden:
                self.assertNotIn(phrase, lowered, f"{name} claims {phrase!r}")


if __name__ == "__main__":
    unittest.main()
