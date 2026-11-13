# Lite2000 verification ledger

Build: CMake Release, C++17, `/mnt/SSD1/zzp/seekdb-round2-lite2000/cmake-build/minivectordb_lite`, with fixed-host `-O3 -ffast-math -march=native`.
The contract build command `g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra` also completed successfully; it emitted only ignored-return-value warnings.

The official local public harness ran all 20 cases with zero failures (95 SQL assertions). The crash check killed a process during an explicit transaction; after restart the committed row remained and the uncommitted row was absent. The shutdown response was returned before exit. The ANN smoke test returned the nearest row, including the case where the vector index was created before inserts. The LSM restart check preserved the manifest and continued SST numbering without overwriting old files; a 600-commit stress check produced an L1 file and remaining SST files. A separate recovery test removed `wal.log` after clean shutdown and recovered the table, vector offsets, and vector index from `STATE`.

The fixed configuration used 1024-dimensional float32 vectors, 256-row INSERT requests, the eight benchmark attributes, and `category='c1'`. Every row below is a three-run wall-clock average from the same Python driver and request count.

The short comparison uses five exact queries, matching the earlier quick checkpoint:

| rows | lite insert | lite five-query | lite total | vector file | lite WAL | baseline insert | baseline five-query | baseline total |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100000 | 36.933 s | 0.438 s | 37.371 s | 409,600,000 B | 4,341,486 B | 33.769 s | 0.282 s | 34.052 s |
| 300000 | 214.665 s | 1.412 s | 216.077 s | 1,228,800,000 B | 13,468,441 B | 93.511 s | 0.156 s | 93.667 s |

The plan's full 1024-query measurement is:

| rows | lite insert | lite 1024-query | lite total | baseline insert | baseline 1024-query | baseline total |
|---:|---:|---:|---:|---:|---:|---:|
| 100000 | 36.570 s | 93.904 s | 130.474 s | 31.907 s | 0.396 s | 32.302 s |
| 300000 | 206.201 s | 366.114 s | 572.315 s | 91.729 s | 0.276 s | 92.005 s |

The baseline is the inherited C++ executable built separately under `baseline-build`; the new runtime does not link to or call it. The new implementation keeps vectors in an independent float32 file with mmap-on-demand reads, durable framed WAL, batched MemTable-to-SST flushing, buffered vector writes, incremental indexed ANN candidates, and a clean-shutdown STATE fallback when the WAL is unavailable. The current exact path is slower than this baseline's in-memory vector path, so the measured result is recorded rather than presented as a claimed 2x win.

The runtime source audit reports 71 physical lines, below the 2000-line budget. Data generation and verification scripts are outside that runtime count.
