"""Protocol-level conformance: response shapes, failure resilience, read-only run.

The judge sends statements that are expected to fail and runs the image with a
read-only root filesystem, so the program is exercised here as a real subprocess:
every response must carry an int id and a JSON boolean ok, a success must expose
either columns/rows or an int affected_rows, a failure must expose a non-empty
error code, and no failing statement may end the process.

The read-only case is skipped on Windows, where directory permissions do not
prevent writes.
"""

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

SOURCE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.dirname(SOURCE)          # repository root holding minivectordb.py

STATEMENTS = [
    "CREATE TABLE docs (id BIGINT PRIMARY KEY, title VARCHAR(16), v VECTOR(3));",
    "INSERT INTO docs VALUES (1,'a','[0,0,0]'),(2,'b','[1,0,0]');",
    "CREATE VECTOR INDEX vi ON docs(v);",
    "SELECT * FROM docs ORDER BY id;",
    "SELECT id, L2_DISTANCE(v,'[0,0,0]') AS d FROM docs ORDER BY d LIMIT 1;",
    "SELECT id FROM docs ORDER BY id LIMIT 0;",
    "SELECT id FROM docs ORDER BY id LIMIT 100;",
    "SELECT id FROM docs WHERE id = 999 ORDER BY id;",
    "UPDATE docs SET title = 'z' WHERE id = 1;",
    "UPDATE docs SET title = 'z' WHERE id = 4242;",
    "DELETE FROM docs WHERE id = 4242;",
    "INSERT INTO docs VALUES (1,'dup','[0,0,0]');",
    "SELECT id FROM missing_table;",
    "SELECT missing_column FROM docs;",
    "SELECT id FROM docs WHERE id = 'not-an-int';",
    "INSERT INTO docs VALUES (3,'c');",
    "INSERT INTO docs VALUES (4,'way-too-long-title-value','[0,0,0]');",
    "UPDATE docs SET v = '[9,9]' WHERE id = 1;",
    "CREATE VECTOR INDEX vi ON docs(v);",
    "CREATE TABLE docs (id BIGINT PRIMARY KEY);",
    "COMMIT;",
    "ROLLBACK;",
    "BEGIN;",
    "SELECT id FROM docs ORDER BY id;",
    "COMMIT;",
    "nonsense syntax here;",
    "SELECT id FROM docs ORDER BY title LIMIT 1;",
]


def run_session(requests, data_dir, app_dir=None, env_extra=None):
    env = dict(os.environ)
    env.update(env_extra or {})
    script = os.path.join(app_dir or SOURCE, "minivectordb.py")
    process = subprocess.run(
        [sys.executable, script, "--data-dir", data_dir],
        input=b"".join((json.dumps(request) + "\n").encode() for request in requests),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180, env=env)
    return process


