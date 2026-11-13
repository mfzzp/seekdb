#!/usr/bin/env python3
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
files = [root / "src" / "minivectordb_lite.cpp"]
total = sum(p.read_text(encoding="utf-8").count("\n") for p in files if p.exists())
print(f"runtime_physical_lines={total}")
if total > 2000:
    print("ERROR: runtime line budget exceeded", file=sys.stderr)
    raise SystemExit(1)
