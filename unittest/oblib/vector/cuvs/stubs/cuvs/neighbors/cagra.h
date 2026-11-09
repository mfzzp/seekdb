#pragma once
#include <cuvs/core/c_api.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {AUTO_SELECT, IVF_PQ, NN_DESCENT, ITERATIVE_CAGRA_SEARCH};
typedef struct {size_t graph_degree, intermediate_graph_degree, nn_descent_niter; int build_algo;} *cuvsCagraIndexParams_t;
typedef struct {size_t itopk_size, search_width;} *cuvsCagraSearchParams_t;
typedef struct TestIndex *cuvsCagraIndex_t;
enum {NO_FILTER};
typedef struct {int type; uintptr_t addr;} cuvsFilter;
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *p);
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t p);
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *p);
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p);
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *p);
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t p);
cuvsError_t cuvsCagraBuild(cuvsResources_t r, cuvsCagraIndexParams_t p, DLManagedTensor *d, cuvsCagraIndex_t i);
cuvsError_t cuvsCagraSearch(cuvsResources_t r, cuvsCagraSearchParams_t p, cuvsCagraIndex_t i, DLManagedTensor *q, DLManagedTensor *n, DLManagedTensor *d, cuvsFilter f);
cuvsError_t cuvsCagraSerialize(cuvsResources_t r, const char *filename, cuvsCagraIndex_t i, bool include_dataset);
cuvsError_t cuvsCagraDeserialize(cuvsResources_t r, const char *filename, cuvsCagraIndex_t i);
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t i, int64_t *n);
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t i, int64_t *d);
#ifdef __cplusplus
}
#endif
