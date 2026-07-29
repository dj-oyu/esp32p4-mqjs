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
# WHICH dictionary is a menuconfig choice (Kconfig in this directory).
# Two ways to satisfy it:
#
#   1. CONFIG_MQJS_SKK_DICT_SOURCE_DIR points at a directory holding the
#      SKK-JISYO.* files -> the image is regenerated here, by the same
#      tools/skk_prep.py that re-sorts into UTF-8 byte order and verifies
#      every entry before writing (the EUC-JP sort trap, §6.1).
#   2. It is empty -> skk_dict.bin is used exactly as it is. That is the
#      default, because the source dictionaries are large, carry their
#      own licence terms and are gitignored.
#
# Either way, with no image at all the component still builds and links;
# skk.open() then falls back to the `jisyo` partition or a file. The
# dictionary is deliberately NOT a build-time requirement.
#
# NOT `EMBED_FILES`: that path emits no alignment directive at all
# (sh_addralign = 1, measured), and skk_dict_open() needs the image base
# 8-byte aligned to read the uint64 index in place. See the comment at
# the top of skk_dict_image.S — this is a real failure that appears and
# disappears with unrelated commits.
set(SKK_IMAGE "${CMAKE_CURRENT_LIST_DIR}/skk_dict.bin")

if(CONFIG_MQJS_SKK_DICT_NONE)
    set(SKK_WANT "")
elseif(CONFIG_MQJS_SKK_DICT_S)
    set(SKK_WANT "S")
elseif(CONFIG_MQJS_SKK_DICT_ML)
    set(SKK_WANT "ML")
elseif(CONFIG_MQJS_SKK_DICT_L)
    set(SKK_WANT "L")
else()
    set(SKK_WANT "M")   # also the case when Kconfig is absent (host builds)
endif()

if(CONFIG_MQJS_SKK_DICT_NONE)
    # Selected "none": do not link an image even if skk_dict.bin exists,
    # or the choice would be silently ignored on a tree that once built
    # one.
    set(SKK_IMAGE "")
elseif(SKK_WANT AND CONFIG_MQJS_SKK_DICT_SOURCE_DIR AND
       NOT CONFIG_MQJS_SKK_DICT_SOURCE_DIR STREQUAL "")
    # Regenerate. Done at CONFIGURE time, not as a build rule: the image
    # is an input to the .S below, skk_prep.py verifies the whole thing
    # (a few seconds for M, ~30 s for L) and it must not re-run on every
    # incremental build. Reconfigure to pick up a changed choice — which
    # menuconfig does anyway.
    get_filename_component(SKK_SRC_DIR "${CONFIG_MQJS_SKK_DICT_SOURCE_DIR}"
                           ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(SKK_SRC_FILE "${SKK_SRC_DIR}/SKK-JISYO.${SKK_WANT}")
    if(NOT EXISTS "${SKK_SRC_FILE}")
        message(FATAL_ERROR
            "skk_core: CONFIG_MQJS_SKK_DICT_SOURCE_DIR is set but "
            "${SKK_SRC_FILE} is not there. Get it from "
            "https://github.com/skk-dev/dict , or clear the option to use "
            "components/skk_core/skk_dict.bin as it is.")
    endif()
    set(SKK_PREP_ARGS "")
    if(CONFIG_MQJS_SKK_DICT_STRIP_ANNOTATIONS)
        list(APPEND SKK_PREP_ARGS "--strip-annotations")
    endif()
    message(STATUS "skk_core: building the ${SKK_WANT} image from ${SKK_SRC_FILE}")
    idf_build_get_property(skk_python PYTHON)
    execute_process(
        COMMAND ${skk_python} "${CMAKE_SOURCE_DIR}/tools/skk_prep.py" build
                "${SKK_SRC_FILE}" -o "${SKK_IMAGE}" -q ${SKK_PREP_ARGS}
        RESULT_VARIABLE skk_prep_rc
        OUTPUT_VARIABLE skk_prep_out
        ERROR_VARIABLE  skk_prep_err)
    if(NOT skk_prep_rc EQUAL 0)
        # Loud on purpose: a half-written or unverified dictionary does
        # not crash, it answers wrong for some words (§6.1).
        message(FATAL_ERROR "skk_core: tools/skk_prep.py failed (${skk_prep_rc})\n"
                            "${skk_prep_out}${skk_prep_err}")
    endif()
    set(SKK_REGENERATED TRUE)
endif()

if(SKK_IMAGE AND EXISTS "${SKK_IMAGE}")
    list(APPEND SKK_SRCS "skk_dict_image.S")
endif()

idf_component_register(SRCS ${SKK_SRCS}
                       INCLUDE_DIRS "include")

if(SKK_IMAGE AND EXISTS "${SKK_IMAGE}")
    # .incbin resolves against the assembler's include path.
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SKK_HAVE_BUILTIN_IMAGE=1)
    # Re-link when the image is regenerated.
    set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/skk_dict_image.S"
                 APPEND PROPERTY OBJECT_DEPENDS "${SKK_IMAGE}")
    file(SIZE "${SKK_IMAGE}" SKK_IMAGE_BYTES)

    # Report what is ACTUALLY in the image, not what menuconfig asked
    # for. Without regeneration the two can differ — skk_dict.bin is
    # whatever was built last — and printing the choice next to a stale
    # file is how you end up debugging the wrong dictionary. The heading
    # count is nasi_count at offset 28 of skk_image_hdr_t (little endian,
    # skk_core.h).
    file(READ "${SKK_IMAGE}" SKK_HDR HEX OFFSET 28 LIMIT 4)
    string(REGEX MATCHALL ".." SKK_HDR_BYTES "${SKK_HDR}")
    list(REVERSE SKK_HDR_BYTES)
    string(JOIN "" SKK_HDR_HEX ${SKK_HDR_BYTES})
    math(EXPR SKK_IMAGE_NASI "0x${SKK_HDR_HEX}")

    if(SKK_REGENERATED)
        message(STATUS "skk_core: embedding the ${SKK_WANT} dictionary — "
                       "${SKK_IMAGE_BYTES} B, ${SKK_IMAGE_NASI} okuri-nasi headings")
    else()
        message(STATUS "skk_core: embedding skk_dict.bin as it stands — "
                       "${SKK_IMAGE_BYTES} B, ${SKK_IMAGE_NASI} okuri-nasi headings. "
                       "The menuconfig choice (${SKK_WANT}) only takes effect once "
                       "CONFIG_MQJS_SKK_DICT_SOURCE_DIR points at the SKK-JISYO files")
    endif()
elseif(CONFIG_MQJS_SKK_DICT_NONE)
    message(STATUS "skk_core: no built-in dictionary by choice — "
                   "skk.open() needs the `jisyo` partition or a path")
else()
    # Not fatal on purpose: a fresh clone has no skk_dict.bin and must
    # still build. Say what to do rather than failing the build.
    message(WARNING "skk_core: components/skk_core/skk_dict.bin is missing, so "
                    "this image has NO built-in dictionary. Either set "
                    "CONFIG_MQJS_SKK_DICT_SOURCE_DIR to a directory holding "
                    "SKK-JISYO.${SKK_WANT}, run tools/skk_prep.py yourself, or "
                    "flash the `jisyo` partition (README 3.5).")
endif()
