#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ECHOMATE_EXPRESSION_UNKNOWN = 0,
    ECHOMATE_EXPRESSION_HAPPY,
    ECHOMATE_EXPRESSION_NEUTRAL,
    ECHOMATE_EXPRESSION_SAD,
} echomate_expression_t;

typedef enum {
    ECHOMATE_EMOTION_HAPPY = 0,
    ECHOMATE_EMOTION_CALM,
    ECHOMATE_EMOTION_LONELY,
    ECHOMATE_EMOTION_ALERT,
    ECHOMATE_EMOTION_SLEEPY,
} echomate_emotion_t;

typedef enum {
    ECHOMATE_PIXEL_GRAY8 = 0,
    ECHOMATE_PIXEL_RGB565,
    ECHOMATE_PIXEL_RGB888,
} echomate_pixel_format_t;

typedef enum {
    ECHOMATE_CAMERA_BACKEND_STUB = 0,
    ECHOMATE_CAMERA_BACKEND_MIPI_CSI,
} echomate_camera_backend_t;

typedef enum {
    ECHOMATE_CAMERA_STATE_DISABLED = 0,
    ECHOMATE_CAMERA_STATE_INIT,
    ECHOMATE_CAMERA_STATE_OK,
    ECHOMATE_CAMERA_STATE_ERROR,
} echomate_camera_state_t;

typedef enum {
    ECHOMATE_BAYER_BGGR = 0,
    ECHOMATE_BAYER_GBRG,
    ECHOMATE_BAYER_GRBG,
    ECHOMATE_BAYER_RGGB,
} echomate_bayer_pattern_t;

typedef enum {
    ECHOMATE_COMMAND_NONE = 0,
    ECHOMATE_COMMAND_WAKE,
    ECHOMATE_COMMAND_DANCE,
    ECHOMATE_COMMAND_SLEEP,
} echomate_command_t;

typedef enum {
    ECHOMATE_GESTURE_NONE = 0,
    ECHOMATE_GESTURE_WAVE,
    ECHOMATE_GESTURE_STOP,
} echomate_gesture_t;

typedef enum {
    ECHOMATE_VOICE_NONE = 0,
    ECHOMATE_VOICE_GREETING,
    ECHOMATE_VOICE_CALMING,
    ECHOMATE_VOICE_COMPANION,
} echomate_voice_cue_t;

typedef enum {
    ECHOMATE_MOTION_NONE = 0,
    ECHOMATE_MOTION_NOD,
    ECHOMATE_MOTION_SWAY,
    ECHOMATE_MOTION_ALERT,
    ECHOMATE_MOTION_SLEEP,
} echomate_motion_cue_t;

typedef enum {
    ECHOMATE_REASON_NONE = 0,
    ECHOMATE_REASON_FACE = 1 << 0,
    ECHOMATE_REASON_SMILE = 1 << 1,
    ECHOMATE_REASON_LOUD = 1 << 2,
    ECHOMATE_REASON_WAKE = 1 << 3,
    ECHOMATE_REASON_IDLE = 1 << 4,
} echomate_reason_flags_t;

typedef enum {
    ECHOMATE_FACE_DETECTOR_STUB = 0,
    ECHOMATE_FACE_DETECTOR_HEURISTIC,
    ECHOMATE_FACE_DETECTOR_ESP_WHO,
    ECHOMATE_FACE_DETECTOR_ESP_DL,
} echomate_face_detector_backend_t;

typedef enum {
    ECHOMATE_EXPRESSION_BACKEND_STUB = 0,
    ECHOMATE_EXPRESSION_BACKEND_HEURISTIC,
    ECHOMATE_EXPRESSION_BACKEND_ESP_DL,
} echomate_expression_backend_t;

typedef struct {
    uint32_t frame_id;
    int64_t timestamp_us;
    uint16_t width;
    uint16_t height;
    echomate_pixel_format_t format;
    const uint8_t *buffer;
    size_t length;
} camera_frame_msg_t;

