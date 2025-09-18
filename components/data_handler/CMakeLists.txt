idf_component_register(
    SRCS "src/data_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES compact_config_db error_defs esp_wifi display_handler
)

