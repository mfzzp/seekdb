# Lite2000 verification ledger

Build: CMake Release, C++17, /mnt/SSD1/zzp/seekdb-round2-lite2000/cmake-build/minivectordb_lite.

The official local public harness ran all 20 cases with zero failures (95 SQL assertions). The crash check killed a process during an explicit transaction; after restart the committed row remained and the uncommitted row was absent. The response for shutdown was returned before exit.

The final fixed configuration used 1024-dimensional float32 vectors, 256-row INSERT requests, the eight benchmark attributes, and five exact queries with category='c1' for the short timing comparison. Results are wall-clock seconds from the Python driver:

| rows | lite insert | lite five queries | vector file | lite WAL | baseline insert | baseline five queries |
|---:|---:|---:|---:|---:|---:|---:|
| 10000 | 2.99 | 0.027 | 40,960,000 B | 414,379 B | 2.95 | 0.030 |
| 100000 | 42.01 | 0.471 | 409,600,000 B | 4,341,486 B | 33.67 | 0.298 |
| 300000 | 231.32 | 1.506 | 1,228,800,000 B | 13,468,441 B | 93.85 | 0.155 |

The baseline is the inherited C++ executable built separately under baseline-build; the new runtime does not link to or call it. The current implementation prioritizes bounded vector residency and durable WAL/rollback semantics, so it does not yet beat the baseline's in-memory write/query path at these exact timings.
