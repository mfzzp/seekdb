"""Crash recovery under a real SIGKILL (POSIX) / TerminateProcess (Windows).

Two shapes matter: a kill between commits, where every acknowledged batch must
survive and the partially written suffix must be truncated, and a kill inside an
open transaction, where the uncommitted rows must not appear after restart.
The process is killed with SIGKILL, so it never runs its shutdown path.
"""

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest

SOURCE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Client:
    def __init__(self, data_dir):
        self.process = subprocess.Popen(
            [sys.executable, os.path.join(SOURCE, "minivectordb.py"),
             "--data-dir", data_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.sequence = 0
        self.process.stdout.readline()          # ready

    def ask(self, sql, mode="exact"):
        self.sequence += 1
        request = {"id": self.sequence, "op": "execute", "sql": sql,
                   "search_mode": mode}
        self.process.stdin.write((json.dumps(request) + "\n").encode())
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("server stopped answering")
        return json.loads(line)

    def _close_pipes(self):
        for stream in (self.process.stdin, self.process.stdout):
            try:
                stream.close()
            except (BrokenPipeError, OSError):
                pass

    def kill(self):
        if os.name == "nt":
            self.process.kill()
        else:
            os.kill(self.process.pid, signal.SIGKILL)
        self.process.wait(timeout=30)
        self._close_pipes()

    def shutdown(self):
        request = {"id": self.sequence + 1, "op": "shutdown"}
        self.process.stdin.write((json.dumps(request) + "\n").encode())
        self.process.stdin.flush()
        self.process.stdout.readline()
        self.process.wait(timeout=30)
        self._close_pipes()


class CrashRecoveryTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.data = os.path.join(self.root.name, "data")

    def test_kill_between_commits_keeps_acknowledged_batches(self):
        client = Client(self.data)
        client.ask("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(4));")
        acknowledged = 0
        for batch in range(6):
            rows = ",".join(f"({batch * 10 + index},'[0,0,0,0]')" for index in range(10))
            response = client.ask(f"INSERT INTO t VALUES {rows};")
            self.assertIs(response["ok"], True)
            self.assertEqual(response["affected_rows"], 10)
            acknowledged += 10
        client.kill()                      # no shutdown, no flush, no cleanup

        restarted = Client(self.data)
        response = restarted.ask("SELECT id FROM t ORDER BY id;")
        self.assertIs(response["ok"], True)
        self.assertEqual(len(response["rows"]), acknowledged,
                         "every acknowledged batch must survive a SIGKILL")
        # the log must still be usable afterwards
        self.assertIs(restarted.ask("INSERT INTO t VALUES (999,'[1,1,1,1]');")["ok"], True)
        self.assertEqual(len(restarted.ask("SELECT id FROM t ORDER BY id;")["rows"]),
                         acknowledged + 1)
        restarted.shutdown()

    def test_kill_inside_transaction_drops_uncommitted_rows(self):
        client = Client(self.data)
        client.ask("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(4));")
        self.assertEqual(client.ask("INSERT INTO t VALUES (1,'[0,0,0,0]');")
                         ["affected_rows"], 1)
        client.ask("BEGIN;")
        for key in range(2, 6):
            self.assertIs(client.ask(f"INSERT INTO t VALUES ({key},'[0,0,0,0]');")["ok"],
                          True)
        client.kill()

        restarted = Client(self.data)
        response = restarted.ask("SELECT id FROM t ORDER BY id;")
        self.assertEqual(response["rows"], [[1]],
                         "uncommitted rows must not survive a SIGKILL")
        self.assertIs(restarted.ask("INSERT INTO t VALUES (7,'[0,0,0,0]');")["ok"], True)
        self.assertEqual(restarted.ask("SELECT id FROM t ORDER BY id;")["rows"],
                         [[1], [7]])
        restarted.shutdown()

    def test_kill_during_bulk_load_leaves_no_partial_batch(self):
        client = Client(self.data)
        client.ask("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(4));")
        committed = 0
        for batch in range(3):
            rows = ",".join(f"({batch * 100 + index},'[0,0,0,0]')" for index in range(100))
            self.assertIs(client.ask(f"INSERT INTO t VALUES {rows};")["ok"], True)
            committed += 100
        # one large statement, killed while it is still being processed
        big = 100000
        rows = ",".join(f"({1000 + index},'[0,0,0,0]')" for index in range(big))
        request = json.dumps({"id": 999, "op": "execute",
                              "sql": f"INSERT INTO t VALUES {rows};",
                              "search_mode": "exact"}) + "\n"
        client.process.stdin.write(request.encode())
        client.process.stdin.flush()
        time.sleep(0.2)
        client.kill()

        restarted = Client(self.data)
        ids = [row[0] for row in restarted.ask("SELECT id FROM t ORDER BY id;")["rows"]]
        # Either the statement never committed (only the earlier batches remain)
        # or it committed entirely.  A partially applied statement or a gap in the
        # primary keys would both be durability bugs.
        self.assertIn(len(ids), (committed, committed + big),
                      f"unexpected row count {len(ids)}")
        self.assertEqual(ids, list(range(len(ids))),
                         "recovered rows must be a gap-free prefix")
        self.assertIs(restarted.ask("INSERT INTO t VALUES (999999,'[0,0,0,0]');")["ok"],
                      True)
        restarted.shutdown()

    def test_torn_log_suffix_is_truncated_on_recovery(self):
        """A partially written frame must be discarded, deterministically."""
        client = Client(self.data)
        client.ask("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(4));")
        for key in range(5):
            self.assertIs(client.ask(f"INSERT INTO t VALUES ({key},'[0,0,0,0]');")["ok"],
                          True)
        client.shutdown()
        log = os.path.join(self.data, "rows.log")
        size = os.path.getsize(log)
        with open(log, "ab") as handle:
            handle.write((4096).to_bytes(8, "little"))    # promises 4096 bytes
            handle.write(b"r")                            # tag
            handle.write(b"\x00" * 64)                    # far too few bytes
        self.assertGreater(os.path.getsize(log), size)

        restarted = Client(self.data)
        response = restarted.ask("SELECT id FROM t ORDER BY id;")
        self.assertEqual([row[0] for row in response["rows"]], list(range(5)))
        self.assertEqual(os.path.getsize(log), size,
                         "recovery must truncate the torn suffix")
        self.assertIs(restarted.ask("INSERT INTO t VALUES (6,'[0,0,0,0]');")["ok"], True)
        restarted.shutdown()
        again = Client(self.data)
        self.assertEqual([row[0] for row in again.ask("SELECT id FROM t ORDER BY id;")["rows"]],
                         [0, 1, 2, 3, 4, 6])
        again.shutdown()


if __name__ == "__main__":
    unittest.main()
