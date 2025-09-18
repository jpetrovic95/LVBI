idf_component_register(
    SRCS "src/oled_ssd1306.c"
    INCLUDE_DIRS "include"
    REQUIRES hwa_i2c error_defs
)
