idf_component_register(
    SRCS "core/jn_dsp.c" "core/jn_patch.c" "core/jn_presets.c" "jn_mod.c"
    INCLUDE_DIRS "." "core"
    LDFRAGMENTS "linker.lf"
    REQUIRES tulip_studio lvgl json
    PRIV_REQUIRES freertos
)
