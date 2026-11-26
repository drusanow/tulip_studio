idf_component_register(
    SRCS
        "core/s1_model.c"
        "core/s1_seq.c"
        "s1_mod.c"
        "ui/s1_ui.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "core" "ui"
    REQUIRES tulip_studio lvgl json
)
