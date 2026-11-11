"""Startup diagnostics must go to stderr and stdout must stay protocol-only.

The judge reads stdout as JSONL, so any diagnostic printed there would break the
handshake.  This runs the real program as a subprocess and inspects both streams.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

import minivectordb

SOURCE = os.path.dirname(os.path.abspath(minivectordb.__file__))


class StartupDiagnosticsTest(unittest.TestCase):
    def test_stdout_is_protocol_only_and_diagnostics_go_to_stderr(self):
        data_dir = tempfile.mkdtemp(prefix="diag-")
        request = json.dumps({"id": 1, "op": "execute",
                              "sql": "SELECT 1", "search_mode": "exact"}) + "\n"
        request += json.dumps({"id": 2, "op": "shutdown"}) + "\n"
        env = dict(os.environ)
        env["PYTHONPATH"] = os.pathsep.join(
            [SOURCE] + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
        proc = subprocess.run(
            [sys.executable, os.path.join(SOURCE, "minivectordb.py"),
             "--data-dir", data_dir],
            input=request.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=60, env=env)
        stdout_lines = proc.stdout.decode("utf-8").splitlines()
        stderr = proc.stderr.decode("utf-8")

        self.assertEqual(len(stdout_lines), 3, stdout_lines)
        self.assertEqual(json.loads(stdout_lines[0]), {"type": "ready"})
        first = json.loads(stdout_lines[1])
        self.assertEqual(first["id"], 1)
        self.assertIn("ok", first)
        self.assertEqual(json.loads(stdout_lines[2]), {"id": 2, "ok": True})
        self.assertEqual(proc.returncode, 0)

        self.assertIn("minivectordb:", stderr)
        self.assertIn("python ", stderr)
        self.assertIn("numpy=", stderr)
        self.assertIn("matrix_cap=", stderr)
        self.assertIn("memory_budget=", stderr)
        # The diagnostics must not leak into stdout.
        for line in stdout_lines:
            self.assertNotIn("minivectordb:", line)


if __name__ == "__main__":
    unittest.main()
