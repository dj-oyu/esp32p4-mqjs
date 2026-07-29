# SKK conversion engine shared by every app that wants Japanese input.
#
# Pure logic — no I/O, no LVGL, no allocation, no clock — so it builds
# unconditionally on any target and needs no REQUIRES at all. That is
# deliberate: nothing here may pull in an ESP-IDF header, because the
# same .c files are compiled by plain host gcc for the unit tests and
# are linked into tools/run_pc.
#
#   skk_kana.c   state machine, romaji->kana, preedit/commit rendering,
#                the UTF-8 helpers and the accessors
#   skk_dict.c   collation table + packed key, CRC32, image open,
#                binary search, index build/verify
#   skk_builtin.c  the one function that knows where the bytes live
#
# Keep the split: the order-defining code (skk_dict.c) is what both the
# prep tool and the search call, and that single source of truth is the
# whole point of the packed key (see skk_core.h).
set(SKK_SRCS "skk_kana.c" "skk_dict.c" "skk_builtin.c")

# The built-in dictionary image, embedded into flash rodata and read in
# place: zero allocation, zero load time (docs/skk-ime-design.md §6.6).
#
# Produce it with, e.g.
#   python tools/skk_prep.py build <SKK-JISYO.M> \
#          -o components/skk_core/skk_dict.bin
# It is intentionally NOT committed: it is generated data, its size
# depends on which dictionary you pick, and the source dictionaries
# carry their own licence terms. Without it the component still builds
# and skk.open() falls back to loading a file.
#
# NOT `EMBED_FILES`: that path emits no alignment directive at all
# (sh_addralign = 1, measured), and skk_dict_open() needs the image base
# 8-byte aligned to read the uint64 index in place. See the comment at
# the top of skk_dict_image.S — this is a real failure that appears and
# disappears with unrelated commits.
set(SKK_IMAGE "${CMAKE_CURRENT_LIST_DIR}/skk_dict.bin")
if(EXISTS "${SKK_IMAGE}")
    list(APPEND SKK_SRCS "skk_dict_image.S")
endif()

idf_component_register(SRCS ${SKK_SRCS}
                       INCLUDE_DIRS "include")

if(EXISTS "${SKK_IMAGE}")
    # .incbin resolves against the assembler's include path.
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SKK_HAVE_BUILTIN_IMAGE=1)
    # Re-link when the image is regenerated.
    set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/skk_dict_image.S"
                 APPEND PROPERTY OBJECT_DEPENDS "${SKK_IMAGE}")
    file(SIZE "${SKK_IMAGE}" SKK_IMAGE_BYTES)
    message(STATUS "skk_core: embedding ${SKK_IMAGE_BYTES} B dictionary image")
else()
    message(STATUS "skk_core: no skk_dict.bin — skk.open() will need a path")
endif()
