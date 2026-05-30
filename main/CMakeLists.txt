idf_component_register(SRCS "workbuddy_main.c" "workbuddy_actions.c" "workbuddy_display_test.c" "workbuddy_launcher.c" "workbuddy_cn_20.c" "workbuddy_cn_28.c"
    PRIV_REQUIRES driver esp_event esp_http_client esp_hw_support esp_lcd esp_netif esp_wifi esp_lcd_ek79007 esp_lcd_touch_gt911 esp_lvgl_port lvgl lwip nvs_flash
    INCLUDE_DIRS "")
