"""Run the repository's own judge against an extra case package.

`protocol_check/run.py` is the public half of the judge: the same transport, the
same `type`-strict comparison and the same 1e-5 float tolerance.  The extra
package under tests/extra_cases/ adds shapes the twenty public cases do not
cover - range and AND predicates, a restart with a vector index, a failed
statement inside a transaction, LIMIT 0 and an empty result, non-ASCII values,
and ann versus exact on the same data - with expectations derived by hand from
the SQL semantics rather than captured from the implementation.

The test skips when the judge package is not present (for example when this tree
is used outside the competition checkout).
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, "protocol_check", "run.py")
PACKAGE = os.path.join(ROOT, "tests", "extra_cases")


@unittest.skipUnless(os.path.isfile(RUNNER) and os.path.isfile(
    os.path.join(PACKAGE, "manifest.json")),
    "protocol_check or the extra case package is not available")
class JudgePackageTest(unittest.TestCase):
    def test_extra_cases_pass_the_official_runner(self):
        output = tempfile.mkdtemp(prefix="extra-cases-")
        command = json.dumps([sys.executable, os.path.join(ROOT, "minivectordb.py"),
                              "--data-dir", "{data_dir}"])
        process = subprocess.run(
            [sys.executable, RUNNER, "--package", PACKAGE,
             "--command", command, "--output", os.path.join(output, "report")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600,
            cwd=ROOT)
        text = process.stdout.decode("utf-8", "replace")
        report_path = os.path.join(output, "report", "result.json")
        self.assertTrue(os.path.isfile(report_path),
                        f"runner produced no report:\n{text[-2000:]}")
        with open(report_path, encoding="utf-8") as handle:
            report = json.load(handle)
        failed = [case for case in report["cases"] if case["status"] != "PASS"]
        self.assertEqual(failed, [], f"failing cases: {failed}\n{text[-2000:]}")
        self.assertTrue(report["passed"])
        self.assertEqual(len(report["cases"]), 7)


if __name__ == "__main__":
    unittest.main()
