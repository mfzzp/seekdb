"""The deliverable text must not carry mojibake.

A PowerShell round-trip once rewrote README.md through a legacy codepage, turning
em dashes, middle dots, norm bars and two arrows into CJK characters followed by
"?" (U+9225 and friends).  The file looked fine in a terminal, and only a code-point
scan found it.  This test performs that scan over every delivered text file, so the
damage cannot ship again.

The markers are expressed as code points here; there are no literal markers in this
file for the scan to trip over.
"""

import json
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT_SUFFIXES = (".md", ".py", ".json", ".sh", ".txt")
MARKERS = ("\ufffd", "\u9225", "\u951f")       # replacement char, and the two CJK leads
SKIP_DIRS = (".git", "__pycache__", "extra_cases")


def _is_cjk(character):
    code = ord(character)
    return (0x3400 <= code <= 0x4DBF or 0x4E00 <= code <= 0x9FFF
            or 0xF900 <= code <= 0xFAFF or 0x3040 <= code <= 0x30FF)


def text_files():
    for directory, subdirectories, files in os.walk(ROOT):
        subdirectories[:] = [name for name in subdirectories if name not in SKIP_DIRS]
        for name in files:
            if name.endswith(TEXT_SUFFIXES):
                yield os.path.join(directory, name)


class TextIntegrityTest(unittest.TestCase):
    def test_no_mojibake_markers_anywhere(self):
        problems = []
        for path in text_files():
            try:
                with open(path, encoding="utf-8") as handle:
                    text = handle.read()
            except UnicodeDecodeError as exc:
                problems.append(f"{os.path.relpath(path, ROOT)}: not UTF-8 ({exc})")
                continue
            for number, line in enumerate(text.splitlines(), 1):
                if any(marker in line for marker in MARKERS):
                    problems.append(
                        f"{os.path.relpath(path, ROOT)}:{number}: {line.strip()[:100]}")
        self.assertEqual(problems, [], "mojibake found:\n" + "\n".join(problems))

    def test_english_deliverables_contain_no_cjk(self):
        """A GBK round-trip also produces valid CJK, which markers cannot catch.

        The comparison and multiplication signs turned into characters such as
        U+922E and U+8133; only a range check finds those.
        """
        problems = []
        for name in ("README.md", "VERIFICATION.md", "Dockerfile", "minivectordb.py",
                     "minivector_sql.py", "minivector_store.py"):
            path = os.path.join(ROOT, name)
            self.assertTrue(os.path.isfile(path), f"{name} is missing")
            with open(path, encoding="utf-8") as handle:
                content = handle.read()
            for number, line in enumerate(content.splitlines(), 1):
                hits = [character for character in line if _is_cjk(character)]
                if hits:
                    problems.append(f"{name}:{number}: {''.join(hits)!r} in "
                                    f"{line.strip()[:80]}")
        self.assertEqual(problems, [], "CJK in an English deliverable:\n" + "\n".join(problems))

    def test_documents_keep_their_typography(self):
        with open(os.path.join(ROOT, "README.md"), encoding="utf-8") as handle:
            readme = handle.read()
        # The repaired lines use proper punctuation; check a few of them.
        for expected in ("\u2014 an `UPDATE`", "\u00b7width\u00b7eps\u00b7",
                         "\u2192 1.13 ms"):
            self.assertIn(expected, readme, f"README lost {expected!r}")

    def test_evidence_file_is_valid_utf8_json_without_markers(self):
        path = os.path.join(ROOT, "agent", "conversation.json")
        with open(path, "rb") as handle:
            raw = handle.read()
        text = raw.decode("utf-8")
        self.assertFalse(any(marker in text for marker in MARKERS))
        json.loads(text)          # raises if the file is not valid JSON


if __name__ == "__main__":
    unittest.main()
