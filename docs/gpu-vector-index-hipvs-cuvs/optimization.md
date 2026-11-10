# AMD CAGRA candidates: construction and multi-CTA search

This candidate is based on contest V5 commit
`634888d9b037153fa143d2c913ba4b25503e1d62`. It uses the unmodified hipVS
commit `f96a663d0a4441d6f5344bd3ef552b71ae7b370b` supplied by the contest.
Each new candidate needs its own GPU validation. The 2026-11-10 candidate below
has real GPU and SQL measurements, including an unresolved long-tail stall.
It is a WIP candidate, not a declaration that official Recall/resource gates or
performance requirements pass. Older sections retain their historical status.

## Current WIP: lighter construction paired with multi-CTA search (2026-11-10)

Following the supplied optimization walkthrough, the bridge uses graph degree
96, intermediate degree 128 and two NN-descent iterations. Native `MULTI_CTA`
search uses `search_width=4` and a candidate budget of
`align_up(max(hipVS default, 8192, 4*topk), 64)`. Overflow is rejected before
allocation; each query recomputes its budget, including after loading an older
index. No dataset name, dimension, query or score selects these parameters.
Every FP32 input row still reaches the fixed hipVS build and every query executes
the native search. Existing V3 snapshot deduplication and V2/V3/V4 readers remain.

The comparison uses parent `af06346bff80aeadca159518d424b9d8c16ef8d9` as the SQL
control, not an independently compiled V5 SQL server. Both bridges run in the
same seekdb binary, with fresh data directories, 16 CPUs, 24 GiB seekdb memory,
TRACE disabled and identical FP32 data/individual SQL requests. Hardware is
gfx1100, 48 GiB VRAM, a 16-CPU/55-GiB container and ROCm 7.2.1. The official
7.2.4 environment and hidden input are not reproduced.

The public SIFT diagnostic uses the first 250,000 training rows and first 4,096
test rows from `sift-128-euclidean.hdf5`; exact neighbors are recomputed for that
subset. The other input contains 250,000 independent 256-dimensional Gaussian
vectors and 4,096 independent queries generated with seed 371912. No dataset
rows are omitted. Each SQL run executes all 4,096 probes separately for Top-10
and Top-100 and includes the first query in index-build readiness.

| First SQL run | Control build, seconds | Candidate build, seconds | Candidate Recall@10 / @100 |
| --- | ---: | ---: | --- |
| SIFT subset, 128 dimensions | 21.5681 | 4.7415 | 0.9999756 / 0.9999756 |
| Gaussian, 256 dimensions | 22.1414 | 6.4552 | 0.9997559 / 0.9994263 |

These build samples improved, but **the Gaussian Top-10 run includes a 283.798-second
single-query stall**: its measured QPS is only 14.19, compared with the control's
246.23. That delay is included, not removed as an outlier. The same persisted
indexes passed a fresh-process restart and another complete query pass; Gaussian
QPS then measured 805.76 / 632.79, with Recall 0.9997559 / 0.9994409. This does not
erase the first-run failure to deliver stable performance. A five-iteration
diagnostic also stalled and was interrupted; it is not a passing alternative.

Native profiling found both long `kern_prune` samples and large gaps between
GPU operations, including delays in frozen V5. A kernel-only five-iteration
profile took 195.77 seconds for the bridge build but recorded less than one
second of GPU kernel execution. The current evidence does not distinguish
runtime/device scheduling, synchronization and parameter effects sufficiently
to assign a root cause. Disabling SDMA in a separate native diagnostic did not
eliminate variable build times. No driver setting is changed by the product.
A later full SQL isolation run with `HSA_ENABLE_SDMA=0` passed the expanded
filter/delete/rollback/reinsert regression and all candidate Recall queries,
but the candidate SIFT build took 122.607 seconds. The unchanged control then
took 149.520 seconds to build the Gaussian index and its overall run timed out
at 300 seconds during querying. Disabling SDMA is **not an established remedy**;
the paired performance run is incomplete.

The complete seekdb binary compiled on Linux, bootstrapped on the GPU instance,
and returned `SELECT 1`. Its source is the parent revision; bridge-only changes
are dynamically loaded from the independently compiled candidate library.
Linux host checks with ASan/UBSan and Windows host checks both passed 7/7.
Host doubles cover ownership and errors; they do not establish ANN quality.
The real SQL DDL regression and full-data restart checks passed before the
additional filter/delete/rollback regression was added. That expanded test first
hit a 300-second DDL timeout in the default runtime environment; it passed in
2.768 seconds after restart with SDMA disabled. Neither result is hidden. The
Linux build used Clang 17 and an available Rust 1.98.1 toolchain override rather
than the repository Rust 1.97.1 pin; no toolchain pin was changed in source.

Final bridge write/read processes passed FP32 byte equality, shape rejection,
Top-10/100/200/10 and one/three-query checks. Two additional 100,000-row,
256-dimensional sphere/anisotropic fixtures each achieved Recall@10/@100 of
1.0 across 256 independent queries, before and after reload. These final checks
used the documented SDMA-disabled diagnostic environment, not a product change.
Commands, individual measurements, failed runs and source/binary hashes are in
[`validation/gpu_mcta_20261110.json`](validation/gpu_mcta_20261110.json).

Two standalone diagnostic tools preserve the measurement procedure:
`tools/hipvs_bridge/benchmark_recall.cpp` generates full-input exact references,
checks FP32 restoration, IDs and distances, and reports native timings;
`tools/hipvs_bridge/benchmark_sql.py` imports those files and measures individual
SQL queries. These tools do not replace or alter the contest's runner or scoring.
Recall values must be compared with the same-round baseline separately; a zero
exit status only establishes successful execution and structural assertions.

