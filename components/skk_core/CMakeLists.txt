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
set(SKK_JISYO_IMAGE "${CMAKE_CURRENT_LIST_DIR}/skk_jisyo.bin")

# Whether an image EXISTS decides what gets compiled and flashed, and
# that is decided at configure time — so CMake has to re-run when one
# appears or disappears.
#
# The files themselves cover the DELETE direction: without them, removing
# skk_dict.bin from a tree configured with it leaves ninja chasing a
# missing dependency ("needed by skk_dict_image.S.obj, missing and no
# known rule to make it") instead of taking the no-dictionary path.
#
# The DIRECTORY covers the create direction, which the files cannot:
# CMake drops paths that do not exist at configure time, so an image that
# was absent and then appears would otherwise be ignored until the next
# reconfigure — and the symptom is a firmware quietly built without a
# dictionary. A directory's mtime changes when a file is created or
# removed in it and NOT when one is edited, so this does not re-run on
# every source edit.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${SKK_IMAGE}" "${SKK_JISYO_IMAGE}" "${CMAKE_CURRENT_LIST_DIR}")

# Build <out> from SKK-JISYO.<want>, or leave it alone when no source
# directory is configured. Sets <done_var> to TRUE when it really ran, so
# the caller can tell "this is the dictionary you asked for" from "this
# is whatever was there".
#
# At CONFIGURE time, not as a build rule: the embedded image is an input
# to the .S below, and skk_prep.py verifies the whole thing (seconds for
# M, ~30 s for L) — it must not re-run on every incremental build.
# menuconfig reconfigures, which is when a changed choice should act.
function(skk_make_image want out max_size done_var)
    set(${done_var} FALSE PARENT_SCOPE)
    if(NOT want OR NOT CONFIG_MQJS_SKK_DICT_SOURCE_DIR OR
       CONFIG_MQJS_SKK_DICT_SOURCE_DIR STREQUAL "")
        return()
    endif()
    get_filename_component(src_dir "${CONFIG_MQJS_SKK_DICT_SOURCE_DIR}"
                           ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(src "${src_dir}/SKK-JISYO.${want}")
    if(NOT EXISTS "${src}")
        message(FATAL_ERROR
            "skk_core: CONFIG_MQJS_SKK_DICT_SOURCE_DIR is set but ${src} is "
            "not there. Get it from https://github.com/skk-dev/dict , or "
            "clear the option to use the prepared .bin files as they are.")
    endif()
    set(prep_args "")
    if(CONFIG_MQJS_SKK_DICT_STRIP_ANNOTATIONS)
        list(APPEND prep_args "--strip-annotations")
    endif()
    if(max_size)
        list(APPEND prep_args "--max-size" "${max_size}")
    endif()
    message(STATUS "skk_core: building the ${want} image from ${src}")
    idf_build_get_property(py PYTHON)
    execute_process(
        COMMAND ${py} "${CMAKE_SOURCE_DIR}/tools/skk_prep.py" build
                "${src}" -o "${out}" -q ${prep_args}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out_txt ERROR_VARIABLE err_txt)
    if(NOT rc EQUAL 0)
        # Loud on purpose: a half-written or unverified dictionary does
        # not crash, it answers wrong for some words (§6.1).
        message(FATAL_ERROR "skk_core: tools/skk_prep.py failed (${rc})\n"
                            "${out_txt}${err_txt}")
    endif()
    set(${done_var} TRUE PARENT_SCOPE)
endfunction()

# Read nasi_count out of an image header (offset 28, little endian —
# skk_image_hdr_t). Reporting what the file ACTUALLY holds rather than
# what menuconfig asked for: without regeneration the two differ, and
# printing the choice next to a stale file is how you end up debugging
# the wrong dictionary.
function(skk_image_headings path out_var)
    file(READ "${path}" hdr HEX OFFSET 28 LIMIT 4)
    string(REGEX MATCHALL ".." bytes "${hdr}")
    list(REVERSE bytes)
    string(JOIN "" hex ${bytes})
    math(EXPR n "0x${hex}")
    set(${out_var} "${n}" PARENT_SCOPE)
endfunction()

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
else()
    skk_make_image("${SKK_WANT}" "${SKK_IMAGE}" "" SKK_REGENERATED)
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

    skk_image_headings("${SKK_IMAGE}" SKK_IMAGE_NASI)
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

# ------------------------------------------------------------------
# The `jisyo` partition — where a big dictionary actually belongs.
#
# Same bytes, same mmap, same search speed as the embedded copy, but it
# does not compete with the code for the 6 MB app partition, it can hold
# pine (8.07 MB) which the app partition never could, and replacing it
# does not mean rebuilding and re-flashing the firmware.
#
# Registered with esptool_py_flash_to_partition so plain `idf.py flash`
# delivers it — the offset comes from the partition table rather than
# from a number written down twice. `idf.py app-flash` does not touch it
# and the contents survive, so the day-to-day loop is unaffected.

if(CONFIG_MQJS_SKK_JISYO_M)
    set(SKK_JISYO_WANT "M")
elseif(CONFIG_MQJS_SKK_JISYO_L)
    set(SKK_JISYO_WANT "L")
elseif(CONFIG_MQJS_SKK_JISYO_NONE)
    set(SKK_JISYO_WANT "")
else()
    set(SKK_JISYO_WANT "ML")
endif()

if(SKK_JISYO_WANT)
    # --max-size is the partition's, so an oversized dictionary is refused
    # here rather than truncated at the boundary at flash time — and a
    # truncated image is not obviously broken, it opens and searches and
    # answers wrong past the cut.
    partition_table_get_partition_info(SKK_JISYO_SIZE
                                       "--partition-name jisyo" "size")
    skk_make_image("${SKK_JISYO_WANT}" "${SKK_JISYO_IMAGE}"
                   "${SKK_JISYO_SIZE}" SKK_JISYO_REGENERATED)
endif()

if(SKK_JISYO_WANT AND EXISTS "${SKK_JISYO_IMAGE}")
    file(SIZE "${SKK_JISYO_IMAGE}" SKK_JISYO_BYTES)
    skk_image_headings("${SKK_JISYO_IMAGE}" SKK_JISYO_NASI)
    esptool_py_flash_to_partition(flash "jisyo" "${SKK_JISYO_IMAGE}")
    if(SKK_JISYO_REGENERATED)
        message(STATUS "skk_core: `idf.py flash` will write the "
                       "${SKK_JISYO_WANT} dictionary to `jisyo` — "
                       "${SKK_JISYO_BYTES} B, ${SKK_JISYO_NASI} okuri-nasi headings")
    else()
        message(STATUS "skk_core: `idf.py flash` will write skk_jisyo.bin to "
                       "`jisyo` as it stands — ${SKK_JISYO_BYTES} B, "
                       "${SKK_JISYO_NASI} okuri-nasi headings. The menuconfig "
                       "choice (${SKK_JISYO_WANT}) only takes effect once "
                       "CONFIG_MQJS_SKK_DICT_SOURCE_DIR is set")
    endif()
elseif(SKK_JISYO_WANT)
    # Not fatal: a fresh clone has neither the source dictionaries nor a
    # prepared image, and must still build and flash.
    message(STATUS "skk_core: no skk_jisyo.bin, so `jisyo` is left alone. "
                   "Set CONFIG_MQJS_SKK_DICT_SOURCE_DIR to a directory holding "
                   "SKK-JISYO.${SKK_JISYO_WANT} and the build will prepare and "
                   "flash it (README 3.5)")
endif()
