# Tab5 on-device UI. Sources are only compiled with CONFIG_MQJS_TAB5_UI=y;
# REQUIRES must stay unconditional (requirement expansion runs before
# kconfig), so Stamp builds still fetch+compile LVGL but link none of it —
# flash size is unaffected.
if(CONFIG_MQJS_TAB5_UI)
    set(UI_SRCS "ui_tab5.cpp" "ui_widgets.cpp" "fonts/font_noto_jp_20_4.c"
        # monospace terminal font (HackGen Console NF size17 bpp4 via
        # lv_font_conv; ASCII + box-drawing + Nerd Font BMP icons so an
        # SSH'd neovim/powerline/eza renders) — used by ui.cells, and
        # since S0 also the icon link of the ui_font fallback chain (it
        # is a strict superset of the deleted 20px font_nf_ui_20.c).
        # S4 added JIS X 0208 ku 1-7 + 16-47 (kana, punctuation, Greek,
        # Cyrillic, level-1 kanji): 3,747 -> 7,204 codepoints, glyph_bitmap
        # 366,124 -> 772,460 B. Regenerate with tools/font_term_mono.py
        # (it re-inserts the _Static_assert lv_font_conv does not emit and
        # refuses to install a font over the LVGL 1 MB bitmap_index cap).
        "fonts/font_term_mono.c"
        # ST712x panel drivers vendored from M5Tab5-UserDemo (Apache-2.0)
        "vendor/esp_lcd_st7123.c" "vendor/esp_lcd_st7121.c")
else()
    set(UI_SRCS "")
endif()

idf_component_register(SRCS ${UI_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "vendor"
                       PRIV_REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio
                                     esp_driver_ledc esp_driver_ppa esp_timer
                                     kbd_core mooncake smooth_ui_toolkit)

if(CONFIG_MQJS_TAB5_UI)
    # the lv_font_conv-generated font selects #include "lvgl.h" via this guard
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    # defer esp-hosted's constructor-time transport init (see ui_tab5.cpp)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_hosted_init")
    # Put esp_lvgl_port's PPA rotation scratch in PSRAM (see
    # __wrap_lvgl_port_ppa_create). The port sizes that scratch to the
    # draw buffer and gives both the same caps, so without this the only
    # way to enlarge the draw buffer is to pay for it twice in internal
    # DMA. lcd_ppa.h is private to the component, hence the include dir.
    idf_component_get_property(lvgl_port_dir espressif__esp_lvgl_port
                               COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
                               "${lvgl_port_dir}/src/common/ppa")
    target_link_options(${COMPONENT_LIB} INTERFACE
                        "-Wl,--wrap=lvgl_port_ppa_create")
endif()

# W1-1: back the LVGL builtin tlsf pool with PSRAM. lv_mem_init() (called
# from lv_init() inside lvgl_port_init, scheduler running, heap up) then
# does lv_tlsf_create_with_pool(LV_MEM_POOL_ALLOC(LV_MEM_SIZE), LV_MEM_SIZE)
# instead of reserving a LV_MEM_SIZE static array in .bss — without this
# define CONFIG_LV_MEM_SIZE_KILOBYTES=3072 would not even link (3MB .bss).
# Pool size lives in sdkconfig.defaults. PRIVATE on the lvgl lib is enough:
# the macros are only consumed by src/stdlib/builtin/lv_mem_core_builtin.c.
if(CONFIG_LV_USE_BUILTIN_MALLOC)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PRIVATE
        "LV_MEM_POOL_INCLUDE=\"esp_heap_caps.h\"")
    # NB: function-like macros are silently DROPPED by
    # target_compile_definitions — must go through compile options.
    target_compile_options(${lvgl_lib} PRIVATE
        "-DLV_MEM_POOL_ALLOC(size)=heap_caps_malloc(size,MALLOC_CAP_SPIRAM)")
endif()
