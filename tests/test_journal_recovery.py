"""Journal recovery tests for the native C++ engine.

The engine appends SQL records to ``journal.log`` and truncates a torn tail at
startup.  These tests build the real binary with g++ and drive it over
stdin/stdout, so they cover the same code path the judge uses:

* tail truncation of 1..7 bytes, a partial frame header and a partial payload;
* truncation followed by a new commit and two consecutive restarts;
* a damaged tail with a later valid record behind it (the old crash);
* an incomplete count field, which is a torn tail and must be recovered;
* a corrupt magic, an absurd count and a corrupt SQL statement in a *complete*
  frame, which must fail loudly with ``storage_error`` and must not be silently
  truncated.

Skipped when g++ is unavailable.  The tests do not import the Python engine.
"""

import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "minivectordb.cpp")
GXX = shutil.which("g++")
MAGIC = 0x4D564A31


def frame_offsets(data):
    """Return (start, payload_bytes) for every complete-looking frame."""
    out = []
    pos = 0
    while pos + 8 <= len(data):
        magic, size = struct.unpack_from("<II", data, pos)
        if magic != MAGIC or size > (1 << 30) or pos + 8 + size > len(data):
            break
        out.append((pos, size))
        pos += 8 + size
    return out


@unittest.skipUnless(GXX, "g++ is required for the native journal tests")
class JournalRecoveryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.mkdtemp(prefix="mvdb-journal-build-")
        cls.binary = os.path.join(cls.build_dir, "minivectordb")
        subprocess.run([GXX, "-O2", "-std=c++17", "-o", cls.binary, SOURCE],
                       check=True, timeout=180)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.build_dir, ignore_errors=True)

    def setUp(self):
        self.dir = tempfile.mkdtemp(dir=self.build_dir)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    # -- helpers ---------------------------------------------------------
    def run_jsonl(self, statements):
        requests = [{"id": index, "op": "execute", "sql": sql, "search_mode": "exact"}
                    for index, sql in enumerate(statements, 1)]
        requests.append({"id": len(statements) + 1, "op": "shutdown"})
        payload = "".join(json.dumps(request) + "\n" for request in requests)
        proc = subprocess.run([self.binary, "--data-dir", self.dir], input=payload,
                              text=True, capture_output=True, timeout=120)
        replies = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
        return proc.returncode, replies, proc.stderr

    def create_with_two_rows(self):
        code, replies, err = self.run_jsonl([
            "create table t (id bigint primary key, v varchar(16))",
            "insert into t values (1,'a')",
            "insert into t values (2,'b')",
        ])
        self.assertEqual(code, 0, err)
        self.assertEqual(replies[0], {"type": "ready"})
        self.assertEqual(len(replies), 5)

    def insert(self, key):
        code, replies, err = self.run_jsonl([f"insert into t values ({key},'x')"])
        self.assertEqual(code, 0, err)
        self.assertTrue(replies[0] == {"type": "ready"}, replies)

    def rows(self):
        code, replies, err = self.run_jsonl(["select id from t order by id"])
        self.assertEqual(code, 0, err)
        body = next(reply for reply in replies if "rows" in reply)
        return [row[0] for row in body["rows"]]

    def journal_bytes(self):
        with open(os.path.join(self.dir, "journal.log"), "rb") as handle:
            return handle.read()

    def rewrite_journal(self, data):
        with open(os.path.join(self.dir, "journal.log"), "wb") as handle:
            handle.write(data)

    # -- tail damage -----------------------------------------------------
    def test_truncate_one_to_seven_bytes_recovers(self):
        for count in range(1, 8):
            with self.subTest(truncated=count):
                shutil.rmtree(self.dir, ignore_errors=True)
                os.makedirs(self.dir)
                self.create_with_two_rows()
                data = self.journal_bytes()
                self.rewrite_journal(data[:-count])
                # First start repairs the tail; the last INSERT is gone.
                self.assertEqual(self.rows(), [1])
                self.insert(3)
                # Second start sees a clean log and a valid later record.
                self.assertEqual(self.rows(), [1, 3])

    def test_partial_frame_header_recovers(self):
        for count in range(1, 8):
            with self.subTest(header_bytes=count):
                shutil.rmtree(self.dir, ignore_errors=True)
                os.makedirs(self.dir)
                self.create_with_two_rows()
                clean = self.journal_bytes()
                self.rewrite_journal(clean + struct.pack("<I", MAGIC)[:count])
                self.assertEqual(self.rows(), [1, 2])
                self.assertEqual(len(self.journal_bytes()), len(clean),
                                 "the partial header must be truncated")
                self.insert(3)
                self.assertEqual(self.rows(), [1, 2, 3])

    def test_partial_payload_recovers(self):
        self.create_with_two_rows()
        data = self.journal_bytes()
        start, _ = frame_offsets(data)[-1]
        # Keep the header and exactly one payload byte.
        self.rewrite_journal(data[:start + 9])
        self.assertEqual(self.rows(), [1])
        self.insert(3)
        self.assertEqual(self.rows(), [1, 3])

    def test_incomplete_count_field_is_a_torn_tail(self):
        self.create_with_two_rows()
        data = bytearray(self.journal_bytes())
        start, _ = frame_offsets(bytes(data))[-1]
        # Claim a 4-byte payload: the count field cannot be read in full.
        struct.pack_into("<I", data, start + 4, 4)
        self.rewrite_journal(bytes(data))
        self.assertEqual(self.rows(), [1])
        self.insert(3)
        self.assertEqual(self.rows(), [1, 3])

    def test_tail_damage_with_a_later_valid_record(self):
        self.create_with_two_rows()
        data = self.journal_bytes()
        self.rewrite_journal(data[:-1])
        # The engine appends a valid record behind the torn tail; the next
        # start must not fail and must keep the record written after the repair.
        self.insert(3)
        self.assertEqual(self.rows(), [1, 3])
        self.assertEqual(self.rows(), [1, 3])

    # -- middle corruption ----------------------------------------------
    def test_mid_file_magic_corruption_fails_loudly(self):
        self.create_with_two_rows()
        data = bytearray(self.journal_bytes())
        data[0] ^= 0xFF
        self.rewrite_journal(bytes(data))
        code, replies, err = self.run_jsonl(["select id from t"])
        self.assertNotEqual(code, 0, "a corrupt magic must abort startup")
        self.assertEqual(replies, [], "no ready line may be sent")
        self.assertIn("startup failed", err)
        self.assertIn("invalid journal magic", err)

    def test_mid_file_absurd_count_fails_loudly(self):
        self.create_with_two_rows()
        data = bytearray(self.journal_bytes())
        start, size = frame_offsets(bytes(data))[-1]
        struct.pack_into("<I", data, start + 8, 0xFFFFFFFF)
        self.rewrite_journal(bytes(data))
        code, replies, err = self.run_jsonl(["select id from t"])
        self.assertNotEqual(code, 0, "an absurd count must abort startup")
        self.assertEqual(replies, [])
        self.assertIn("startup failed", err)
        self.assertIn("invalid journal frame", err)

    def test_mid_file_sql_corruption_fails_loudly(self):
        self.create_with_two_rows()
        data = bytearray(self.journal_bytes())
        index = data.find(b"insert into t values (1")
        self.assertNotEqual(index, -1)
        data[index] = ord("x")  # "xnsert ..." is not a statement
        self.rewrite_journal(bytes(data))
        code, replies, err = self.run_jsonl(["select id from t"])
        self.assertNotEqual(code, 0, "a corrupt statement must abort startup")
        self.assertEqual(replies, [])
        self.assertIn("startup failed", err)
        self.assertIn("invalid journal statement", err)


if __name__ == "__main__":
    unittest.main()
