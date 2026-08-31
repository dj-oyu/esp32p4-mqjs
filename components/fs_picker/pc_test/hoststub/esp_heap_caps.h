#pragma once
#include <stdint.h>
#include <stddef.h>
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_DEFAULT (1 << 12)
#ifdef __cplusplus
extern "C" {
#endif
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps);
#ifdef __cplusplus
}
#endif
