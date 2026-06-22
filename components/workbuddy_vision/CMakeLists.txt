set(vision_sources
    "workbuddy_vision.c"
    "port/camera_port.c"
    "port/vision_input.c"
    "port/vision_model.c"
    "port/emotion_engine.c"
    "port/face_detector_adapter.c"
    "port/face_detector_esp_who.cpp"
    "port/expression_adapter.c"
    "port/expression_adapter_esp_dl.cpp")

idf_component_register(
    SRCS ${vision_sources}
    INCLUDE_DIRS "include" "port"
    REQUIRES
        esp_timer
        esp_system
        esp_common
        esp_hw_support
        esp_driver_cam
        espressif__esp32_p4_function_ev_board
        human_face_detect
        espressif__esp-dl)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_ECHOMATE_ENABLE_CAMERA_DRIVER=1
    CONFIG_ECHOMATE_ENABLE_ESP_WHO=1
    CONFIG_ECHOMATE_FACE_DETECTOR_ESP_WHO=1
    CONFIG_ECHOMATE_ENABLE_EXPRESSION_HEURISTIC=1
    CONFIG_ECHOMATE_EXPRESSION_BACKEND_HEURISTIC=1
    CONFIG_ECHOMATE_VISION_MODEL_INPUT_RGB888=1
    CONFIG_ECHOMATE_ENABLE_ESP_DL=0
    ECHOMATE_EXPRESSION_ESP_DL_MODEL_EMBEDDED=0)
