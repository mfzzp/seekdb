#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
python3 "$root/tools/check_lines.py"
python3 -m py_compile "$root"/bench/*.py
cmake --build "${BUILD_DIR:-$root/../../cmake-build}" --parallel 2
