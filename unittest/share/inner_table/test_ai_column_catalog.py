"""Exercise the real inner-table generator for the AI Column catalog foundation."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = ROOT / "src/share/inner_table/generate_inner_table_schema.py"


class AiColumnCatalogTest(unittest.TestCase):
    def test_catalog_and_history_are_generated(self):
        with tempfile.TemporaryDirectory(prefix="ai-column-catalog-") as temporary:
            output = pathlib.Path(temporary)
            share = output / "share"
            observer = output / "observer"
            subprocess.run(
                [sys.executable, str(GENERATOR), "--share-output-dir", str(share),
                 "--observer-output-dir", str(observer), "--quiet"],
                cwd=GENERATOR.parent, check=True, timeout=180,
            )
            generated = "\n".join(
                path.read_text(encoding="utf-8")
                for path in sorted(share.glob("*")) if path.is_file()
            )
            for name in ("__all_ai_column", "__all_ai_column_history"):
                self.assertIn('"' + name + '"', generated)
            for name in ("state_column_id", "request_column_id", "result_spec_column_id",
                         "normalized_expr", "dependency_column_ids", "definition_version"):
                self.assertIn('"' + name + '"', generated)
            self.assertIn("OB_ALL_AI_COLUMN_TID", generated)
            self.assertIn("OB_ALL_AI_COLUMN_HISTORY_TID", generated)
            # These ranges are already part of the production source inventory,
            # including the auxiliary LOB tables generated for system tables.
            for name in ("ob_inner_table_schema.1101_1150.cpp",
                         "ob_inner_table_schema.51101_51150.cpp",
                         "ob_inner_table_schema.61101_61150.cpp"):
                self.assertTrue((share / name).is_file(), name)
            self.assertIn('"ai_column_info"', generated)


if __name__ == "__main__":
    unittest.main()
