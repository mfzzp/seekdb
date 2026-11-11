"""Static guards for the Dockerfile, which cannot be built in this environment.

Neither machine has a Docker runtime, so the image is the one deliverable that is
never executed here.  What can be checked is everything the file must keep true:
a pinned base image, COPY sources that exist, an entrypoint that points at a file
that was copied, the thread limits justified by the pid budget, a pinned and
best-effort dependency install, and the /data mount the judge uses.

If any of these drifts, the test fails here instead of during the judge's build.
"""

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCKERFILE = os.path.join(ROOT, "Dockerfile")

# Files the image must contain; minivectordb.py imports the other two.
REQUIRED_SOURCES = ("minivectordb.py", "minivector_sql.py", "minivector_store.py")


class DockerfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(DOCKERFILE, "rb") as handle:
            cls.raw = handle.read()
        cls.text = cls.raw.decode("utf-8")
        cls.lines = [line for line in cls.text.splitlines()]
        # Join backslash continuations first, otherwise a multi-line ENV or RUN is
        # parsed as several unrelated instructions.
        joined = re.sub(r"\\\s*\n", " ", cls.text)
        cls.instructions = [line.strip() for line in joined.splitlines()
                            if line.strip() and not line.strip().startswith("#")]

    def test_line_endings_and_encoding(self):
        self.assertNotIn(b"\r\n", self.raw, "the Dockerfile must use LF, not CRLF")
        self.assertFalse(self.raw.startswith(b"\xef\xbb\xbf"), "no BOM")

    def test_base_image_is_pinned(self):
        from_lines = [line for line in self.instructions if line.upper().startswith("FROM ")]
        self.assertEqual(len(from_lines), 1, "exactly one FROM is expected")
        image = from_lines[0].split()[1]
        self.assertEqual(image, "python:3.12.10-slim-bookworm",
                         "the base image must stay pinned to the tested interpreter")
        self.assertRegex(image, r":\d+\.\d+\.\d+-", "no floating tag such as :3.12-slim")

    def test_copied_sources_exist(self):
        copies = [line for line in self.instructions if line.upper().startswith("COPY ")]
        self.assertTrue(copies, "the image must copy the implementation")
        seen = set()
        for line in copies:
            parts = line.split()[1:]
            self.assertGreaterEqual(len(parts), 2, f"malformed COPY: {line}")
            *sources, destination = parts
            for source in sources:
                path = os.path.join(ROOT, source)
                self.assertTrue(os.path.isfile(path),
                                f"COPY source {source} does not exist in the tree")
                seen.add(os.path.basename(source))
            self.assertTrue(destination.endswith("/") or destination == "./",
                            f"COPY should target a directory: {line}")
        self.assertEqual(set(REQUIRED_SOURCES) - seen, set(),
                         "every module the entry point imports must be copied")

    def test_entrypoint_points_at_a_copied_file(self):
        entry = [line for line in self.instructions if line.upper().startswith("ENTRYPOINT ")]
        self.assertEqual(len(entry), 1, "exactly one ENTRYPOINT is expected")
        self.assertIn("/app/minivectordb.py", entry[0],
                      "the entry point must be the copied program path")
        self.assertFalse([line for line in self.instructions
                          if line.upper().startswith("CMD ")],
                         "a CMD would be appended to the entry point and is not wanted")
        workdir = [line for line in self.instructions if line.upper().startswith("WORKDIR ")]
        self.assertEqual(workdir, ["WORKDIR /app"])

    def test_thread_limits_are_set(self):
        environment = " ".join(line for line in self.instructions if line.startswith("ENV "))
        for name in ("OPENBLAS_NUM_THREADS=1", "OMP_NUM_THREADS=1", "MKL_NUM_THREADS=1"):
            self.assertIn(name, environment,
                          f"{name} keeps the process inside the 127-pid budget")
        self.assertIn("PYTHONDONTWRITEBYTECODE=1", environment,
                      "the judge runs the container with a read-only root filesystem")

    def test_dependency_is_pinned_and_the_install_is_best_effort(self):
        run_lines = " ".join(line for line in self.instructions if line.startswith("RUN "))
        self.assertIn("numpy==2.3.5", run_lines, "the dependency must be pinned exactly")
        self.assertNotIn("numpy>=", run_lines, "an open range would drift between builds")
        self.assertIn("||", run_lines,
                      "a failed install must not fail the build while the fallback exists")
        self.assertIn("--timeout", run_lines, "bound the network wait inside pip itself")

    def test_data_volume_and_syntax_compilation(self):
        self.assertIn('VOLUME ["/data"]', self.instructions,
                      "the judge mounts the database directory at /data")
        self.assertIn("py_compile", " ".join(self.instructions),
                      "the build should fail fast on a syntax error")

    def test_no_relative_data_path_baked_in(self):
        entry = next(line for line in self.instructions if line.upper().startswith("ENTRYPOINT "))
        self.assertNotIn("--data-dir", entry,
                         "the judge passes --data-dir; the image must not force another one")


if __name__ == "__main__":
    unittest.main()