Example single-line commands (use a dedicated instance and new output paths):

```sh
g++ -O3 -std=c++17 -fopenmp tools/hipvs_bridge/benchmark_recall.cpp -ldl -o /tmp/benchmark_recall
STUDY_EXPORT_PREFIX=/tmp/gaussian STUDY_EXPORT_ONLY=1 /tmp/benchmark_recall /tmp/amd/libseekdb_cuvs_bridge.so 250000 256 4096 gaussian /tmp/unused-snapshot.bin
python3 tools/hipvs_bridge/benchmark_sql.py --mode prepare --dataset-prefix /tmp/gaussian --dimension 256
SEEKDB_TEST_PORT=2881 python3 tools/hipvs_bridge/benchmark_sql.py --mode run --dataset-prefix /tmp/gaussian --dimension 256 --database gaussian_diagnostic --label candidate --output /tmp/candidate-sql.json
SEEKDB_TEST_PORT=2881 python3 tools/hipvs_bridge/benchmark_sql.py --mode verify --dataset-prefix /tmp/gaussian --dimension 256 --database gaussian_diagnostic --label candidate-restarted --output /tmp/candidate-restarted.json
```

Run the last command after restarting the same dedicated instance. Dependencies
are a C++17/OpenMP compiler for reference generation and PyMySQL for SQL. The SQL
tool reads `SEEKDB_TEST_HOST/PORT/USER/PASSWORD`; never put credentials in source.
It creates a new database and retains it for restart verification. The native
tool reports bridge QPS, which must not be described as SQL QPS.

## Seventh candidate (historical): respect CPU quotas and restore direct snapshot writes

The sixth candidate, `2caab0c9a1577b17ca2f7805c8b0d1f4f6c2aede`, received
**7.204/20**, `build=1.099x, query=1.790x, gates=pass`. The best earlier result
remains **8.456/20** for `2f2fc3d0742d63571216313c2b8eec88c7fed3d5`.
The aggregate platform numbers alone cannot distinguish machine variation from
the cost of a change. This revision uses the first real AMD component experiments
to select changes; it has not been submitted to the official evaluator.

### Container CPU oversubscription

The diagnostic Radeon Cloud instance exposes 128 CPUs in its affinity mask, but
its cgroup v2 `cpu.max` is `1600000 100000`: only 16 CPU-equivalents of runtime.
ROCm's LLVM OpenMP defaults to 128 threads. The frozen NN-descent and CAGRA graph
construction code contains CPU OpenMP loops, including work on native
`std::thread` threads. A runtime probe confirmed that `omp_set_num_threads(16)`
only changes the caller's limit; new native threads still report 128. LLVM's
`kmp_set_defaults("OMP_NUM_THREADS=16")` also changes the new threads' default.

The bridge now initializes this runtime policy once. It reads positive quotas
from cgroup v1/v2 at common Linux mount points and visible parents, rounds
fractional CPU quotas up, and caps the current OpenMP limit without increasing
it. Missing, unlimited, malformed or unsupported quota layouts retain the
runtime default. This is based on the actual quota, not a fixed core count.
Explicit `OMP_NUM_THREADS`, `OMP_THREAD_LIMIT`, `KMP_ALL_THREADS` or
`KMP_HW_SUBSET` settings take precedence. `build.sh` links ROCm's LLVM OpenMP,
which is already used by hipVS. No pinned dependency source is modified.

This sets the **process-wide LLVM OpenMP default** for newly created native
threads, including threads outside the bridge that share that runtime; it is
not a per-index setting. Existing explicit administrator policy is preserved,
and multi-index/mixed-workload throughput still requires SQL-level testing.

The same baseline bridge built all 65,536 synthetic FP32 vectors, dimension 128,
in six fresh processes, with thread limits ordered 128/16/16/128/128/16. Each run
also saved, freed and reloaded the index, verified byte-exact vectors, unique
valid Top-100 IDs, L2 distances and the self neighbor. Three additional fresh
processes tested the new automatic quota policy with no thread override.
The baseline has the parent algorithm plus the error-logging correction below;
that correction does not change successful build/search execution.

| Configuration | Build samples, ms | Mean, ms |
| --- | --- | ---: |
| Parent bridge, 128 OpenMP threads | 19000.474, 21041.209, 22161.972 | 20734.552 |
| Same bridge, explicit 16 threads | 5585.234, 5610.628, 5817.339 | 5671.067 |
| New bridge, automatic quota | 5614.364, 5661.864, 5537.902 | 5604.710 |

The measured **bridge build call** is 3.699x faster than the 128-thread samples.
This excludes SQL row scanning, publication and official first-query readiness;
it is neither the competition build speedup nor a score prediction. Environments
that already size OpenMP correctly may see little benefit from this change.

### Snapshot transfer and larger Top-k

On an actual 67,110,577-byte hipVS snapshot, alternating five measured runs per
format after warmup gave these results. Every restored file was byte-identical.

| Format | Stored bytes | Mean save ms | Mean restore ms |
| --- | ---: | ---: | ---: |
| V3 direct copy | 67,110,577 | 47.878 | 34.110 |
| V4 compressed blocks | 51,405,159 | 113.126 | 82.513 |

These are buffered filesystem/tmpfile transfers, without fsync or seekdb's
persistence stream. Compression's CPU cost outweighed its byte saving here.
The writer therefore returns to V3, while retaining V2/V3/V4 readers and codec
regressions for existing indexes. It still persists every opaque hipVS byte and
the ID mapping; no FP32 data or graph work is dropped.

