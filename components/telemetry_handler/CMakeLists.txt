idf_component_register(
    SRCS "src/telemetry_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_wifi esp-tls mbedtls data_handler compact_config_db error_defs
)

