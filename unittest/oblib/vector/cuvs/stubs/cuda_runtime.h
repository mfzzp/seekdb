// Test double declarations; never used by tools/hipvs_bridge/build.sh.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct TestStream *cudaStream_t;
typedef int cudaError_t;
typedef int cudaDataType_t; // opaque enum used only by unrelated IVF-PQ declarations
enum {cudaSuccess = 0, cudaMemcpyDefault = 0, cudaMemcpyHostToDevice = 1, cudaMemcpyDeviceToHost = 2};
cudaError_t cudaStreamCreate(cudaStream_t *stream);
cudaError_t cudaStreamDestroy(cudaStream_t stream);
cudaError_t cudaMemcpyAsync(void *destination, const void *source, size_t bytes, int kind, cudaStream_t stream);
#ifdef __cplusplus
}
#endif
