#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WORKBUDDY_VISION_PREVIEW_WIDTH 288
#define WORKBUDDY_VISION_PREVIEW_HEIGHT 216

typedef enum {
    WORKBUDDY_VISION_EXPRESSION_UNKNOWN = 0,
    WORKBUDDY_VISION_EXPRESSION_NEUTRAL,
    WORKBUDDY_VISION_EXPRESSION_HAPPY,
    WORKBUDDY_VISION_EXPRESSION_SAD,
} workbuddy_vision_expression_t;

typedef enum {
    WORKBUDDY_VISION_BACKEND_NONE = 0,
    WORKBUDDY_VISION_BACKEND_HEURISTIC,
    WORKBUDDY_VISION_BACKEND_ESP_WHO,
} workbuddy_vision_backend_t;

typedef enum {
    WORKBUDDY_VISION_EMOTION_HAPPY = 0,
    WORKBUDDY_VISION_EMOTION_CALM,
    WORKBUDDY_VISION_EMOTION_LONELY,
    WORKBUDDY_VISION_EMOTION_ALERT,
    WORKBUDDY_VISION_EMOTION_SLEEPY,
} workbuddy_vision_emotion_t;

typedef struct {
    bool service_ready;
    bool camera_ready;
    bool face_detected;
    workbuddy_vision_expression_t expression;
    workbuddy_vision_backend_t backend;
    workbuddy_vision_emotion_t emotion;
    uint8_t confidence;
    uint8_t emotion_confidence;
    uint32_t inference_ms;
    uint32_t frame_id;
    uint16_t camera_fps_x10;
    uint16_t face_x;
    uint16_t face_y;
    uint16_t face_width;
    uint16_t face_height;
    uint16_t input_width;
    uint16_t input_height;
    int64_t updated_at_ms;
    esp_err_t last_error;
    char response[96];
} workbuddy_vision_snapshot_t;

typedef void (*workbuddy_vision_callback_t)(const workbuddy_vision_snapshot_t *snapshot,
                                            void *user_data);

esp_err_t workbuddy_vision_start(workbuddy_vision_callback_t callback, void *user_data);
void workbuddy_vision_get_snapshot(workbuddy_vision_snapshot_t *snapshot);
esp_err_t workbuddy_vision_copy_preview(uint16_t *pixels,
                                        size_t pixel_count,
                                        uint32_t *frame_id);

#ifdef __cplusplus
}
#endif
