// GPU API doubles: checks ordering/ownership, not CAGRA quality or performance.
#include "seekdb_cuvs_bridge.h"
#include <cuvs/neighbors/cagra.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "check failed line %d: %s\n", __LINE__, #condition); std::abort(); } } while (0)
struct TestStream { std::vector<std::function<void()>> pending; };
struct Resources { cudaStream_t stream = nullptr; };
struct TestIndex { int64_t n = 0, dim = 0; };
static int resources_live, streams_live, indexes_live, build_params_live, search_params_live;
static int allocation_calls, search_calls, search_param_creates;
static int serialization_completions;
static int dataset_metadata_live, dataset_deletions, export_copies;
static DLTensor export_dataset = {};
static const char *bad_dataset = "";
static int alloc_fail_countdown, copy_fail_countdown;
static std::map<void *, size_t> allocations;
static std::string failure;
static bool fail(const char *name) { if (failure == name) { failure.clear(); return true; } return false; }
static Resources *resource(cuvsResources_t r) { return reinterpret_cast<Resources *>(r); }
static void drained(cudaStream_t stream) { if (stream) { for (auto &task : stream->pending) task(); stream->pending.clear(); } }
static void no_leaks()
{
  CHECK(!resources_live && !streams_live && !indexes_live && !build_params_live && !search_params_live &&
        !dataset_metadata_live && allocations.empty());
}

