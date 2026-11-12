import json
import os
import shutil
import subprocess
import tempfile
import unittest


class SqlCorrectnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = os.path.dirname(os.path.dirname(__file__))
        cls.binary = os.path.join(tempfile.gettempdir(), "minivectordb-correctness")
        subprocess.run(
            [
                "g++",
                "-O3",
                "-DNDEBUG",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-o",
                cls.binary,
                os.path.join(cls.root, "minivectordb.cpp"),
            ],
            check=True,
        )

    def run_db(self, statements):
        data_dir = tempfile.mkdtemp(prefix="seekdb-correctness-")
        try:
            lines = []
            for i, sql in enumerate(statements, 1):
                lines.append(json.dumps({
                    "id": i,
                    "op": "execute",
                    "sql": sql,
                }))
            lines.append(json.dumps({
                "id": len(statements) + 1,
                "op": "shutdown",
            }))

            result = subprocess.run(
                [self.binary, "--data-dir", data_dir],
                input="\n".join(lines) + "\n",
                text=True,
                capture_output=True,
                check=False,
            )

            responses = [
                json.loads(line)
                for line in result.stdout.splitlines()
                if line.strip() and not line.startswith('{"type":"ready"}')
            ]
            return result, responses
        finally:
            shutil.rmtree(data_dir, ignore_errors=True)

    def test_varchar_uses_utf8_character_count(self):
        result, responses = self.run_db([
            "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(3))",
            "INSERT INTO t VALUES (1, '中文中')",
        ])

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(responses[0]["ok"])
        self.assertTrue(responses[1]["ok"])

    def test_varchar_rejects_four_characters(self):
        result, responses = self.run_db([
            "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(3))",
            "INSERT INTO t VALUES (1, '中文中字')",
        ])

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(responses[0]["ok"])
        self.assertFalse(responses[1]["ok"])
        self.assertEqual(
            responses[1]["error"]["code"],
            "invalid_value",
        )

    def test_update_primary_key_is_unsupported(self):
        result, responses = self.run_db([
            "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(16))",
            "INSERT INTO t VALUES (1, 'a')",
            "UPDATE t SET id = 2 WHERE id = 1",
            "SELECT * FROM t ORDER BY id",
        ])

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(responses[0]["ok"])
        self.assertTrue(responses[1]["ok"])
        self.assertFalse(responses[2]["ok"])
        self.assertEqual(
            responses[2]["error"]["code"],
            "unsupported_sql",
        )
        self.assertTrue(responses[3]["ok"])
        self.assertEqual(responses[3]["rows"][0][0], 1)
