"""The cgroup limit is read from the process's own cgroup, not just the root.

Docker mounts the container's cgroup at /sys/fs/cgroup, so the old root-path read
worked there.  It returned None, though, whenever the process sat in a nested
cgroup - a systemd user scope on this host, a Kubernetes pod elsewhere - and the
budget then fell back to "unlimited" inside a container that does have a limit.

These tests drive the reader against a fake cgroup tree, including the case where a
scope says "max" and the slice above it carries the real limit.
"""

import os
import tempfile
import unittest

from minivector_store import _cgroup_memory_files, memory_limit_bytes

GIB = 2 ** 30


class CgroupLimitTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.base = os.path.join(self.root.name, "cgroup")
        os.makedirs(self.base)
        self.proc = os.path.join(self.root.name, "cgroup_proc")
        self.write_proc("/user.slice/user-1002.slice/run-abc.scope")

    def write_proc(self, relative):
        with open(self.proc, "w", encoding="ascii") as handle:
            handle.write(f"0::{relative}\n")

    def write_limit(self, relative, text):
        path = os.path.join(self.base, relative.strip("/"))
        os.makedirs(path, exist_ok=True)
        with open(os.path.join(path, "memory.max"), "w", encoding="ascii") as handle:
            handle.write(text + "\n")

    def test_reads_the_scope_limit(self):
        self.write_limit("/user.slice/user-1002.slice/run-abc.scope", str(256 * 2 ** 20))
        self.assertEqual(memory_limit_bytes(self.base, self.proc), 256 * 2 ** 20)

    def test_walks_up_and_takes_the_tightest(self):
        self.write_limit("/user.slice/user-1002.slice/run-abc.scope", str(2 * GIB))
        self.write_limit("/user.slice/user-1002.slice", str(512 * 2 ** 20))
        self.write_limit("/user.slice", "max")
        self.assertEqual(memory_limit_bytes(self.base, self.proc), 512 * 2 ** 20,
                         "the slice above the scope is tighter")

    def test_max_everywhere_means_unlimited(self):
        self.write_limit("/user.slice/user-1002.slice/run-abc.scope", "max")
        self.write_limit("/user.slice", "max")
        self.assertIsNone(memory_limit_bytes(self.base, self.proc))

    def test_falls_back_to_the_root_file_when_no_cgroup_line(self):
        with open(self.proc, "w", encoding="ascii") as handle:
            handle.write("4:memory:/docker/abc\n")     # cgroup v1 layout
        self.write_limit("/", str(1024 * 2 ** 20))
        self.assertEqual(memory_limit_bytes(self.base, self.proc), 1024 * 2 ** 20)

    def test_cgroup_v1_unlimited_sentinel_is_ignored(self):
        with open(self.proc, "w", encoding="ascii") as handle:
            handle.write("4:memory:/docker/abc\n")
        legacy = os.path.join(self.base, "memory")
        os.makedirs(legacy)
        with open(os.path.join(legacy, "memory.limit_in_bytes"), "w",
                  encoding="ascii") as handle:
            handle.write("9223372036854771712\n")
        self.assertIsNone(memory_limit_bytes(self.base, self.proc))

    def test_candidates_start_at_the_process_cgroup(self):
        paths = list(_cgroup_memory_files(self.base, self.proc))
        self.assertTrue(paths[0].endswith(
            "user.slice/user-1002.slice/run-abc.scope/memory.max"), paths[0])
        self.assertTrue(paths[-1].endswith("memory.max"), paths[-1])
        self.assertGreater(len(paths), 1, "the walk must include the ancestors")
        # The process cgroup is also the current working directory when the tree has
        # no file: the reader must simply skip missing levels.
        self.assertIsNone(memory_limit_bytes(self.base, self.proc))


if __name__ == "__main__":
    unittest.main()
