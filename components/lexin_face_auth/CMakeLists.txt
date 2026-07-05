idf_component_register(
    SRCS "lexin_face_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        esp_netif
        esp_timer
        esp_wifi
        freertos
        nvs_flash
        lexin_vision)
