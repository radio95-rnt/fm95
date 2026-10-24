#include "bulk.h"
void scale_array(float *restrict arr, size_t n, float scale) {
    // GCC should do auto-vectorization
    for (size_t i = 0; i < n; i++)
        arr[i] *= scale;
}