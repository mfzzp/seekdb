#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
bin=${1:-$root/../../cmake-build/minivectordb_lite}
out=${2:-/tmp/minivectordb-lite-run}
rows=${3:-200}
dim=${4:-16}
rm -rf "$out" /tmp/minivectordb-lite-data
python3 "$root/bench/make_data.py" --rows "$rows" --dim "$dim" --out /tmp/minivectordb-lite-data
python3 "$root/bench/oracle.py" --data /tmp/minivectordb-lite-data --out /tmp/minivectordb-lite-data/oracle.jsonl
for mode in S1 B256 B1024 T256; do
  python3 "$root/bench/run_bench.py" --binary "$bin" --data /tmp/minivectordb-lite-data --run-root "$out/$mode" --mode "$mode" --queries 32 --rows "$rows"
done
python3 "$root/bench/verify_results.py" --queries "$out/B256/queries.json" --oracle /tmp/minivectordb-lite-data/oracle.jsonl
python3 "$root/bench/summarize.py" --root "$out"
