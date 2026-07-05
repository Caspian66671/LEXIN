#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "echomate_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FER2013 canonical order; indices 0/1/2 stay neutral/happy/sad so the
 * legacy three-class plumbing keeps working. Keep in sync with
 * tools/train_emotion_fer2013.py CANONICAL_LABELS. */
#define EXPRESSION_FER_CLASS_COUNT 7

typedef enum {
    EXPRESSION_FER_NEUTRAL = 0,
    EXPRESSION_FER_HAPPY,
    EXPRESSION_FER_SAD,
    EXPRESSION_FER_ANGRY,
    EXPRESSION_FER_SURPRISE,
    EXPRESSION_FER_FEAR,
    EXPRESSION_FER_DISGUST,
} expression_fer_label_t;

typedef struct {
    bool headers_available;
    bool model_embedded;
    uint16_t input_width;
    uint16_t input_height;
    uint8_t input_channels;
    echomate_pixel_format_t input_format;
    uint32_t inference_us;
    uint8_t confidence;
    int16_t score_neutral;
    int16_t score_happy;
    int16_t score_sad;
    /* Full FER2013 result (valid when the model exposes >= 3 outputs). */
    uint8_t class_count;            /* number of model output classes used */
    expression_fer_label_t fer_label;
    uint8_t fer_confidence;
    int16_t fer_scores[EXPRESSION_FER_CLASS_COUNT];
    const char *fer_label_name;
    const char *fallback_reason;
} expression_esp_dl_status_t;

const char *expression_fer_label_name(expression_fer_label_t label);

esp_err_t expression_esp_dl_init(expression_esp_dl_status_t *status);
esp_err_t expression_esp_dl_classify(const vision_input_frame_t *input,
                                     const face_detection_result_t *face,
                                     echomate_expression_t *expression,
                                     expression_esp_dl_status_t *status);

#ifdef __cplusplus
}
#endif
