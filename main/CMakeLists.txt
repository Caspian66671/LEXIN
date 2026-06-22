set(workbuddy_sources
    "workbuddy_main.c"
    "workbuddy_actions.c"
    "workbuddy_display_test.c"
    "workbuddy_interaction.c"
    "workbuddy_launcher.c"
    "workbuddy_edge_advisor.cpp"
    "workbuddy_cn_20.c"
    "workbuddy_cn_28.c")

set(workbuddy_priv_requires
    driver
    esp_event
    esp_http_client
    esp_hw_support
    esp_lcd
    esp_netif
    esp_wifi
    esp_lcd_ek79007
    esp_lcd_touch_gt911
    esp_lvgl_port
    lvgl
    lwip
    nvs_flash
    workbuddy_vision)

set(workbuddy_advisor_model "${CMAKE_CURRENT_LIST_DIR}/models/workbuddy_advisor.espdl")
set(workbuddy_has_espdl_model 0)

idf_build_get_property(component_targets __COMPONENT_TARGETS)
if(EXISTS "${workbuddy_advisor_model}")
    set(workbuddy_has_espdl_model 1)
    if("___idf_espressif__esp-dl" IN_LIST component_targets)
        idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
        list(APPEND workbuddy_priv_requires espressif__esp-dl)
    elseif("___idf_esp-dl" IN_LIST component_targets)
        idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
        list(APPEND workbuddy_priv_requires esp-dl)
    endif()
    set(cmake_dir "${espdl_dir}/fbs_loader/cmake")
    include("${cmake_dir}/utilities.cmake")
endif()

idf_component_register(SRCS ${workbuddy_sources}
    PRIV_REQUIRES ${workbuddy_priv_requires}
    INCLUDE_DIRS "")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WORKBUDDY_HAS_ESPDL_MODEL=${workbuddy_has_espdl_model})

if(workbuddy_has_espdl_model)
    target_add_aligned_binary_data(${COMPONENT_LIB} "${workbuddy_advisor_model}" BINARY)
endif()
