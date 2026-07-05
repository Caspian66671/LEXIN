set(vision_sources
    "lexin_vision.c"
    "port/camera_port.c"
    "port/vision_input.c"
    "port/vision_model.c"
    "port/emotion_engine.c"
    "port/face_detector_adapter.c"
    "port/face_detector_esp_who.cpp"
    "port/expression_adapter.c"
    "port/expression_adapter_esp_dl.cpp")

# Resolve the model path relative to this component instead of relying on
# PROJECT_DIR (which did not resolve here), mirroring how main/CMakeLists.txt
# locates lexin_advisor.espdl. components/lexin_vision/ -> ../../main/models/.
get_filename_component(expression_model
    "${CMAKE_CURRENT_LIST_DIR}/../../main/models/expression.espdl" ABSOLUTE)
set(expression_has_espdl_model 0)
set(expression_backend_definition CONFIG_ECHOMATE_EXPRESSION_BACKEND_HEURISTIC=1)
set(expression_espdl_definition CONFIG_ECHOMATE_ENABLE_ESP_DL=0)
set(expression_embedded_definition ECHOMATE_EXPRESSION_ESP_DL_MODEL_EMBEDDED=0)

if(EXISTS "${expression_model}")
    message(STATUS "lexin_vision: expression model FOUND -> ESP-DL backend (${expression_model})")
    set(expression_has_espdl_model 1)
    set(expression_backend_definition CONFIG_ECHOMATE_EXPRESSION_BACKEND_ESP_DL=1)
    set(expression_espdl_definition CONFIG_ECHOMATE_ENABLE_ESP_DL=1)
    set(expression_embedded_definition ECHOMATE_EXPRESSION_ESP_DL_MODEL_EMBEDDED=1)
else()
    message(STATUS "lexin_vision: expression model NOT FOUND at ${expression_model} -> heuristic backend")
endif()

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
    ${expression_backend_definition}
    CONFIG_ECHOMATE_VISION_MODEL_INPUT_RGB888=1
    # FER2013 emotion model: 48x48 single-channel input. Must match
    # tools/train_emotion_fer2013.py (IMG_SIZE) and the exported
    # main/models/expression.espdl tensor shape.
    CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH=48
    CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT=48
    CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_RGB888=0
    ${expression_espdl_definition}
    ${expression_embedded_definition})

if(expression_has_espdl_model)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
    set(cmake_dir "${espdl_dir}/fbs_loader/cmake")
    include("${cmake_dir}/utilities.cmake")
    target_add_aligned_binary_data(${COMPONENT_LIB}
        "${expression_model}"
        BINARY
        RENAME_TO "expression_espdl")
endif()
