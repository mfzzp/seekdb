"""Exercise the real inner-table generator for the AI Column catalog foundation."""
import pathlib
import re
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
            for name in ("__all_ai_column_task", "__all_ai_column_task_history",
                         "request_id", "lease_id", "state", "result_value",
                         "eligible_ts", "retry_count"):
                self.assertIn('"' + name + '"', generated)
            # Check each task schema itself: similarly named fields in another
            # system table must not conceal a missing worker catalog column.
            for suffix in ("task", "task_history"):
                body = re.search(
                    r"int ObInnerTableSchema::all_ai_column_" + suffix
                    + r"_schema\([^\n]+\)\s*\{(.*?)\n\}", generated, re.S
                )
                self.assertIsNotNone(body, suffix)
                for name, sql_type in (("state", "ObIntType"),
                                       ("lease_id", "ObVarcharType"),
                                       ("result_value", "ObVarcharType")):
                    column = re.search(r'ADD_COLUMN_SCHEMA(?:_T)?\("' + name
                                       + r'",(.*?)\n  \}', body[1], re.S)
                    self.assertIsNotNone(column, (suffix, name))
                    self.assertIn(sql_type + ", //column_type", column[1])
                    if suffix == "task" and name != "result_value":
                        self.assertIn("false, //is_nullable", column[1])
                self.assertIn("state_default.set_int(0)", body[1])
            # These ranges are already part of the production source inventory,
            # including the auxiliary LOB tables generated for system tables.
            for name in ("ob_inner_table_schema.1101_1150.cpp",
                         "ob_inner_table_schema.51101_51150.cpp",
                         "ob_inner_table_schema.61101_61150.cpp"):
                self.assertTrue((share / name).is_file(), name)
            self.assertIn('"ai_column_info"', generated)


if __name__ == "__main__":
    unittest.main()
