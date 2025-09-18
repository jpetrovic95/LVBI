# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif data_handler battery_reader display_handler telemetry_handler ina219 hwa_i2c compact_config_db error_defs oled_ssd1306 setup_portal lvbi_kv
)
