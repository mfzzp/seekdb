# Lite2000 verification ledger

Build: CMake Release, C++17, `/mnt/SSD1/zzp/seekdb-round2-lite2000/cmake-build/minivectordb_lite`.

The official local public harness ran all 20 cases with zero failures (95 SQL assertions). The crash check killed a process during an explicit transaction; after restart the committed row remained and the uncommitted row was absent. The shutdown response was returned before exit. The ANN smoke test returned the nearest two rows, and the LSM restart check preserved `L1-33.sst` plus continued at `sst-43.sst` without overwriting old files. A 600-commit stress check produced an L1 file and the remaining SST files.

The final fixed configuration used 1024-dimensional float32 vectors, 256-row INSERT requests, the eight benchmark attributes, and five exact queries with `category='c1'`. Each timing below is a three-run wall-clock average from the Python driver:

| rows | lite insert avg | lite five-query avg | lite total avg | vector file | lite WAL | baseline insert avg | baseline five-query avg | baseline total avg |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100000 | 36.933 s | 0.438 s | 37.371 s | 409,600,000 B | 4,341,486 B | 33.769 s | 0.282 s | 34.052 s |

Earlier single-run checkpoints were 10,000 rows (lite 2.99 s insert / 0.027 s queries; baseline 2.95 s / 0.030 s) and 300,000 rows (lite 231.32 s insert / 1.506 s queries; baseline 93.85 s / 0.155 s). They are retained as exploratory checkpoints, not mixed into the three-run average above.

The baseline is the inherited C++ executable built separately under `baseline-build`; the new runtime does not link to or call it. The new implementation keeps vectors in an independent float32 file with mmap-on-demand reads and uses durable framed WAL plus batched MemTable-to-SST flushing. The current exact 100k path is still about 9.7% slower end-to-end than this baseline, so the result is recorded rather than presented as a claimed 2x win.

The runtime source audit reports 67 physical lines, below the 2000-line budget. Data generation and verification scripts are outside that runtime count.
