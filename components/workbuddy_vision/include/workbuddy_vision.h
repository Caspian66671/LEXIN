#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct {
    bool service_ready;
    bool camera_ready;
    bool face_detected;
    workbuddy_vision_expression_t expression;
    workbuddy_vision_backend_t backend;
    uint8_t confidence;
    uint32_t inference_ms;
    uint32_t frame_id;
    int64_t updated_at_ms;
    esp_err_t last_error;
} workbuddy_vision_snapshot_t;

typedef void (*workbuddy_vision_callback_t)(const workbuddy_vision_snapshot_t *snapshot,
                                            void *user_data);

esp_err_t workbuddy_vision_start(workbuddy_vision_callback_t callback, void *user_data);
void workbuddy_vision_get_snapshot(workbuddy_vision_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

