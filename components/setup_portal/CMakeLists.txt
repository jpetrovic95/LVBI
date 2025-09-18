idf_component_register(SRCS "src/setup_portal.c"
                       INCLUDE_DIRS "include"
                       REQUIRES  esp_http_server
				                 esp_wifi
				                 nvs_flash
				                 compact_config_db
								 data_handler
)