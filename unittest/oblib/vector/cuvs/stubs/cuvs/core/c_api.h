#pragma once
#include <cuda_runtime.h>
#include <dlpack/dlpack.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {CUVS_ERROR, CUVS_SUCCESS} cuvsError_t;
typedef uintptr_t cuvsResources_t;
cuvsError_t cuvsResourcesCreate(cuvsResources_t *resources);
cuvsError_t cuvsResourcesDestroy(cuvsResources_t resources);
cuvsError_t cuvsStreamSet(cuvsResources_t resources, cudaStream_t stream);
cuvsError_t cuvsStreamSync(cuvsResources_t resources);
cuvsError_t cuvsRMMAlloc(cuvsResources_t resources, void **pointer, size_t bytes);
cuvsError_t cuvsRMMFree(cuvsResources_t resources, void *pointer, size_t bytes);
const char *cuvsGetLastErrorText(void);
#ifdef __cplusplus
}
#endif