Actual GPU persistence testing also exposed an existing Top-200 failure:
`itopk_size cannot be larger than 512` in the pinned `search_single_cta.cuh`.
The bridge now selects `MULTI_CTA` above that candidate limit, preserving
`itopk_size=max(default,512,4*topk)`, and restores the default strategy on smaller
requests. Top-10/100 search parameters and the NN-descent recipe are unchanged.
Search errors are logged before synchronization clears the library's error text;
cleanup still waits for outstanding work and propagates failure.

### Executed checks and limits

The frozen hipVS C library and the candidate bridge compiled and ran on gfx1100,
Ubuntu 24.04.4, ROCm **7.2.1**, with a 16-CPU quota and 55 GiB RAM. All 1,677
source files in the frozen hipVS archive were checked unchanged. This differs
from the official ROCm **7.2.4**, 24-CPU/96-GB environment; dependency archives
and source hashes are recorded in the evidence. Do not deploy this diagnostic
binary in place of the evaluator's prescribed dependencies.

- Windows host regressions: **7/7 passed**; adaptor compatibility-header syntax
  check passed. This is not a complete Windows/Linux seekdb build.
- Linux host regressions: **7/7 passed**, ASan/UBSan, leak detection,
  `-Wall -Wextra -Werror` (bundled compressor warnings remain suppressed).
- Actual GPU, separate write/read processes: Top-10/100/200/10, one/three query
  inputs, guards, IDs, distances, shape rejection and FP32 byte equality passed.
- Actual GPU Recall, the same 64-probe exact CPU-reference driver before/after:
  256/512/513-row boundary fixtures and 65,536-row uniform/clustered fixtures
  passed before and after reload. Uniform Recall@100 was 0.998594 -> 0.998750;
  clustered Recall@100 was 0.998437 -> 0.998125. The largest observed decline is
  about 0.000312. These synthetic samples do not establish hidden-dataset gates.
- Explicit `OMP_NUM_THREADS=2` was preserved and its GPU smoke test passed.
- Full seekdb compilation/bootstrap, SQL DDL/restart, SIFT/hidden250k, paired
  peak VRAM and official build/query performance remain **pending**.

Commands, outputs, source hashes and individual measurements are in
[`validation/gpu_quota_20261109.json`](validation/gpu_quota_20261109.json).
The exact diagnostic sources are `tools/hipvs_bridge/benchmark_build_phases.cpp`
and `tools/hipvs_bridge/benchmark_snapshot_transfer.cpp`. From the repository
root, with a locally built bridge and the frozen dependencies, these single-line
commands reproduce the component checks (use a new output directory):

```sh
mkdir -p /tmp/amd-quota && HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm bash tools/hipvs_bridge/build.sh /tmp/amd-quota/libseekdb_cuvs_bridge.so
g++ -O3 -std=c++17 tools/hipvs_bridge/benchmark_build_phases.cpp -Itools/hipvs_bridge -L/tmp/amd-quota -lseekdb_cuvs_bridge -Wl,-rpath,/tmp/amd-quota -Wl,-rpath-link,/opt/hipvs/lib -Wl,-rpath-link,/opt/rocm/lib -Wl,-rpath-link,/opt/rocm/lib/llvm/lib -o /tmp/amd-quota/benchmark_build_phases
LD_LIBRARY_PATH=/opt/hipvs/lib:/opt/rocm/lib:/opt/rocm/lib/llvm/lib /tmp/amd-quota/benchmark_build_phases 65536 128 /tmp/amd-quota/fixture.bin
g++ -O3 -std=c++17 tools/hipvs_bridge/test_build_quality.cpp -Itools/hipvs_bridge -L/tmp/amd-quota -lseekdb_cuvs_bridge -Wl,-rpath,/tmp/amd-quota -Wl,-rpath-link,/opt/hipvs/lib -Wl,-rpath-link,/opt/rocm/lib -Wl,-rpath-link,/opt/rocm/lib/llvm/lib -o /tmp/amd-quota/test_build_quality
LD_LIBRARY_PATH=/opt/hipvs/lib:/opt/rocm/lib:/opt/rocm/lib/llvm/lib /tmp/amd-quota/test_build_quality /tmp/amd-quota/recall 65536
cmake -S unittest/oblib/vector/cuvs -B /tmp/amd-quota/host -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/amd-quota/host --parallel 4
g++ -O3 -std=c++17 tools/hipvs_bridge/benchmark_snapshot_transfer.cpp -Isrc/oblib /tmp/amd-quota/host/libcuvs_test_zstd.a -o /tmp/amd-quota/benchmark_snapshot_transfer
/tmp/amd-quota/benchmark_snapshot_transfer /tmp/amd-quota/fixture.bin /tmp/amd-quota/transfer
```

For a paired OpenMP experiment, run the same bridge binary with explicit thread
counts matching that machine's visible CPUs and quota; alternate order and use
fresh snapshot paths. Neither diagnostic replaces the unchanged official SQL
benchmark. The evidence archive and the locally built runtime were backed up
off the temporary GPU instance and their SHA256 hashes checked.

## Sixth candidate (historical, gates passed): lossless snapshot blocks

The user reported **8.456/20**, `build=1.213x, query=1.799x, gates=pass`
for `2f2fc3d0742d63571216313c2b8eec88c7fed3d5`. This is the best observed
candidate so far. It is the parent of this change; it remains available for
paired comparison. The platform did not supply phase timings, numerical Recall
or peak memory, so the exact remaining bottleneck is not established.

This candidate retains that version's NN-descent recipe, graph degrees, complete
FP32 inputs, query parameters, bridge and frozen hipVS library. It targets the
bytes crossing the seekdb persistence stream after hipVS serialization. The
complete opaque hipVS file contains both adjacency IDs and the FP32 dataset;
the previous change removed duplicate vectors but still wrote this file in full.
Fewer bytes can reduce stream callbacks and durable writes. Whether this saving
exceeds compression CPU time must be measured on the AMD host.

