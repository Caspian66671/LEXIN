#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LEXIN_VISION_PREVIEW_WIDTH 288
#define LEXIN_VISION_PREVIEW_HEIGHT 216

typedef enum {
    LEXIN_VISION_EXPRESSION_UNKNOWN = 0,
    LEXIN_VISION_EXPRESSION_NEUTRAL,
    LEXIN_VISION_EXPRESSION_HAPPY,
    LEXIN_VISION_EXPRESSION_SAD,
} lexin_vision_expression_t;

typedef enum {
    LEXIN_VISION_BACKEND_NONE = 0,
    LEXIN_VISION_BACKEND_HEURISTIC,
    LEXIN_VISION_BACKEND_ESP_WHO,
} lexin_vision_backend_t;

typedef enum {
    LEXIN_VISION_EMOTION_HAPPY = 0,
    LEXIN_VISION_EMOTION_CALM,
    LEXIN_VISION_EMOTION_LONELY,
    LEXIN_VISION_EMOTION_ALERT,
    LEXIN_VISION_EMOTION_SLEEPY,
} lexin_vision_emotion_t;

/* Product-facing moods, grouped from the neural model's 7 FER classes and
 * stabilised by temporal voting. AWAY = no face / user not at the desk. */
typedef enum {
    LEXIN_VISION_MOOD_FOCUSED = 0,  /* neutral */
    LEXIN_VISION_MOOD_HAPPY,        /* happy */
    LEXIN_VISION_MOOD_TIRED,        /* sad */
    LEXIN_VISION_MOOD_STRESSED,     /* angry + fear + disgust */
    LEXIN_VISION_MOOD_SURPRISED,    /* surprise */
    LEXIN_VISION_MOOD_AWAY,         /* no recent face */
} lexin_vision_mood_t;

typedef struct {
    bool service_ready;
    bool camera_ready;
    bool face_detected;
    lexin_vision_expression_t expression;
    lexin_vision_backend_t backend;
    lexin_vision_emotion_t emotion;
    lexin_vision_mood_t mood;
    uint8_t mood_confidence;
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
} lexin_vision_snapshot_t;

typedef void (*lexin_vision_callback_t)(const lexin_vision_snapshot_t *snapshot,
                                            void *user_data);

esp_err_t lexin_vision_start(lexin_vision_callback_t callback, void *user_data);
void lexin_vision_get_snapshot(lexin_vision_snapshot_t *snapshot);
esp_err_t lexin_vision_copy_preview(uint16_t *pixels,
                                        size_t pixel_count,
                                        uint32_t *frame_id);

#ifdef __cplusplus
}
#endif