typedef struct {
    uint32_t frame_id;
    int64_t timestamp_us;
    uint16_t width;
    uint16_t height;
    echomate_pixel_format_t format;
    const uint8_t *buffer;
    size_t length;
    uint8_t mean_luma;
    uint8_t contrast;
} vision_input_frame_t;

typedef struct {
    bool detected;
    bool fresh;
    echomate_face_detector_backend_t backend;
    uint8_t confidence;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    echomate_expression_t expression;
} face_detection_result_t;

typedef struct {
    echomate_face_detector_backend_t requested_backend;
    echomate_face_detector_backend_t active_backend;
    bool esp_who_enabled;
    bool esp_dl_enabled;
    bool esp_who_headers_available;
    bool esp_dl_headers_available;
    bool real_backend_available;
    esp_err_t init_result;
    bool run_attempted;
    esp_err_t run_result;
    uint16_t run_fail_count;
    uint32_t detector_call_count;
    uint32_t run_call_count;
    uint32_t init_heap_before;
    uint32_t init_heap_after;
    const char *fallback_reason;
} face_detector_status_t;

typedef struct {
    uint32_t frame_count;
    uint32_t preprocess_ok_count;
    uint32_t preprocess_fail_count;
    uint32_t detector_ok_count;
    uint32_t detector_fail_count;
    uint32_t frame_fallback_count;
    uint32_t last_frame_id;
    esp_err_t last_preprocess_result;
    esp_err_t last_detector_result;
} vision_pipeline_status_t;

typedef struct {
    echomate_expression_backend_t requested_backend;
    echomate_expression_backend_t active_backend;
    bool heuristic_enabled;
    bool esp_dl_enabled;
    bool esp_dl_headers_available;
    bool esp_dl_model_embedded;
    bool real_backend_available;
    esp_err_t init_result;
    bool run_attempted;
    esp_err_t run_result;
    uint16_t run_fail_count;
    uint32_t classify_count;
    uint32_t happy_count;
    uint32_t neutral_count;
    uint32_t sad_count;
    uint32_t unknown_count;
    echomate_expression_t last_expression;
    echomate_expression_t raw_expression;
    uint8_t last_mean_luma;
    uint8_t last_contrast;
    uint8_t last_upper_luma;
    uint8_t last_lower_luma;
    uint8_t last_upper_contrast;
    uint8_t last_lower_contrast;
    uint8_t last_mouth_luma;
    uint8_t last_mouth_contrast;
    uint16_t model_input_width;
    uint16_t model_input_height;
    uint8_t model_input_channels;
    echomate_pixel_format_t model_input_format;
    uint32_t last_inference_us;
    uint8_t last_confidence;
    int16_t score_neutral;
    int16_t score_happy;
    int16_t score_sad;
    /* Full 7-class FER label from the ESP-DL backend (0..6, canonical order:
     * neutral/happy/sad/angry/surprise/fear/disgust). */
    uint8_t fer_label;
    uint8_t fer_confidence;
    const char *fallback_reason;
} expression_adapter_status_t;

typedef struct {
    int64_t timestamp_us;
    echomate_camera_backend_t backend;
    echomate_camera_state_t state;
    uint32_t frame_count;
    uint32_t dropped_frames;
    uint16_t width;
    uint16_t height;
    uint32_t fourcc;
    uint16_t fps_x10;
    echomate_bayer_pattern_t bayer_pattern;
    uint8_t raw_black_level;
    uint16_t awb_r_gain_q8;
    uint16_t awb_b_gain_q8;
    esp_err_t last_error;
} camera_status_msg_t;

typedef struct {
    uint32_t frame_id;
    int64_t timestamp_us;
    bool face_detected;
    echomate_face_detector_backend_t detector_backend;
    echomate_expression_t expression;
    uint8_t confidence;
    echomate_gesture_t gesture;
    uint8_t scene_luma;
    uint8_t scene_motion;
    uint16_t face_x;
    uint16_t face_y;
    uint16_t face_width;
    uint16_t face_height;
    uint16_t input_width;
    uint16_t input_height;
    uint32_t preprocess_us;
    uint32_t inference_us;
    uint16_t ai_fps_x10;
    uint32_t avg_preprocess_us;
    uint32_t avg_inference_us;
    uint32_t max_inference_us;
    uint16_t stale_frames_dropped;
} vision_result_msg_t;

