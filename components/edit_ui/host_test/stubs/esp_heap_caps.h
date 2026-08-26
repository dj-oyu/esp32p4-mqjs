#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_8BIT     (1u << 2)
void  *heap_caps_aligned_alloc(size_t align, size_t n, uint32_t caps);
void   heap_caps_free(void *p);
size_t heap_caps_get_largest_free_block(uint32_t caps);