`OBCUVS04` retains the V3 header and IDs, then encodes the complete opaque file
in blocks of at most 1 MiB. It uses seekdb's existing Zstd 1.3.8 public wrapper
at its existing level 1, with no new dependency or shared compressor changes.
A block that does not shrink is stored raw. Each block has a 16-byte header
(raw length, stored length and checksum seeded by block ordinal). The decoder
checks lengths before decompression, requires exact decoded length and checksum,
and reconstructs every original byte before the existing bridge loads the file.
Truncation, corrupt data, compression and I/O failures fail the operation.
There is no quantization, omitted graph processing or cross-run cache.

Codec buffers are about 2 MiB plus the bundled compressor's workspace per active
serialization, independent of dataset size. The existing complete staging file
is still needed; this change does not reduce its memory or hipVS build memory.
The codec makes no device allocations. Actual peak host memory and VRAM still
require platform measurement. Incompressible blocks incur 16 bytes of framing
per MiB and compression CPU time; random-looking vectors may benefit little.

The new binary reads V2, V3 and V4, and writes V4. Older binaries cannot read V4;
use independent fresh instances or rebuild indexes before downgrading. CPU VSAG
persistence and the pinned hipVS file format are unchanged.

Windows host tests passed **6/6**; the adaptor passed its compatibility-header
syntax check. GCC 13.3 on the existing NVIDIA-only Linux server also passed
**6/6** with ASan/UBSan. Codec tests use the actual bundled compressor and cover
byte equality, old/new envelopes, mixed raw/compressed blocks, corrupt sizes,
checksum/reordering, truncation, failed I/O and exception cleanup. These host
tests do not establish full seekdb linking, SQL restart, Recall or GPU speed.
The codec also linked and passed its tests with the real bundled Zstd objects
after Linux `ld -r` and `objcopy --localize-hidden`, matching production symbol
packaging. Only the public wrapper is referenced; no hidden Zstd entry point is
required. This targeted check is not a complete seekdb link test.

A Linux Release probe used four **synthetic 32 MiB byte fixtures**, three runs
each. It checks full byte equality; it is not a hipVS/SQL or contest benchmark.
The timings include temporary-file and string-stream work on the available
server and do not predict the platform score.

| Synthetic fixture | Raw bytes | Stored bytes (including framing) | Reduction | Encode ms | Decode ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Bounded 32-bit IDs | 33,554,432 | 24,114,599 | 28.13% | 79.989 | 43.718 |
| FP32 uniform values | 33,554,432 | 30,502,116 | 9.10% | 69.846 | 50.142 |
| FP32 integer values | 33,554,432 | 12,506,714 | 62.73% | 135.441 | 50.544 |
| Random bytes | 33,554,432 | 33,554,944 | -0.0015% | 65.544 | 16.338 |

Run this optional probe after the host build with
`./build_cuvs_host/test_cuvs_snapshot_codec --bench`.
Source hashes, check commands and results are in
[`validation/host_codec_20261109.json`](validation/host_codec_20261109.json).
Snapshot logs now include `graph_stored_bytes`, `compressed_blocks` and
`raw_blocks` alongside `graph_bytes` and the existing phase timers. Compare
`graph_write_us` and the entire measured construction time against the parent;
a smaller file alone is not a successful performance result.

## Fifth candidate (historical, gates passed): restore NN-descent and store the dataset once

The fourth candidate, `fa64d4537830b163f7ff53240d338c5d06e8b362`, scored
**0/20** on the platform. The supplied failures were `relative_vram_limit` and
`hidden250k:recall@100_baseline_delta`; no numerical VRAM or Recall measurements
were supplied. These are hard gates, not a lack of timing improvement. The
iterative builder is therefore removed from the current candidate, including
its build-time switch. Construction again uses the previously gate-passing
NN-descent recipe: graph degree 128, intermediate degree 256, 100 maximum
iterations. The fixed CAGRA search, its parameters, input FP32 rows, SQL workload
and scoring files are unchanged. Before this candidate was evaluated, the best
observed result was **6.943/20** at
`79f8929b4caa733598962d110779c1c45d56780a`. Its later platform result was
**8.456/20**, with all reported gates passing, as recorded above. The following
host-check details describe the evidence available before that platform run.

