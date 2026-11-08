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
static int alloc_fail_countdown, copy_fail_countdown;
static std::map<void *, size_t> allocations;
static std::string failure;
static bool fail(const char *name) { if (failure == name) { failure.clear(); return true; } return false; }
static Resources *resource(cuvsResources_t r) { return reinterpret_cast<Resources *>(r); }
static void drained(cudaStream_t stream) { if (stream) { for (auto &task : stream->pending) task(); stream->pending.clear(); } }
static void no_leaks()
{
  CHECK(!resources_live && !streams_live && !indexes_live && !build_params_live && !search_params_live && allocations.empty());
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
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *p) { if (fail("search-params")) return CUVS_ERROR; *p = new std::remove_pointer<cuvsCagraSearchParams_t>::type{64, 1}; ++search_params_live; ++search_param_creates; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p) { delete p; --search_params_live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *p) { if (fail("index")) return CUVS_ERROR; *p = new TestIndex; ++indexes_live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t p) { delete p; --indexes_live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraBuild(cuvsResources_t r, cuvsCagraIndexParams_t p, DLManagedTensor *d, cuvsCagraIndex_t i)
{
  CHECK(p->build_algo == NN_DESCENT && p->graph_degree == 128 && p->intermediate_graph_degree == 256 && p->nn_descent_niter == 100);
  CHECK(d->dl_tensor.device.device_type == kDLCPU && d->dl_tensor.dtype.bits == 32);
  if (fail("build")) return CUVS_ERROR;
  i->n = d->dl_tensor.shape[0]; i->dim = d->dl_tensor.shape[1];
  resource(r)->stream->pending.push_back([] {});
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearch(cuvsResources_t r, cuvsCagraSearchParams_t p, cuvsCagraIndex_t i, DLManagedTensor *q, DLManagedTensor *n, DLManagedTensor *d, cuvsFilter f)
{
  CHECK(f.type == NO_FILTER && f.addr == 0);
  const size_t nq = q->dl_tensor.shape[0], dim = q->dl_tensor.shape[1], k = n->dl_tensor.shape[1];
  CHECK(p->itopk_size == std::max<size_t>(512, k * 4) && p->search_width == 4);
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
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t i, int64_t *n) { *n = i->n; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t i, int64_t *d) { *d = i->dim; return CUVS_SUCCESS; }
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

int main()
{
  float base = 0;
  CHECK(!seekdb_cuvs_build(nullptr, 1000, 32));
  CHECK(!seekdb_cuvs_build(&base, -1, 32));
  CHECK(!seekdb_cuvs_build(&base, 1000, 0));
  if (sizeof(long) == 8) CHECK(!seekdb_cuvs_build(&base, LONG_MAX, LONG_MAX));
  for (const char *stage : {"resources", "stream", "stream-set", "build-params", "index", "search-params", "build", "sync"}) {
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
  std::puts("PASS bridge: parameter parity, real call per probe, warm buffer reuse, shape changes, async completion, injected errors and release; no GPU used");
}
