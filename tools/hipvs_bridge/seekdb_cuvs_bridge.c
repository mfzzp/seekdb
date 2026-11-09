/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  cuvsCagraSearchParams_t sp;
  cudaStream_t stream;
  void *query_buffer, *neighbor_buffer, *distance_buffer;
  size_t query_bytes, neighbor_bytes, distance_bytes, default_itopk;
  long n, dim;
} sk_cuvs_index;

/* One explicit stream survives the short-lived build/deserialize threads and
 * the index's persistent search worker. Never depend on a per-thread default. */
static sk_cuvs_index* sk_create_index(void)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }
  if (cudaStreamCreate(&h->stream) != cudaSuccess ||
      cuvsStreamSet(h->res, h->stream) != CUVS_SUCCESS ||
      cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS ||
      cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS ||
      cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) {
    seekdb_cuvs_free(h);
    return NULL;
  }
  h->default_itopk = h->sp->itopk_size;
  h->sp->search_width = 4;
  return h;
}

static int sk_matrix_bytes(long rows, long columns, size_t element_size, size_t* bytes)
{
  if (rows <= 0 || columns <= 0 || (uint64_t)rows > SIZE_MAX / element_size /
      (uint64_t)columns) return 1;
  *bytes = (size_t)rows * (size_t)columns * element_size;
  return 0;
}

/* Calls on a handle are serialized by the adaptor. No result cache is kept:
 * all queries still copy their inputs and execute the fixed hipVS search. */
static int sk_reserve(sk_cuvs_index* h, void** buffer, size_t* capacity, size_t bytes)
{
  if (*capacity >= bytes) return 0;
  if (*buffer) {
    if (cuvsRMMFree(h->res, *buffer, *capacity) != CUVS_SUCCESS) return 1;
    *buffer = NULL;
    *capacity = 0;
  }
  if (cuvsRMMAlloc(h->res, buffer, bytes) != CUVS_SUCCESS) return 1;
  *capacity = bytes;
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  size_t bytes = 0;
  if (!base || (uint64_t)n > UINT32_MAX || sk_matrix_bytes(n, dim, sizeof(float), &bytes)) return NULL;
  sk_cuvs_index* h = sk_create_index();
  if (!h) return NULL;
  DLManagedTensor dset = {0}; int64_t ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 256;
  h->ip->nn_descent_niter = 100;
  /* Restore the construction path that passed the platform's Recall/VRAM gates.
   * Persistence optimization must not change the graph or search parameters. */
  h->ip->build_algo = NN_DESCENT;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS ||
      cuvsStreamSync(h->res) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] build failed (algo=%d, n=%ld, dim=%ld): %s\n",
            (int)h->ip->build_algo, n, dim, cuvsGetLastErrorText());
    seekdb_cuvs_free(h);
    return NULL;
  }
  {
    int64_t actual_n = 0, actual_dim = 0;
    if (cuvsCagraIndexGetSize(h->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(h->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] built index shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      seekdb_cuvs_free(h);
      return NULL;
    }
  }
  fprintf(stderr, "[seekdb_cuvs] build complete algo=%s n=%ld dim=%ld graph_degree=%zu intermediate_degree=%zu\n",
          h->ip->build_algo == NN_DESCENT ? "nn_descent" : "iterative_cagra_search",
          n, dim, h->ip->graph_degree, h->ip->intermediate_graph_degree);
  h->n = n; h->dim = dim;
  return h;
}

/* Copy the index-owned FP32 dataset to an existing host buffer, without an
 * extra device allocation. Used to restore the CPU fallback/rebuild buffer
 * from a complete hipVS snapshot instead of persisting those vectors twice. */
