idf_component_register(SRCS "src/lvbi_kv.c"
                    INCLUDE_DIRS "include"
                    REQUIRES     spiffs nvs_flash
                 				 
)