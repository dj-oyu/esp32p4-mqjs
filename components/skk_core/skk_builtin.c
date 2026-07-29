/*
 * Access to the dictionary image linked into the firmware, if any.
 *
 * This is the whole of skk_core's contact with "where do the bytes come
 * from". Everything else takes a skk_blob_t and cannot tell rodata from
 * an mmap'd partition from a host malloc — which is exactly what lets
 * the same code run in the unit tests (§6.7 of docs/skk-ime-design.md).
 *
 * SKK_HAVE_BUILTIN_IMAGE is set by CMakeLists when
 * components/skk_core/skk_dict.bin exists at configure time. Without it
 * this returns an empty blob and callers fall back to loading a file,
 * so a tree with no prepared dictionary still builds and links.
 */
#include "skk_core.h"

#ifdef SKK_HAVE_BUILTIN_IMAGE
/* Defined by skk_dict_image.S, 64-byte aligned. Declared as arrays, not
   pointers: these are linker-placed addresses, not storage. */
extern const uint8_t skk_dict_image[];
extern const uint8_t skk_dict_image_end[];
#endif

skk_blob_t skk_builtin_image(void)
{
#ifdef SKK_HAVE_BUILTIN_IMAGE
    skk_blob_t b;
    b.base = skk_dict_image;
    b.len  = (size_t)(skk_dict_image_end - skk_dict_image);
    return b;
#else
    skk_blob_t b;
    b.base = NULL;
    b.len  = 0;
    return b;
#endif
}