int seekdb_cuvs_export_vectors(void* handle, float* output, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  size_t bytes = 0, width = 0, pitch = 0;
  uintptr_t address = 0;
  int rc = 1;
  DLManagedTensor view = {0};
  if (!h || !output || n != h->n || dim != h->dim ||
      sk_matrix_bytes(n, dim, sizeof(float), &bytes)) return 1;
  if (cuvsCagraIndexGetDataset(h->idx, &view) != CUVS_SUCCESS) goto done;
  if (!view.dl_tensor.data || !view.dl_tensor.shape || view.dl_tensor.ndim != 2 ||
      view.dl_tensor.device.device_type != kDLCUDA ||
      view.dl_tensor.dtype.code != kDLFloat || view.dl_tensor.dtype.bits != 32 ||
      view.dl_tensor.dtype.lanes != 1 || view.dl_tensor.shape[0] != n || view.dl_tensor.shape[1] != dim) goto done;
  width = bytes / (size_t)n;
  pitch = width;
  if (view.dl_tensor.strides) {
    if (view.dl_tensor.strides[1] != 1 || view.dl_tensor.strides[0] < dim ||
        (uint64_t)view.dl_tensor.strides[0] > SIZE_MAX / sizeof(float)) goto done;
    pitch = (size_t)view.dl_tensor.strides[0] * sizeof(float);
  }
  address = (uintptr_t)view.dl_tensor.data;
  if (view.dl_tensor.byte_offset > UINTPTR_MAX - address) goto done;
  address += (uintptr_t)view.dl_tensor.byte_offset;
  if (width > UINTPTR_MAX - address ||
      (uint64_t)(n - 1) > (UINTPTR_MAX - address - width) / pitch) goto done;
  if (cudaMemcpy2DAsync(output, width, (const void*)address, pitch, width, (size_t)n,
                         cudaMemcpyDeviceToHost, h->stream) != cudaSuccess) goto done;
  rc = 0;
done:
  // Complete even failed submissions before releasing metadata or caller buffers.
  if (cuvsStreamSync(h->res) != CUVS_SUCCESS) rc = 1;
  if (view.deleter) view.deleter(&view);
  if (rc) fprintf(stderr, "[seekdb_cuvs] cannot restore complete FP32 dataset from index\n");
  return rc;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  size_t qbytes = 0, nbytes = 0, dbytes = 0;
  if (!h || !query || !out_ids || topk > h->n ||
      sk_matrix_bytes(nq, h->dim, sizeof(float), &qbytes) ||
      sk_matrix_bytes(nq, topk, sizeof(unsigned), &nbytes) ||
      sk_matrix_bytes(nq, topk, sizeof(float), &dbytes) ||
      (uint64_t)topk > SIZE_MAX / 4) return 1;
  int rc = 0;
  if (sk_reserve(h, &h->query_buffer, &h->query_bytes, qbytes)) { rc=10; goto done; }
  if (sk_reserve(h, &h->neighbor_buffer, &h->neighbor_bytes, nbytes)) { rc=11; goto done; }
  if (sk_reserve(h, &h->distance_buffer, &h->distance_bytes, dbytes)) { rc=12; goto done; }
  if (cudaMemcpyAsync(h->query_buffer, query, qbytes, cudaMemcpyHostToDevice, h->stream) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt = {0}, nt = {0}, dt = {0}; int64_t qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->query_buffer; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->neighbor_buffer; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->distance_buffer; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      h->sp->itopk_size = h->default_itopk > required_itopk ? h->default_itopk : required_itopk;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpyAsync(out_ids, h->neighbor_buffer, nbytes, cudaMemcpyDeviceToHost, h->stream) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpyAsync(out_dist, h->distance_buffer, dbytes, cudaMemcpyDeviceToHost, h->stream) != cudaSuccess) { rc=17; goto done; }
  }
done:
  // Wait even after an error: a caller may release its host buffers on return.
  if (cuvsStreamSync(h->res) != CUVS_SUCCESS && rc == 0) rc=18;
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->res) {
    cuvsStreamSync(h->res);
    if (h->distance_buffer) cuvsRMMFree(h->res, h->distance_buffer, h->distance_bytes);
    if (h->neighbor_buffer) cuvsRMMFree(h->res, h->neighbor_buffer, h->neighbor_bytes);
    if (h->query_buffer) cuvsRMMFree(h->res, h->query_buffer, h->query_bytes);
    if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
    if (h->idx) cuvsCagraIndexDestroy(h->idx);
    if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
    cuvsStreamSync(h->res);
    cuvsResourcesDestroy(h->res);
  }
  if (h->stream) cudaStreamDestroy(h->stream);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  const cuvsError_t result = cuvsCagraSerialize(index->res, filename, index->idx, true);
  const cuvsError_t sync_result = cuvsStreamSync(index->res);
  if (result != CUVS_SUCCESS || sync_result != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  size_t bytes = 0;
  if (!filename || (uint64_t)n > UINT32_MAX || sk_matrix_bytes(n, dim, sizeof(float), &bytes)) return NULL;
  sk_cuvs_index* index = sk_create_index();
  if (!index) return NULL;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS ||
      cuvsStreamSync(index->res) != CUVS_SUCCESS) goto fail;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  seekdb_cuvs_free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
