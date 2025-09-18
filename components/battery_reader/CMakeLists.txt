idf_component_register(
    SRCS "src/battery_reader.c"
    INCLUDE_DIRS "include"
    REQUIRES ina219 hwa_i2c error_defs display_handler data_handler
)