class ProtocolConformanceTest(unittest.TestCase):
    def test_semicolons_and_newlines_inside_sql_strings(self):
        """The specification warns that a semicolon may sit inside a string literal.

        A request must therefore be split by JSON, never by semicolons, and a newline
        inside a SQL string has to come back JSON-escaped so the response stays one
        physical line.
        """
        data_dir = tempfile.mkdtemp(prefix="semicolon-")
        self.addCleanup(shutil.rmtree, data_dir, True)
        requests = [
            {"id": 1, "op": "execute", "search_mode": "exact",
             "sql": "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(32));"},
            {"id": 2, "op": "execute", "search_mode": "exact",
             "sql": "INSERT INTO docs VALUES (1, 'a;b'), (2, 'x;y;z');"},
            {"id": 3, "op": "execute", "search_mode": "exact",
             "sql": "INSERT INTO docs VALUES (3, 'quote''semi;here');"},
            {"id": 4, "op": "execute", "search_mode": "exact",
             "sql": "INSERT INTO docs VALUES (4, 'two\nlines');"},   # a real newline
            {"id": 5, "op": "execute", "search_mode": "exact",
             "sql": "SELECT id, content FROM docs ORDER BY id;"},
            {"id": 6, "op": "execute", "search_mode": "exact",
             "sql": "SELECT id FROM docs WHERE content = 'x;y;z';"},
            {"id": 7, "op": "shutdown"},
        ]
        process = run_session(requests, data_dir)
        lines = process.stdout.decode("utf-8").splitlines()
        self.assertEqual(len(lines), 8, f"one ready line plus seven responses: {lines}")
        responses = [json.loads(line) for line in lines]
        self.assertEqual(responses[0], {"type": "ready"})
        self.assertEqual(responses[5]["rows"], [
            [1, "a;b"], [2, "x;y;z"], [3, "quote'semi;here"], [4, "two\nlines"]])
        self.assertEqual(responses[6]["rows"], [[2]],
                         "an equality filter on a value containing semicolons")
        # The newline from row 4 must be escaped inside the response line, which is
        # why splitting on newlines yields exactly one JSON object per request.
        self.assertIn("two\\nlines", lines[5])

    def test_startup_failure_is_a_clean_exit_without_ready(self):
        """An unusable data directory must fail before the ready handshake.

        The judge reads the first stdout line as the handshake, so writing ready and
        then dying would look like a running database.  A path under an existing
        file fails on both platforms.
        """
        scratch = tempfile.mkdtemp(prefix="baddir-")
        self.addCleanup(shutil.rmtree, scratch, True)
        blocker = os.path.join(scratch, "not-a-directory")
        with open(blocker, "w", encoding="utf-8") as handle:
            handle.write("x")
        process = subprocess.run(
            [sys.executable, os.path.join(SOURCE, "minivectordb.py"),
             "--data-dir", os.path.join(blocker, "data")],
            input=b'{"id":1,"op":"shutdown"}\n', stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=60)
        self.assertNotEqual(process.returncode, 0, "a failed start must not exit 0")
        self.assertEqual(process.stdout, b"", "no handshake may be written")
        self.assertIn(b"data directory", process.stderr,
                      process.stderr.decode("utf-8", "replace"))

    def test_blank_lines_do_not_produce_responses(self):
        """A blank or whitespace-only line is not a request and must be skipped.

        Answering it would emit a response with no id and shift every later
        request/response pairing for a judge that counts answers.
        """
        data_dir = tempfile.mkdtemp(prefix="blank-")
        self.addCleanup(shutil.rmtree, data_dir, True)
        lines = [
            '{"id":1,"op":"execute","sql":"CREATE TABLE t (id BIGINT PRIMARY KEY);",'
            '"search_mode":"exact"}',
            "",
            "   ",
            "\t",
            '{"id":2,"op":"execute","sql":"INSERT INTO t VALUES (1);","search_mode":"exact"}',
            "",
            '{"id":3,"op":"execute","sql":"SELECT id FROM t;","search_mode":"exact"}',
            '{"id":4,"op":"shutdown"}',
        ]
        payload = ("\n".join(lines) + "\n").encode()
        process = subprocess.run(
            [sys.executable, os.path.join(SOURCE, "minivectordb.py"),
             "--data-dir", data_dir],
            input=payload, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=60)
        responses = [json.loads(line) for line in
                     process.stdout.decode("utf-8").splitlines()]
        self.assertEqual(responses[0], {"type": "ready"})
        self.assertEqual(len(responses), 5, f"one response per request only: {responses}")
        self.assertEqual([response.get("id") for response in responses[1:]], [1, 2, 3, 4])
        self.assertEqual(responses[3]["rows"], [[1]])

    def test_malformed_non_blank_line_is_answered(self):
        data_dir = tempfile.mkdtemp(prefix="malformed-")
        self.addCleanup(shutil.rmtree, data_dir, True)
        payload = b'42\n{"id":2,"op":"shutdown"}\n'
        process = subprocess.run(
            [sys.executable, os.path.join(SOURCE, "minivectordb.py"),
             "--data-dir", data_dir],
            input=payload, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=60)
        responses = [json.loads(line) for line in
                     process.stdout.decode("utf-8").splitlines()]
        self.assertEqual(len(responses), 3, responses)
        self.assertIs(responses[1].get("ok"), False)
        self.assertIs(type((responses[1].get("error") or {}).get("code")), str)
        self.assertEqual(responses[2], {"id": 2, "ok": True})

    def test_every_response_has_the_documented_shape(self):
        data_dir = tempfile.mkdtemp(prefix="sweep-")
        self.addCleanup(shutil.rmtree, data_dir, True)
        requests = [{"id": index, "op": "execute", "sql": sql,
                     "search_mode": "exact"}
                    for index, sql in enumerate(STATEMENTS, 1)]
        requests.append({"id": 999, "op": "shutdown"})
        process = run_session(requests, data_dir)
        lines = process.stdout.decode("utf-8").splitlines()
        self.assertEqual(len(lines), len(requests) + 1, lines)
        self.assertEqual(json.loads(lines[0]), {"type": "ready"})
        for index, sql in enumerate(STATEMENTS, 1):
            response = json.loads(lines[index])
            self.assertEqual(response.get("id"), index, response)
            self.assertIs(type(response.get("id")), int, response)
            self.assertIs(type(response.get("ok")), bool, response)
            if response["ok"]:
                if "affected_rows" in response:
                    self.assertIs(type(response["affected_rows"]), int, response)
                elif "columns" in response or "rows" in response:
                    self.assertIs(type(response.get("columns")), list, response)
                    self.assertIs(type(response.get("rows")), list, response)
                # DDL and transaction control legitimately answer with id/ok only.
            else:
                code = (response.get("error") or {}).get("code")
                self.assertIs(type(code), str, response)
                self.assertTrue(code, response)
        self.assertEqual(json.loads(lines[-1]), {"id": 999, "ok": True})
        self.assertEqual(process.returncode, 0)

    def test_statement_results_are_semantically_right(self):
        data_dir = tempfile.mkdtemp(prefix="semantics-")
        self.addCleanup(shutil.rmtree, data_dir, True)
        requests = [
            {"id": 1, "op": "execute", "sql": STATEMENTS[0], "search_mode": "exact"},
            {"id": 2, "op": "execute", "sql": STATEMENTS[1], "search_mode": "exact"},
            {"id": 3, "op": "execute", "sql": "SELECT * FROM docs ORDER BY id;",
             "search_mode": "exact"},
            {"id": 4, "op": "execute", "sql": "SELECT id FROM docs ORDER BY id LIMIT 0;",
             "search_mode": "exact"},
            {"id": 5, "op": "execute", "sql": "SELECT id FROM docs WHERE id = 999 ORDER BY id;",
             "search_mode": "exact"},
            {"id": 6, "op": "shutdown"},
        ]
        process = run_session(requests, data_dir)
        lines = [json.loads(line) for line in process.stdout.decode("utf-8").splitlines()]
        # lines[0] is the ready handshake, so request N is at index N.
        self.assertEqual(lines[3]["columns"], ["id", "title", "v"])
        self.assertEqual(lines[3]["rows"], [[1, "a", [0.0, 0.0, 0.0]],
                                            [2, "b", [1.0, 0.0, 0.0]]])
        self.assertEqual(lines[4]["rows"], [])
        self.assertEqual(lines[4]["columns"], ["id"])
        self.assertEqual(lines[5]["rows"], [])
        self.assertEqual(lines[5]["columns"], ["id"])

    @unittest.skipIf(os.name == "nt", "directory permissions do not block writes on Windows")
    def test_read_only_application_directory(self):
        root = tempfile.mkdtemp(prefix="readonly-")
        self.addCleanup(lambda: (shutil.rmtree(root, True)))
        app = os.path.join(root, "app")
        data = os.path.join(root, "data")
        os.makedirs(app)
        os.makedirs(data)
        for name in ("minivectordb.py", "minivector_sql.py", "minivector_store.py"):
            shutil.copy(os.path.join(SOURCE, name), os.path.join(app, name))
            os.chmod(os.path.join(app, name), stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
        os.chmod(app, stat.S_IRUSR | stat.S_IXUSR | stat.S_IRGRP | stat.S_IXGRP)
        self.addCleanup(os.chmod, app, stat.S_IRWXU)

        requests = [
            {"id": 1, "op": "execute",
             "sql": "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));",
             "search_mode": "exact"},
            {"id": 2, "op": "execute", "sql": "INSERT INTO docs VALUES (1, '你好');",
             "search_mode": "exact"},
            {"id": 3, "op": "execute", "sql": "SELECT id, content FROM docs ORDER BY id;",
             "search_mode": "exact"},
            {"id": 4, "op": "shutdown"},
        ]
        process = run_session(requests, data, app_dir=app,
                              env_extra={"PYTHONDONTWRITEBYTECODE": "1"})
        self.assertEqual(process.returncode, 0, process.stderr.decode("utf-8"))
        lines = process.stdout.decode("utf-8").splitlines()
        self.assertEqual(len(lines), 5, lines)
        self.assertEqual(json.loads(lines[0]), {"type": "ready"})
        self.assertEqual(json.loads(lines[3])["rows"], [[1, "你好"]])
        for directory, _, files in os.walk(app):
            self.assertNotIn("__pycache__", directory)
            self.assertFalse([name for name in files if name.endswith(".pyc")])


if __name__ == "__main__":
    unittest.main()
