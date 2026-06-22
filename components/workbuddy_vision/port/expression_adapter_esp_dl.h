#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "echomate_types.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    const char *fallback_reason;
} expression_esp_dl_status_t;

esp_err_t expression_esp_dl_init(expression_esp_dl_status_t *status);
esp_err_t expression_esp_dl_classify(const vision_input_frame_t *input,
                                     const face_detection_result_t *face,
                                     echomate_expression_t *expression,
                                     expression_esp_dl_status_t *status);

#ifdef __cplusplus
}
#endif
