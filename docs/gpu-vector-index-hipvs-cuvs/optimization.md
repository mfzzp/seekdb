# AMD CAGRA candidates: integration costs and compact persistence

This candidate is based on contest V5 commit
`634888d9b037153fa143d2c913ba4b25503e1d62`. It uses the unmodified hipVS
commit `f96a663d0a4441d6f5344bd3ef552b71ae7b370b` supplied by the contest.
Each new candidate needs its own GPU validation. Host measurements below do not
establish SQL performance or Recall.

## Fifth candidate (current): restore NN-descent and store the dataset once

The fourth candidate, `fa64d4537830b163f7ff53240d338c5d06e8b362`, scored
**0/20** on the platform. The supplied failures were `relative_vram_limit` and
`hidden250k:recall@100_baseline_delta`; no numerical VRAM or Recall measurements
were supplied. These are hard gates, not a lack of timing improvement. The
iterative builder is therefore removed from the current candidate, including
its build-time switch. Construction again uses the previously gate-passing
NN-descent recipe: graph degree 128, intermediate degree 256, 100 maximum
iterations. The fixed CAGRA search, its parameters, input FP32 rows, SQL workload
and scoring files are unchanged. The best observed result remains **6.943/20**
at `79f8929b4caa733598962d110779c1c45d56780a`.

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
The highest observed score remains the first row of the table.

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

## Evaluation status

Windows and Linux host checks now cover host control flow and Linux mapped-file
transfer. The current server cannot validate ROCm linking, GPU execution,
full candidate seekdb startup, SQL persistence or Recall. The table below applies
to the fifth candidate; prior platform observations are recorded above.

| Required comparison | Status before platform testing |
| --- | --- |
| Official Linux bridge and seekdb build | Pending |
| Fresh bootstrap and SQL GPU execution | Pending |
| Restart, complete persisted index and valid unique Top-k IDs | Pending |
| SIFT and hidden-dataset Recall@10/@100 gates and baseline-relative decline | Pending |
| Construction including first-query readiness and persistence | Pending |
| 4,096 individual SQL query throughput, Top-10 and Top-100 | Pending |
| Peak host memory and GPU memory, paired with V5 | Pending |

Measure the same inputs, SQL, resource limits and compilation flags in each
paired run. Record both dataset results, Recall, build time, query throughput,
peak memory and GPU-path evidence. This candidate targets repeated integration
overhead; it does not yet tune the graph algorithm. Build-time gains may be
limited if GPU graph construction dominates. No performance score is predicted
from the host-only tests.