typedef struct {
    int64_t timestamp_us;
    bool wake_word_detected;
    echomate_command_t command;
    uint16_t amplitude;
    uint16_t speaking_rate_wpm;
} audio_feature_msg_t;

typedef struct {
    int64_t timestamp_us;
    echomate_emotion_t emotion;
    uint8_t confidence;
    uint32_t reason_flags;
} emotion_state_msg_t;

typedef struct {
    int64_t timestamp_us;
    echomate_emotion_t emotion;
    echomate_voice_cue_t voice_cue;
    echomate_motion_cue_t motion_cue;
    char text[96];
} dialogue_msg_t;

typedef struct {
    int64_t timestamp_us;
    echomate_emotion_t emotion;
    const char *label;
} ui_msg_t;

typedef struct {
    int64_t timestamp_us;
    bool touched;
    uint8_t point_count;
    uint16_t x[5];
    uint16_t y[5];
    uint16_t strength[5];
} touch_sample_msg_t;

typedef struct {
    int64_t timestamp_us;
    echomate_motion_cue_t cue;
    uint8_t servo0_deg;
    uint8_t servo1_deg;
    uint16_t duration_ms;
} motion_cmd_msg_t;

typedef struct {
    int64_t timestamp_us;
    uint32_t ready_bits;
    bool touch_ready;
    bool lcd_ready;
    bool camera_driver_enabled;
    bool audio_driver_enabled;
    bool servo_enabled;
    bool wifi_enabled;
    bool ai_model_enabled;
    echomate_face_detector_backend_t ai_requested_backend;
    echomate_face_detector_backend_t ai_active_backend;
    bool ai_real_backend_available;
    bool ai_esp_who_headers_available;
    bool ai_esp_dl_headers_available;
    bool ai_wait_ui_ready;
    uint16_t ai_init_delay_ms;
    uint16_t ai_inference_interval;
    esp_err_t ai_init_result;
    bool ai_run_attempted;
    esp_err_t ai_run_result;
    uint16_t ai_run_fail_count;
    uint32_t ai_detector_call_count;
    uint32_t ai_run_call_count;
    uint32_t vision_frame_count;
    uint32_t vision_preprocess_ok_count;
    uint32_t vision_preprocess_fail_count;
    echomate_expression_backend_t expression_requested_backend;
    echomate_expression_backend_t expression_active_backend;
    bool expression_heuristic_enabled;
    bool expression_esp_dl_enabled;
    bool expression_esp_dl_headers_available;
    bool expression_esp_dl_model_embedded;
    bool expression_real_backend_available;
    esp_err_t expression_init_result;
    bool expression_run_attempted;
    esp_err_t expression_run_result;
    uint16_t expression_run_fail_count;
    uint32_t expression_classify_count;
    uint32_t expression_happy_count;
    uint32_t expression_neutral_count;
    uint32_t expression_sad_count;
    uint32_t expression_unknown_count;
    echomate_expression_t expression_last;
    echomate_expression_t expression_raw;
    uint8_t expression_mean_luma;
    uint8_t expression_contrast;
    uint8_t expression_upper_luma;
    uint8_t expression_lower_luma;
    uint8_t expression_upper_contrast;
    uint8_t expression_lower_contrast;
    uint8_t expression_mouth_luma;
    uint8_t expression_mouth_contrast;
    uint16_t expression_model_input_width;
    uint16_t expression_model_input_height;
    uint8_t expression_model_input_channels;
    echomate_pixel_format_t expression_model_input_format;
    uint32_t expression_last_inference_us;
    uint8_t expression_last_confidence;
    int16_t expression_score_neutral;
    int16_t expression_score_happy;
    int16_t expression_score_sad;
    uint32_t ai_init_heap_before;
    uint32_t ai_init_heap_after;
    const char *ai_fallback_reason;
    uint32_t free_heap;
    uint32_t min_free_heap;
} system_status_msg_t;