extern "C" {
const char *cuvsGetLastErrorText(void) { return "host-test injected failure"; }
cuvsError_t cuvsResourcesCreate(cuvsResources_t *r) { if (fail("resources")) return CUVS_ERROR; *r = reinterpret_cast<uintptr_t>(new Resources); ++resources_live; return CUVS_SUCCESS; }
cuvsError_t cuvsResourcesDestroy(cuvsResources_t r) { delete resource(r); --resources_live; return CUVS_SUCCESS; }
cudaError_t cudaStreamCreate(cudaStream_t *s) { if (fail("stream")) return 1; *s = new TestStream; ++streams_live; return cudaSuccess; }
cudaError_t cudaStreamDestroy(cudaStream_t s) { CHECK(s->pending.empty()); delete s; --streams_live; return cudaSuccess; }
cuvsError_t cuvsStreamSet(cuvsResources_t r, cudaStream_t s) { if (fail("stream-set")) return CUVS_ERROR; resource(r)->stream = s; return CUVS_SUCCESS; }
cuvsError_t cuvsStreamSync(cuvsResources_t r) { drained(resource(r)->stream); return fail("sync") ? CUVS_ERROR : CUVS_SUCCESS; }
cudaError_t cudaMemcpyAsync(void *dest, const void *source, size_t bytes, int, cudaStream_t stream)
{
  CHECK(stream);
  if (copy_fail_countdown && --copy_fail_countdown == 0) return 1;
  stream->pending.push_back([=] { std::memcpy(dest, source, bytes); });
  return cudaSuccess;
}
cudaError_t cudaMemcpy2DAsync(void *dest, size_t dpitch, const void *source, size_t spitch,
                            size_t width, size_t height, int kind, cudaStream_t stream)
{
  CHECK(stream && kind == cudaMemcpyDeviceToHost && dpitch >= width && spitch >= width);
  ++export_copies;
  if (copy_fail_countdown && --copy_fail_countdown == 0) return 1;
  stream->pending.push_back([=] {
    for (size_t row = 0; row < height; ++row) {
      std::memcpy(static_cast<char *>(dest) + row * dpitch,
                  static_cast<const char *>(source) + row * spitch, width);
    }
  });
  return cudaSuccess;
}
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void **pointer, size_t bytes)
{
  ++allocation_calls;
  if (alloc_fail_countdown && --alloc_fail_countdown == 0) return CUVS_ERROR;
  *pointer = std::malloc(bytes); CHECK(*pointer); allocations[*pointer] = bytes; return CUVS_SUCCESS;
}
cuvsError_t cuvsRMMFree(cuvsResources_t r, void *pointer, size_t bytes)
{
  CHECK(!resource(r)->stream || resource(r)->stream->pending.empty());
  CHECK(allocations.count(pointer) == 1 && allocations[pointer] == bytes);
  allocations.erase(pointer); std::free(pointer); return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *p) { if (fail("build-params")) return CUVS_ERROR; *p = new std::remove_pointer<cuvsCagraIndexParams_t>::type{}; ++build_params_live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t p) { delete p; --build_params_live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *p) { if (fail("search-params")) return CUVS_ERROR; *p = new std::remove_pointer<cuvsCagraSearchParams_t>::type{64, 1, SINGLE_CTA}; ++search_params_live; ++search_param_creates; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p) { delete p; --search_params_live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *p) { if (fail("index")) return CUVS_ERROR; *p = new TestIndex; ++indexes_live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t p) { delete p; --indexes_live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraBuild(cuvsResources_t r, cuvsCagraIndexParams_t p, DLManagedTensor *d, cuvsCagraIndex_t i)
{
  CHECK(p->build_algo == NN_DESCENT && p->graph_degree == 128 && p->intermediate_graph_degree == 256 && p->nn_descent_niter == 100);
  CHECK(d->dl_tensor.device.device_type == kDLCPU && d->dl_tensor.dtype.bits == 32);
  if (fail("build")) return CUVS_ERROR;
  i->n = d->dl_tensor.shape[0]; i->dim = d->dl_tensor.shape[1];
  if (fail("incomplete-graph")) { --i->n; }
  if (fail("wrong-dimension")) { --i->dim; }
  resource(r)->stream->pending.push_back([] {});
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearch(cuvsResources_t r, cuvsCagraSearchParams_t p, cuvsCagraIndex_t i, DLManagedTensor *q, DLManagedTensor *n, DLManagedTensor *d, cuvsFilter f)
{
  CHECK(f.type == NO_FILTER && f.addr == 0);
  const size_t nq = q->dl_tensor.shape[0], dim = q->dl_tensor.shape[1], k = n->dl_tensor.shape[1];
  CHECK(p->itopk_size == std::max<size_t>(512, k * 4) && p->search_width == 4);
  CHECK(p->algo == (p->itopk_size > 512 ? MULTI_CTA : SINGLE_CTA));
  CHECK(dim == static_cast<size_t>(i->dim));
  CHECK(q->dl_tensor.dtype.bits == 32 && n->dl_tensor.dtype.code == kDLUInt && d->dl_tensor.dtype.bits == 32);
  ++search_calls;
  if (fail("search")) return CUVS_ERROR;
  const float *queries = static_cast<const float *>(q->dl_tensor.data);
  unsigned *neighbors = static_cast<unsigned *>(n->dl_tensor.data);
  float *distances = static_cast<float *>(d->dl_tensor.data);
  resource(r)->stream->pending.push_back([=] {
    for (size_t row = 0; row < nq; ++row) for (size_t col = 0; col < k; ++col) {
      neighbors[row * k + col] = (static_cast<unsigned>(queries[row * dim]) + col) % i->n;
      distances[row * k + col] = queries[row * dim] + static_cast<float>(col);
    }
  });
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSerialize(cuvsResources_t r, const char *, cuvsCagraIndex_t, bool include_dataset)
{
  CHECK(include_dataset);
  resource(r)->stream->pending.push_back([] { ++serialization_completions; });
  return fail("serialize") ? CUVS_ERROR : CUVS_SUCCESS;
}
cuvsError_t cuvsCagraDeserialize(cuvsResources_t, const char *, cuvsCagraIndex_t i) { if (fail("deserialize")) return CUVS_ERROR; i->n = 1000; i->dim = 32; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t i, int64_t *n) { if (fail("get-size")) return CUVS_ERROR; *n = i->n; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t i, int64_t *d) { if (fail("get-dims")) return CUVS_ERROR; *d = i->dim; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexGetDataset(cuvsCagraIndex_t, DLManagedTensor *out)
{
  if (fail("get-dataset")) return CUVS_ERROR;
  out->dl_tensor = export_dataset;
  // Match the pinned API: a non-owning device view with owned host metadata.
  out->dl_tensor.shape = new int64_t[2]{export_dataset.shape[0], export_dataset.shape[1]};
  if (export_dataset.strides) {
    out->dl_tensor.strides = new int64_t[2]{export_dataset.strides[0], export_dataset.strides[1]};
  }
  out->manager_ctx = out->dl_tensor.shape;
  ++dataset_metadata_live;
  out->deleter = [](DLManagedTensor *t) {
    delete[] static_cast<int64_t *>(t->manager_ctx);
    delete[] t->dl_tensor.strides;
    --dataset_metadata_live; ++dataset_deletions;
  };
  if (std::strcmp(bad_dataset, "null-shape") == 0) out->dl_tensor.shape = nullptr;
  return fail("get-dataset-after-allocation") ? CUVS_ERROR : CUVS_SUCCESS;
}
}

static void probe(void *handle, long nq, long k, float value, bool distances = true)
{
  std::vector<float> query(nq * 32, value), dist(nq * k, -1);
  std::vector<unsigned> ids(nq * k, UINT32_MAX);
  CHECK(seekdb_cuvs_search(handle, query.data(), nq, k, ids.data(), distances ? dist.data() : nullptr) == 0);
  for (long row = 0; row < nq; ++row) for (long col = 0; col < k; ++col) {
    CHECK(ids[row * k + col] == (static_cast<unsigned>(value) + col) % 1000);
    CHECK(dist[row * k + col] == (distances ? value + static_cast<float>(col) : -1));
  }
}

static void check_vector_export()
{
  constexpr long n = 7, dim = 33;
  const uint32_t patterns[] = {0u, 0x80000000u, 0x7fc00001u, 0x7f800000u, 0xff800000u, 0x3f123456u, 1u};
  float unused = 0;
  void *h = seekdb_cuvs_build(&unused, n, dim); CHECK(h);
  for (const long padding : {0L, 7L}) {
    for (const size_t offset : {size_t(0), size_t(16)}) {
      std::vector<unsigned char> source(offset + n * (dim + padding) * sizeof(float), 0xa5);
      std::vector<float> expected(n * dim), output(n * dim + 2);
      for (long row = 0; row < n; ++row) for (long col = 0; col < dim; ++col) {
        const uint32_t bits = patterns[(row * dim + col) % 7];
        std::memcpy(&expected[row * dim + col], &bits, sizeof(bits));
        std::memcpy(source.data() + offset + (row * (dim + padding) + col) * sizeof(float), &bits, sizeof(bits));
      }
      int64_t shape[2] = {n, dim}, strides[2] = {dim + padding, 1};
      export_dataset = {source.data(), {kDLCUDA, 0}, 2, {kDLFloat, 32, 1}, shape,
                        padding ? strides : nullptr, offset};
      const DLTensor valid = export_dataset;
      const int allocs = allocation_calls;
      auto good_copy = [&] {
        std::memset(output.data(), 0xa5, output.size() * sizeof(float));
        CHECK(seekdb_cuvs_export_vectors(h, output.data() + 1, n, dim) == 0);
        CHECK(std::memcmp(output.data() + 1, expected.data(), n * dim * sizeof(float)) == 0);
        uint32_t first, last;
        std::memcpy(&first, output.data(), 4); std::memcpy(&last, &output.back(), 4);
        CHECK(first == 0xa5a5a5a5u && last == 0xa5a5a5a5u);
        CHECK(dataset_metadata_live == 0 && allocation_calls == allocs);
      };
      good_copy();
      CHECK(seekdb_cuvs_export_vectors(nullptr, output.data(), n, dim) != 0);
      CHECK(seekdb_cuvs_export_vectors(h, nullptr, n, dim) != 0);
      CHECK(seekdb_cuvs_export_vectors(h, output.data(), n - 1, dim) != 0);
      CHECK(seekdb_cuvs_export_vectors(h, output.data(), n, dim + 1) != 0);
      for (const char *stage : {"get-dataset", "get-dataset-after-allocation", "sync"}) {
        failure = stage;
        CHECK(seekdb_cuvs_export_vectors(h, output.data() + 1, n, dim) != 0);
        CHECK(dataset_metadata_live == 0);
        good_copy();
      }
      copy_fail_countdown = 1;
      CHECK(seekdb_cuvs_export_vectors(h, output.data() + 1, n, dim) != 0);
      good_copy();
      for (const char *invalid : {"data", "null-shape", "ndim", "device", "code", "bits", "lanes", "rows", "dim",
                                  "column-stride", "short-stride", "negative-stride", "large-stride", "offset", "span"}) {
        export_dataset = valid;
        shape[0] = n; shape[1] = dim; strides[0] = dim + padding; strides[1] = 1;
        const std::string which(invalid);
        bad_dataset = invalid;
        if (which == "data") export_dataset.data = nullptr;
        if (which == "ndim") export_dataset.ndim = 1;
        if (which == "device") export_dataset.device.device_type = kDLCPU;
        if (which == "code") export_dataset.dtype.code = kDLUInt;
        if (which == "bits") export_dataset.dtype.bits = 16;
        if (which == "lanes") export_dataset.dtype.lanes = 2;
        if (which == "rows") --shape[0];
        if (which == "dim") --shape[1];
        if (which.find("stride") != std::string::npos) export_dataset.strides = strides;
        if (which == "column-stride") strides[1] = 2;
        if (which == "short-stride") strides[0] = dim - 1;
        if (which == "negative-stride") strides[0] = -1;
        if (which == "large-stride") strides[0] = INT64_MAX;
        if (which == "offset") export_dataset.byte_offset = UINT64_MAX;
        if (which == "span") export_dataset.data = reinterpret_cast<void *>(UINTPTR_MAX - dim);
        const int copies = export_copies, deletions = dataset_deletions;
        CHECK(seekdb_cuvs_export_vectors(h, output.data() + 1, n, dim) != 0);
        CHECK(export_copies == copies && dataset_metadata_live == 0 && dataset_deletions == deletions + 1);
      }
      bad_dataset = ""; export_dataset = valid;
      shape[0] = n; shape[1] = dim; strides[0] = dim + padding; strides[1] = 1;
      good_copy();
    }
  }
  seekdb_cuvs_free(h); no_leaks();
}

int main()
{
  float base = 0;
  CHECK(!seekdb_cuvs_build(nullptr, 1000, 32));
  CHECK(!seekdb_cuvs_build(&base, -1, 32));
  CHECK(!seekdb_cuvs_build(&base, 1000, 0));
  if (sizeof(long) == 8) CHECK(!seekdb_cuvs_build(&base, LONG_MAX, LONG_MAX));
  // Retain the previously gate-passing NN-descent recipe for every shape.
  for (const long n : {1L, 255L, 256L, 511L, 512L, 513L, 8193L}) {
    for (const long dim : {1L, 33L, 128L, 256L}) {
      void *h = seekdb_cuvs_build(&base, n, dim); CHECK(h);
      seekdb_cuvs_free(h); no_leaks();
    }
  }
  for (const char *stage : {"resources", "stream", "stream-set", "build-params", "index", "search-params", "build", "sync",
                            "get-size", "get-dims", "incomplete-graph", "wrong-dimension"}) {
    failure = stage; CHECK(!seekdb_cuvs_build(&base, 1000, 32)); no_leaks();
  }
  for (int n = 1; n <= 3; ++n) {
    void *h = seekdb_cuvs_build(&base, 1000, 32); CHECK(h);
    float query[32] = {}, distances[10]; unsigned ids[10];
    alloc_fail_countdown = n;
    CHECK(seekdb_cuvs_search(h, query, 1, 10, ids, distances) != 0);
    probe(h, 1, 10, 31); seekdb_cuvs_free(h); no_leaks();
  }
  const int params_before = search_param_creates;
  void *h = seekdb_cuvs_build(&base, 1000, 32); CHECK(h);
  probe(h, 1, 10, 1);
  const int allocs = allocation_calls, searches = search_calls;
  for (int n = 0; n < 100; ++n) probe(h, 1, 10, static_cast<float>(n));
  CHECK(allocation_calls == allocs && search_calls == searches + 100);
  CHECK(search_param_creates == params_before + 1);
  probe(h, 4, 200, 41); probe(h, 1, 10, 51); probe(h, 2, 100, 61, false);
  float query[32] = {}; unsigned ids[10]; float dist[10];
  CHECK(seekdb_cuvs_search(h, query, 0, 10, ids, dist) != 0);
  CHECK(seekdb_cuvs_search(h, query, 1, 1001, ids, dist) != 0);
  CHECK(seekdb_cuvs_search(h, query, 1, 10, nullptr, dist) != 0);
  for (int n = 1; n <= 3; ++n) {
    copy_fail_countdown = n; CHECK(seekdb_cuvs_search(h, query, 1, 10, ids, dist) != 0); probe(h, 1, 10, 71);
  }
  for (const char *stage : {"search", "sync"}) {
    failure = stage; CHECK(seekdb_cuvs_search(h, query, 1, 10, ids, dist) != 0); probe(h, 1, 10, 81);
  }
  const int serialized = serialization_completions;
  CHECK(seekdb_cuvs_serialize(h, "unused") == 0);
  CHECK(serialization_completions == serialized + 1);
  failure = "serialize"; CHECK(seekdb_cuvs_serialize(h, "unused") != 0);
  CHECK(serialization_completions == serialized + 2);
  seekdb_cuvs_free(h); no_leaks();
  CHECK(!seekdb_cuvs_deserialize("unused", 999, 32)); no_leaks();
  CHECK(!seekdb_cuvs_deserialize("unused", 1000, 31)); no_leaks();
  failure = "deserialize"; CHECK(!seekdb_cuvs_deserialize("unused", 1000, 32)); no_leaks();
  h = seekdb_cuvs_deserialize("unused", 1000, 32); CHECK(h); probe(h, 1, 100, 91); seekdb_cuvs_free(h); no_leaks();
  check_vector_export();
  std::puts("PASS bridge: NN-descent parameter parity, search, buffer reuse, shapes, FP32 dataset export including padding/offsets, async completion, injected errors and release; no GPU used");
}