Source inspection found duplicate persistence of every FP32 vector. `OBCUVS02`
stored IDs and a raw vector array before the hipVS snapshot, but the bridge
already calls `cuvsCagraSerialize(..., true)`. The pinned library's
[serializer](https://github.com/AMD-Ecosystem/hipVS/blob/f96a663d0a4441d6f5344bd3ef552b71ae7b370b/cpp/src/neighbors/detail/cagra/cagra_serialize.cuh)
includes the dataset with the complete graph. The new `OBCUVS03` envelope stores
the same header fields, IDs and complete hipVS snapshot, omitting only the
second raw vector array. For 250,000 rows, this removes exactly 128,000,000 bytes
at dimension 128 or 256,000,000 bytes at dimension 256 from the seekdb stream.
These byte counts are arithmetic, **not measured build speedups**. The staging
snapshot remains complete, is transient, and is never reused across builds.

On reload, the bridge uses the pinned library's documented
[`cuvsCagraIndexGetDataset`](https://github.com/AMD-Ecosystem/hipVS/blob/f96a663d0a4441d6f5344bd3ef552b71ae7b370b/cpp/include/cuvs/neighbors/cagra.h)
non-owning device view. It validates shape, FP32 dtype, layout and arithmetic,
then copies each row into the already allocated host rebuild/fallback buffer.
The copy handles padded row strides and byte offsets, makes no additional
device allocation, synchronizes before returning and releases view metadata.
The adaptor publishes a reloaded index only after full vector restoration; a
failure frees the new GPU handle and fails the load. This adds a device-to-host
copy during reload in exchange for eliminating duplicate durable I/O. Its
end-to-end effect and peak memory still need AMD measurement. Persisted graph
and FP32 data are neither quantized nor recomputed.

Compatibility: this version reads both `OBCUVS02` and `OBCUVS03`, and writes
`OBCUVS03`. An older binary cannot read the new envelope; use fresh independent
instances for comparisons or rebuild indexes before downgrading. Deploy the
bridge and seekdb together because vector export is a new bridge entry point.
The CPU VSAG dispatch and its snapshot format are unchanged.

Host checks exercise the production envelope helpers against an independently
encoded V2 layout, exact V3 layout, signed 64-bit IDs, arbitrary FP32 bits,
opaque graph bytes, next-object boundaries, all truncation/short-write offsets
and corrupt headers. Bridge API doubles check byte-exact restoration including
NaNs/signed zero, padding/offsets, invalid metadata, failed copies, failed
synchronization and resource cleanup without device allocation. These tests
do not execute hipVS. `test_persistence.cpp` and `test_build_quality.cpp` now
require full byte equality of exported vectors after build and reload; their
real-GPU execution, SQL restart and the official paired gates remain pending.

Windows host tests passed 5/5 and the adaptor passed its compatibility-header
syntax check. On the existing NVIDIA-only Linux server, GCC 13.3 host tests
passed 5/5 with ASan/UBSan and `-Wall -Wextra -Werror`. C and C++ syntax checks
against the pinned hipVS C headers, compilation of both real-GPU diagnostic
sources to objects, and Bash syntax checks passed. CUDA/DLPack declarations
were test substitutes; these checks did not link ROCm or execute hipVS. Source
hashes, commands, exit codes and outputs are recorded in
[`validation/host_compact_20261109.json`](validation/host_compact_20261109.json).

For an AMD host, build a fresh bridge and run the actual GPU diagnostics:

```sh
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm bash tools/hipvs_bridge/build.sh /tmp/amd-compact-study/libseekdb_cuvs_bridge.so
c++ -std=c++17 -O2 tools/hipvs_bridge/test_build_quality.cpp -Itools/hipvs_bridge -L/tmp/amd-compact-study -lseekdb_cuvs_bridge -Wl,-rpath,/tmp/amd-compact-study -Wl,-rpath-link,/opt/hipvs/lib -Wl,-rpath-link,/opt/rocm/lib -o /tmp/amd-compact-study/test_build_quality
LD_LIBRARY_PATH=/tmp/amd-compact-study:/opt/hipvs/lib:/opt/rocm/lib /tmp/amd-compact-study/test_build_quality /tmp/amd-compact-study/fixture
c++ -std=c++17 -O2 tools/hipvs_bridge/test_persistence.cpp -Itools/hipvs_bridge -L/tmp/amd-compact-study -lseekdb_cuvs_bridge -Wl,-rpath,/tmp/amd-compact-study -Wl,-rpath-link,/opt/hipvs/lib -Wl,-rpath-link,/opt/rocm/lib -o /tmp/amd-compact-study/test_persistence
LD_LIBRARY_PATH=/tmp/amd-compact-study:/opt/hipvs/lib:/opt/rocm/lib /tmp/amd-compact-study/test_persistence write /tmp/amd-compact-study/restart.bin
LD_LIBRARY_PATH=/tmp/amd-compact-study:/opt/hipvs/lib:/opt/rocm/lib /tmp/amd-compact-study/test_persistence read /tmp/amd-compact-study/restart.bin
```

Use a new directory/prefix for each experiment. These bridge diagnostics still
need the existing full seekdb SQL build/restart checks and unchanged official
paired benchmark. The snapshot log records `header.version` and
`omitted_vector_bytes` alongside the existing phase timers to distinguish the
durable I/O change from GPU construction time.

## Fourth candidate (historical, failed gates): iterative builder

The following describes `fa64d4537830b163f7ff53240d338c5d06e8b362`, not the
current implementation. Its hypotheses were not validated by the platform.

Two platform observations supplied by the user are now available:

| Commit | Build speedup | Query speedup | Gates | Score / 20 |
| --- | ---: | ---: | --- | ---: |
| `79f8929b4caa733598962d110779c1c45d56780a` | 1.077x | 1.749x | pass | 6.943 |
| `16c6b28574bffcf4a5f2d5192296372a27140e58` | 1.021x | 1.725x | pass | 6.268 |

These are separate paired platform runs. They do not prove that the mapped
transfer caused the difference, but provide no evidence to promote it. This
candidate restores buffered snapshot transfer and keeps the phase timers.
At that stage, the highest observed score was the first row of the table.

The construction hypothesis is now structural rather than another copy
microbenchmark. In pinned hipVS, NN-descent's `GNND::build` alternates host graph
maintenance with GPU local joins, using a host update thread and transfers on
each iteration. The library also exposes `ITERATIVE_CAGRA_SEARCH` through its
[unchanged C API implementation](https://github.com/AMD-Ecosystem/hipVS/blob/f96a663d0a4441d6f5344bd3ef552b71ae7b370b/cpp/src/neighbors/cagra_c.cpp).
Its [iterative builder](https://github.com/AMD-Ecosystem/hipVS/blob/f96a663d0a4441d6f5344bd3ef552b71ae7b370b/cpp/src/neighbors/detail/cagra/cagra_build.cuh)
expands a connected seed graph using batched CAGRA search, reaches the entire
dataset, and refines the complete graph. This can change the balance of CPU work,
GPU work and transfers. It still performs host graph optimization; no claim is
made that the entire construction stays on the GPU or that it is faster here.

`seekdb_cuvs_build` selects that supported builder when the number of vectors
exceeds twice the intermediate graph degree. Smaller indexes retain NN-descent.
This guard follows its small-input `ceil(n/2)` seed graph and build Top-k bound,
not a dataset identity: requesting 256 neighbors from an initial graph with
fewer nodes is unsafe. Both paths use all FP32 rows, graph degree 128 and
intermediate degree 256. The NN-descent control retains its 100-iteration limit;
the iterative builder uses its own fixed-library schedule. Search parameters
and the fixed CAGRA search implementation are unchanged. After either build,
the bridge validates the returned row count and dimension before publishing it.

Changing the algorithm changes the graph. **Recall, peak VRAM and end-to-end
build time are unverified** and may regress. The lower-scoring mapped path is
disabled, but that alone does not establish a speedup. Use this as an experimental
candidate, not as a proven replacement for the 6.943-point commit.

Windows host checks passed 5/5, plus the compatibility-header adaptor syntax
check. On the existing NVIDIA-only server, GCC 13.3 host checks passed 5/5 under
ASan/UBSan with warnings treated as errors. The production bridge also passed C
and C++ syntax checks against the pinned hipVS C headers (CUDA/DLPack declarations
were test substitutes; no ROCm link was performed). `test_build_quality.cpp`
compiled to an object and the canonical LF build script passed Bash syntax
checking. These are recorded in
[`validation/host_iterative_20261109.json`](validation/host_iterative_20261109.json).
The optional NN-descent control is tested with the same host lifecycle suite.

`test_build_quality.cpp` is a new **real-GPU** diagnostic entry point. It uses
256/512/513-row boundary cases with dimension 33, an 8,193-row uniform fixture
with dimension 128, and an 8,193-row clustered fixture with dimension 256. For
64 nearby/independent probes, it computes exact CPU Top-100 ground truth from
all original rows, then checks GPU Top-10/100 IDs, distances, output guards,
Recall and reload behavior. Its 0.95 fixture threshold does not replace official
dataset-specific or baseline-relative Recall gates. GPU execution of this test
is pending; its source compilation is not a quality result.

The historical commit included an NN-descent build-time comparison switch.
That switch and the iterative default are removed in the fifth candidate.
Each fixture destroys its built index before reloading, and deletes its own
successful test snapshot. A failed fixture retains its snapshot for diagnosis;
use a fresh prefix for the next run. Separate-process persistence remains covered
by `test_persistence.cpp`. Neither diagnostic's raw bridge timings are SQL build
scores: full seekdb compilation, fresh independent instances and the unchanged
official paired workload are still required.

## Third candidate: mapped snapshot transfer and phase timings

The user's platform result for `79f8929b4caa733598962d110779c1c45d56780a`
was **6.943/20**, with `build=1.077x, query=1.749x, gates=pass`.
The query component has reached its two-point ceiling under the supplied scoring
formula. Construction is the remaining performance target. This result belongs
to the second candidate, not to the changes in this section.

Source inspection found that graph serialization stages bytes in a private
file, then copies them through a 1 MiB buffer before the existing seekdb stream
copies them again. On Linux, `cuvs_copy_snapshot` now maps the completed file
read-only and writes all its bytes through the same durable output stream.
The format remains `OBCUVS02`; IDs, FP32 vectors and the complete graph remain
present. The mapping aliases existing file pages instead of allocating another
graph-sized payload. It is released before returning, including on exceptions.
Failure to map retains the buffered path. Output errors stop the operation;
they never retry a partially written stream or report success.

The existing snapshot completion log now separates these wall-clock durations:

| Field | Scope |
| --- | --- |
| `gpu_build_us` | Adaptor build call, including its host work, thread setup and GPU synchronization; not a kernel-only timer |
| `graph_serialize_us` | hipVS graph serialization and staging-file setup |
| `payload_write_us` | Header, IDs and FP32 vectors written to the seekdb stream |
| `graph_write_us` | Graph staging transfer to that stream |
| `snapshot_total_us` | Entire adaptor serialization call through these writes |
| `graph_mapped` | Whether mapped transfer actually ran |

`build_us` retains its earlier combined build-plus-serialization meaning.
These timings exclude prior row scanning/LOB decoding and later stream finishing
and DDL publication. Use the official end-to-end timing for scoring, and compare
it to these phases to locate the remaining cost. The hipVS binary, graph and
search parameters are unchanged.

On 2026-11-09, a shared Ubuntu 24.04 server with two Xeon Gold 6430 CPUs ran
GCC 13.3.0 checks. Its GPUs are NVIDIA; ROCm/hipVS are not installed. All four
standalone host tests passed with AddressSanitizer and UndefinedBehaviorSanitizer
and `-Wall -Wextra -Werror`. All four also passed on Windows, where the modified
adaptor passed the existing compatibility-header syntax check. The new snapshot
test checks full byte equality, page/chunk boundaries, memory-backed and regular
temporary files, an injected mapping failure, truncated input, partial output
and exception cleanup. Bridge test doubles still do not exercise a GPU.

A separate optimized host microbenchmark uses the **production copy helper** in
buffered and mapped modes, with a synthetic bounded stream that copies every
byte. It does not perform durable I/O or run seekdb/hipVS. One warmup pair precedes
seven alternating-order pairs, pinned to logical CPU 0 on the shared server:

| Staged bytes | Buffered mean | Mapped mean | Transfer time reduction |
| --- | ---: | ---: | ---: |
| 64 MiB + 37 | 17.533 ms | 12.755 ms | 27.25% |
| 256 MiB + 37 | 69.044 ms | 59.549 ms | 13.75% |
| 512 MiB + 37 | 92.725 ms | 83.852 ms | 9.57% |

The saving is roughly 5–9 ms in this experiment; it is a small integration
improvement, not evidence of a large build-speed increase. Shared-host load and
different hardware affect these numbers. Commands, exit codes, full host-test
output, raw samples and source hashes are in
[`validation/host_snapshot_20261109.json`](validation/host_snapshot_20261109.json).
To reproduce the host experiment from the repository root on Linux:

```sh
cmake -S unittest/oblib/vector/cuvs -B build_cuvs_host_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_cuvs_host_release --parallel 2
ctest --test-dir build_cuvs_host_release --output-on-failure
taskset -c 0 build_cuvs_host_release/test_cuvs_snapshot_io --benchmark
```

Choose an allowed CPU if CPU 0 is unavailable. The third candidate still needs
the official Linux bridge/seekdb build, SQL persistence checks, paired build
timing, Recall and resource gates on AMD. None of those are claimed by this
host-only experiment.

## Second candidate: batch offline DDL input

The user reported 6/20 for the first candidate (`b206db389e476b53a709db9fd5feb28ceb9fe537`),
without a build/query breakdown. Under the supplied scoring rules that implies
the gates passed in that run; it does not identify which stage earned performance
points. The later second-candidate result is recorded above.

`ObHNSWIndexAppendBufferOperator` previously read rows in batches but acquired an
adaptor guard and called `add_snap_index` once per vector. The new path packs
up to 256 vectors per append, with at most 1 MiB of vector scratch plus 2 KiB of
IDs. It holds the adaptor guard for one input row file. LOB decoding still occurs
for each non-null row, followed by an owned byte copy before LOB scratch is reused.
The last partial batch is flushed at EOF; read or append failures propagate.
No input rows, vector dimensions or FP32 bits are removed or changed.

Batching is selected from parsed index metadata only for dense `lib=cuvs` L2
HNSW/HGRAPH indexes without extra payload columns. VSAG, sparse, quantized and
extra-payload paths retain row-wise appends. NULL/empty vectors retain the
existing skip behavior. Vector byte lengths are checked against the schema.
This borrows the batching approach used in the FTS optimization, without
importing FTS parser, ranking or posting-list changes.

The resource-aware local DDL worker policy from FTS commit
`1b0ef73d7b04e38db86525097c8c2e07f29e8dad` is also ported: a mini-mode instance
with at least 4 GiB configured memory and two effective CPUs uses two local DDL
workers. Smaller instances keep one; non-mini instances retain sixteen. This
allows independent auxiliary index tasks to overlap. Offline GPU snapshot builds
remain serialized by the existing adaptor registry lock; the policy does not
run those CAGRA builds at once.
The selected worker count is logged during initialization. There is no benefit
from this policy if the platform instance already uses non-mini mode.

The bridge, graph construction parameters, search parameters, persistence format
and fixed hipVS library are unchanged from the first candidate. The second
candidate does not claim that pre-GPU overhead dominates total build time.

New host test `cuvs_row_batch` checks bounded memory, unaligned byte-preserving
copies, malformed lengths, multiple dimensions, signed 64-bit IDs, scratch reuse,
full batches and partial tails. Windows checks compile the local DDL executor
translation unit and the changed append methods extracted verbatim from the
pipeline, using developer compatibility headers and V5 vector declarations.
The complete pipeline translation unit cannot be checked with those older
Windows storage headers. This is not a full Linux build or a SQL integration run.

Two SQL regressions are delivered for a dedicated GPU-enabled instance. They
require PyMySQL and create/drop randomly named temporary databases:

```sh
SEEKDB_TEST_PORT=2881 python3 unittest/rootserver/test_cuvs_ddl_batch.py
SEEKDB_TEST_PORT=2881 python3 unittest/rootserver/test_ddl_build_concurrency.py
```

The first checks offline GPU/VSAG DDL, non-contiguous signed IDs, NULL vectors,
more than one batch, a partial tail and an empty table. The second checks concurrent
index DDL, transactional writes and failed unique-index isolation. They are
pending execution on this candidate. SQL assertions alone do not establish GPU
path usage or full Recall; the official profiler, Recall and resource gates still
apply. Preserve the 6-point first candidate for paired comparison.

## Observed overhead and changes

Source inspection of V5 found that each SQL probe creates and joins a pthread
with a 32 MiB stack. The bridge also creates search parameters and allocates and
frees three device buffers for each probe. These costs repeat even when the
index, vector dimension and requested result count are unchanged.

| Path | V5 | Candidate |
| --- | --- | --- |
| Single SQL probe | New large-stack thread | One lazy worker per live index; synchronous dispatch |
| Query scratch space | Three allocations and frees per probe | Reuse capacity; grow when needed; free with index |
| Search parameters | Create/destroy per probe | One object per index, recompute `itopk_size` on every call |
| GPU ordering | Default stream and blocking copies | One explicit stream; ordered copies/search and completion before return |
| Snapshot staging | Filesystem temporary file, 64 KiB copy chunks | Linux anonymous memory-backed file, 1 MiB chunks; temporary-file fallback |

The worker joins before the index and its input buffers are destroyed. Concurrent
submissions are serialized. A failed callback returns failure; a subsequent call
can still execute. Rebuilds prepare the GPU handle and ID map before publishing
the replacement. Bridge failures return an error and synchronize outstanding
work before the caller releases its host buffers.

Scratch space retains capacity, not query results. Every probe copies its own
input and calls `cuvsCagraSearch`. At one query, dimension 256 and Top-100, the
three retained bridge buffers total 1,824 bytes (excluding hipVS internal memory).
Larger requests grow these buffers and retain the new capacity until index
destruction. Each started worker reserves a 32 MiB stack; multi-index workloads
therefore need separate memory measurements.

The anonymous snapshot file is transient: the complete `OBCUVS02` payload,
including IDs, vectors and serialized CAGRA index, still passes through seekdb's
durable output stream. Deserialization reads that persisted payload. There is
no cross-run index cache. The third-candidate host tests exercise Linux staging;
full snapshot memory cost still needs measurement on the contest host.

## First/second candidate semantics and parameter parity

- GPU construction remains NN_DESCENT with 100 iterations, graph degree 128 and
  intermediate graph degree 256.
- GPU search keeps `search_width=4` and
  `itopk_size=max(hipVS default, 512, 4*topk)`, including after a larger Top-k call.
- Input remains all FP32 vectors, without sampling or dimension changes.
- Every successful bridge search completes GPU execution and result transfer
  before returning. Query execution is not moved outside the measured request.
- The original ID mapping, L2 semantics, snapshot format and existing VSAG
  fallback decisions remain in the adaptor. Formal ANN probes must actually
  take the GPU path; a CPU fallback is not evidence of passing the contest.
- Official data, benchmark SQL, scoring scripts and the hipVS library are
  unchanged. The formal workload is individual SQL probes; batch API timing
  is not a substitute.

The C API signatures were checked against the pinned
[hipVS resource API](https://github.com/AMD-Ecosystem/hipVS/blob/f96a663d0a4441d6f5344bd3ef552b71ae7b370b/cpp/include/cuvs/core/c_api.h)
and
[CAGRA API](https://github.com/AMD-Ecosystem/hipVS/blob/f96a663d0a4441d6f5344bd3ef552b71ae7b370b/cpp/include/cuvs/neighbors/cagra.h).

## Host regression checks

From the repository root, with a C11/C++17 compiler, CMake and pthreads:

```sh
cmake -S unittest/oblib/vector/cuvs -B build_cuvs_host -DCMAKE_BUILD_TYPE=Release
cmake --build build_cuvs_host
ctest --test-dir build_cuvs_host --output-on-failure
```

On Windows, supply `-DPTHREADS_ROOT=<pthreads4w-prefix>` to configuration. The
prefix must contain `include/`, `lib/pthreadVC3.lib` and `bin/pthreadVC3.dll`.

`cuvs_bridge_lifecycle` compiles the production bridge against test doubles.
It checks parameter parity, a search call for each varying input, warm buffer
reuse, changing batch/Top-k shapes, asynchronous ordering, allocation/copy/search
failures, cleanup and snapshot dimension validation. These doubles are scoped to
this standalone test target; the production bridge build never includes them.
They cannot establish GPU execution, Recall or GPU memory consumption.

`cuvs_worker_lifecycle` checks 1,800 varying/concurrent jobs, thread reuse,
exception recovery and 50 create/destroy lifecycles. Its printed host dispatch
timing is diagnostic only, not an SQL or GPU benchmark.

## GPU compilation and persistence checks

Use the contest's frozen dependencies. Example output directory `/tmp/amd-cagra`:

```sh
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh /tmp/amd-cagra/libseekdb_cuvs_bridge.so
cmake -S . -B build_release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF -DCUVS_BRIDGE_LIB=/tmp/amd-cagra/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
c++ -std=c++17 tools/hipvs_bridge/test_persistence.cpp -Itools/hipvs_bridge -L/tmp/amd-cagra -lseekdb_cuvs_bridge -Wl,-rpath,/tmp/amd-cagra -Wl,-rpath-link,/opt/hipvs/lib -Wl,-rpath-link,/opt/rocm/lib -o /tmp/amd-cagra/test_persistence
LD_LIBRARY_PATH=/tmp/amd-cagra:/opt/hipvs/lib:/opt/rocm/lib /tmp/amd-cagra/test_persistence write /tmp/amd-cagra/snapshot.bin
LD_LIBRARY_PATH=/tmp/amd-cagra:/opt/hipvs/lib:/opt/rocm/lib /tmp/amd-cagra/test_persistence read /tmp/amd-cagra/snapshot.bin
```

The last two commands are separate processes. They exercise actual hipVS
serialization/deserialization, Top-10/100/200 then Top-10 again, one and three
query inputs, output guards, valid unique IDs and self-neighbor distances.
The fixture is a smoke test, not the official Recall dataset. Follow it with the
existing SQL DDL/restart smoke checks in `validation/` and the unchanged official
benchmark on clean, independent instances for V5 and the candidate.

## Historical evaluation status before 2026-11-10

Windows and Linux host checks cover control flow, CPU quota parsing, snapshot
transfer and the retained lossless decoder. The seventh candidate additionally
passed actual ROCm linking, GPU bridge execution and synthetic Recall checks as
recorded above. It has not run the complete seekdb/official workload.

| Required comparison | Status before platform testing |
| --- | --- |
| Official Linux bridge and seekdb build | Full seekdb/official environment pending; bridge compiled on ROCm 7.2.1 |
| Fresh bootstrap and SQL GPU execution | Pending |
| Restart, complete persisted index and valid unique Top-k IDs | GPU bridge cross-process check passed; full SQL restart pending |
| SIFT and hidden-dataset Recall@10/@100 gates and baseline-relative decline | Pending |
| Construction including first-query readiness and persistence | Pending |
| 4,096 individual SQL query throughput, Top-10 and Top-100 | Pending |
| Peak host memory and GPU memory, paired with V5 | Pending |

Measure the same inputs, SQL, resource limits and compilation flags in each
paired run. Record both dataset results, Recall, build time, query throughput,
peak memory and GPU-path evidence. This candidate addresses measured CPU thread
contention and persistence overhead without changing graph construction
parameters. No official performance score is predicted from these component
tests, and sampled device-wide VRAM is not a substitute for the paired gate.
