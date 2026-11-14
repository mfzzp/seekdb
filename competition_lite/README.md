# MiniVectorDB Lite

This directory is the clean-room C++17 implementation for the bound competition JSONL contract. The runtime entrypoint is `src/minivectordb_lite.cpp`; it does not include or link the previous `minivectordb.cpp` implementation.

The runtime is deliberately single-writer. It has private transaction write sets, framed CRC WAL, a float32 vector file with mmap-on-demand reads, MemTable/Immutable/SST maintenance, checkpointed recovery state, scalar equality postings, exact Top-K, IVF with global non-residual PQ and raw-vector reranking, index persistence, and vector GC. Read-only SELECT avoids write-transaction snapshots, and the bounded query cache is invalidated by every recorded mutation.

The source line checker counts physical lines, including comments and blanks:

```sh
python3 competition_lite/tools/check_lines.py
```

It currently reports 1859 lines against the 2000-line runtime budget. The generator, Oracle, crash checker, benchmark driver, and summaries under `competition_lite/bench` are outside that runtime count.

A small end-to-end run is:

```sh
python3 competition_lite/bench/make_data.py --rows 200 --dim 16 --out /tmp/lite-data
python3 competition_lite/bench/oracle.py --data /tmp/lite-data --out /tmp/lite-data/oracle.jsonl
python3 competition_lite/bench/run_bench.py \
  --binary /mnt/SSD1/zzp/seekdb-round2-lite2000/cmake-build/minivectordb_lite \
  --data /tmp/lite-data --run-root /tmp/lite-run --mode B256 --queries 32
python3 competition_lite/bench/verify_results.py \
  --queries /tmp/lite-run/queries.json --oracle /tmp/lite-data/oracle.jsonl
```

The current evidence is recorded in `RESULTS.md`. It includes public/strict protocol checks, LSM and restart checks, 20 crash rounds, four write modes, an independent Oracle, and an old/new comparison. The plan's large 100k/300k matrix and memory-pressure runs are still pending; the report does not present them as completed. The WAL payload is structured and checksummed but still token-oriented rather than a fully typed column record, and SST files are not yet the sole recovery source. These are tracked as implementation gaps instead of being hidden by line compression or design-only claims.
