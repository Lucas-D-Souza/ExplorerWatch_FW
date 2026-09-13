idf_component_register(SRCS "main.cpp" 
                            "SdUsbManager.cpp"
                       INCLUDE_DIRS "."
                       EMBED_FILES "index.html"
                       REQUIRES bsp_board_extra lvgl ui_common nvs_flash esp_wifi esp_http_server json app_update)