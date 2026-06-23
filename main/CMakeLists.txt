set(lexin_sources
    "lexin_main.c"
    "lexin_actions.c"
    "lexin_display_test.c"
    "lexin_interaction.c"
    "lexin_launcher.c"
    "lexin_edge_advisor.cpp"
    "lexin_cn_20.c"
    "lexin_cn_28.c")

set(lexin_priv_requires
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
    espressif__esp32_p4_function_ev_board
    lvgl
    lwip
    nvs_flash
    lexin_vision)

set(lexin_advisor_model "${CMAKE_CURRENT_LIST_DIR}/models/lexin_advisor.espdl")
set(lexin_has_espdl_model 0)

idf_build_get_property(component_targets __COMPONENT_TARGETS)
if(EXISTS "${lexin_advisor_model}")
    set(lexin_has_espdl_model 1)
    if("___idf_espressif__esp-dl" IN_LIST component_targets)
        idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
        list(APPEND lexin_priv_requires espressif__esp-dl)
    elseif("___idf_esp-dl" IN_LIST component_targets)
        idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
        list(APPEND lexin_priv_requires esp-dl)
    endif()
    set(cmake_dir "${espdl_dir}/fbs_loader/cmake")
    include("${cmake_dir}/utilities.cmake")
endif()

idf_component_register(SRCS ${lexin_sources}
    PRIV_REQUIRES ${lexin_priv_requires}
    INCLUDE_DIRS "")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LEXIN_HAS_ESPDL_MODEL=${lexin_has_espdl_model})

if(lexin_has_espdl_model)
    target_add_aligned_binary_data(${COMPONENT_LIB} "${lexin_advisor_model}" BINARY)
endif()
