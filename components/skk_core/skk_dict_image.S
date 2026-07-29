/*
 * The built-in SKK dictionary image, linked into flash rodata.
 *
 * WHY NOT `EMBED_FILES`. IDF's tools/cmake/scripts/data_file_embed_asm.cmake
 * emits `.section .rodata.embedded` followed by the label and a .byte
 * stream, with NO .align/.balign/.p2align of any kind — the resulting
 * section has sh_addralign = 1. Measured on this project's own build
 * (build_tab5/esp32p4-mqjs.map): of the four existing _binary_*_start
 * symbols, three are not 8-byte aligned and one sits at an ODD address.
 * Worse, the placement is the running sum of every preceding rodata
 * contribution, so it moves whenever unrelated code changes size.
 *
 * skk_dict_open() rejects a misaligned base (SKK_ERR_ALIGN) because the
 * index is an array of uint64 read straight out of the image, so
 * EMBED_FILES would give a firmware that works today, has no dictionary
 * after the next unrelated commit, and reports no build error either way.
 *
 * .balign 64 (not 8) puts the image head on a cache line, which also
 * lands every internal section — they are 8-byte aligned from the image
 * base — at a fixed offset from a line boundary. Costs at most 63 bytes.
 *
 * The image is produced by tools/skk_prep.py and is optional: CMakeLists
 * only compiles this file when components/skk_core/skk_dict.bin exists,
 * and skk_builtin.c reports an empty blob otherwise.
 */
    .section .rodata.embedded.skk, "a", @progbits
    .balign 64
    .globl  skk_dict_image
    .type   skk_dict_image, @object
skk_dict_image:
    .incbin "skk_dict.bin"
    .globl  skk_dict_image_end
skk_dict_image_end:
    .size   skk_dict_image, . - skk_dict_image
