# MiniVectorDB Lite

This directory is the clean-room C++17 implementation for the bound competition JSONL contract. The runtime entrypoint is `src/minivectordb_lite.cpp`; it does not include or link the previous `minivectordb.cpp` implementation.

The runtime is deliberately single-writer. It has private transaction write sets, framed CRC WAL, a float32 vector file with mmap-on-demand reads, a bounded continuous-vector matrix cache for small tables, persisted scalar postings and full-coordinate Block statistics, MemTable/Immutable/SST maintenance, checkpointed recovery state, exact Top-K, IVF with global non-residual PQ and raw-vector reranking, index persistence, and vector GC. Read-only SELECT executes against current state without a result cache.

The source line checker counts physical lines, including comments and blanks:

```sh
python3 competition_lite/tools/check_lines.py
```

The current checker result is 2168 lines; the original 2000-line target is exceeded by explicit runtime index/statistics and cache paths. Correctness and measured performance take priority over artificial compression. The generator, Oracle, crash checker, benchmark driver, and summaries under `competition_lite/bench` are outside that runtime count.

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

The current evidence is recorded in `RESULTS.md`. WAL frames are aligned and reject complete-frame CRC corruption while truncating only incomplete tails. Manifest and index publication use temporary files; persisted indexes include a CRC and nprobe metrics expose actual bucket coverage. Checkpoint state is binary and recovery uses windowed mmap, so the 300k LOWMEM load/check and 256-query subset stay below 1 GiB. It includes public/strict protocol checks, LSM and restart checks, 20 crash rounds, four write modes, a small independent Oracle, a full 1024-case engine sweep at 100k/300k, and old/new timings. The plan's large Oracle, LOWMEM, full kill matrix, and submission checks remain explicitly marked as pending. The WAL is framed, checksummed, and carries typed integer/string/vector/keyword fields, but it still reconstructs mutation handlers from canonical tokens rather than a fully typed column record; SST files are not yet the sole recovery source.





