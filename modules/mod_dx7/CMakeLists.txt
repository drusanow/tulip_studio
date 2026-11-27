idf_component_register(
    SRCS "core/dx_dsp.c" "core/dx_voice.c" "core/dx_presets.c" "dx_mod.c"
    INCLUDE_DIRS "." "core"
    LDFRAGMENTS "linker.lf"
    REQUIRES tulip_studio lvgl json
    PRIV_REQUIRES freertos
)
