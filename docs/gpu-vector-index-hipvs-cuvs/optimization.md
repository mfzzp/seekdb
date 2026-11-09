# AMD CAGRA candidate: reuse query resources

This candidate is based on contest V5 commit
`634888d9b037153fa143d2c913ba4b25503e1d62`. It uses the unmodified hipVS
commit `f96a663d0a4441d6f5344bd3ef552b71ae7b370b` supplied by the contest.
It needs GPU validation before any speedup, Recall or contest score can be claimed.

## Second candidate: batch offline DDL input

The user reported 6/20 for the first candidate (`b206db389e476b53a709db9fd5feb28ceb9fe537`),
without a build/query breakdown. Under the supplied scoring rules that implies
the gates passed in that run; it does not identify which stage earned performance
points. The changes below have not yet been evaluated on the GPU platform.

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
no cross-run index cache. Linux snapshot staging and its memory cost still need
measurement on the contest host.

## Semantics and parameter parity

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

Development on Windows without an AMD GPU can check host control flow and
compile interfaces. It cannot validate the Linux memory-backed file branch,
ROCm linking, GPU execution, full seekdb startup, SQL persistence or Recall.

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
