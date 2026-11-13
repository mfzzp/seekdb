# Lite2000 verification ledger

Build: CMake Release, C++17, `/mnt/SSD1/zzp/seekdb-round2-lite2000/cmake-build/minivectordb_lite`, with fixed-host `-O3 -ffast-math -march=native`.

The official local public harness ran all 20 cases with zero failures (95 SQL assertions). The crash check killed a process during an explicit transaction; after restart the committed row remained and the uncommitted row was absent. The shutdown response was returned before exit. The ANN smoke test returned the nearest two rows. The LSM restart check preserved the manifest and continued SST numbering without overwriting old files; a 600-commit stress check produced an L1 file and remaining SST files. A separate recovery test removed `wal.log` after clean shutdown and recovered the table, vector offsets, and vector index from `STATE`.

The fixed configuration used 1024-dimensional float32 vectors, 256-row INSERT requests, the eight benchmark attributes, and five exact queries with `category='c1'`. The 100k table below is a three-run wall-clock average from the Python driver; the final fdatasync-order hardening spot-check was 37.136 s total.

| rows | lite insert | lite five-query | lite total | vector file | lite WAL | baseline insert | baseline five-query | baseline total |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100000 | 36.933 s avg | 0.438 s avg | 37.371 s avg | 409,600,000 B | 4,341,486 B | 33.769 s avg | 0.282 s avg | 34.052 s avg |
| 300000 | 213.511 s | 1.415 s | 214.926 s | 1,228,800,000 B | 13,468,441 B | 92.109 s | 0.154 s | 92.264 s |

The baseline is the inherited C++ executable built separately under `baseline-build`; the new runtime does not link to or call it. The new implementation keeps vectors in an independent float32 file with mmap-on-demand reads, durable framed WAL, batched MemTable-to-SST flushing, and a clean-shutdown STATE fallback when the WAL is unavailable. The current exact path is slower than this baseline's in-memory vector path, so the measured result is recorded rather than presented as a claimed 2x win.

The runtime source audit reports 71 physical lines, below the 2000-line budget. Data generation and verification scripts are outside that runtime count.
