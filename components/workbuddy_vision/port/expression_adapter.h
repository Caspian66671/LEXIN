#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "echomate_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    uint8_t mean_luma;
    uint8_t contrast;
    uint8_t channel_spread;
    uint8_t upper_luma;
    uint8_t lower_luma;
    uint8_t upper_contrast;
    uint8_t lower_contrast;
    uint8_t mouth_luma;
    uint8_t mouth_contrast;
} expression_features_t;

esp_err_t expression_adapter_init(void);
echomate_expression_t expression_adapter_classify(const face_detection_result_t *face,
                                                  const expression_features_t *features);
echomate_expression_t expression_adapter_classify_from_input(const vision_input_frame_t *input,
                                                             const face_detection_result_t *face,
                                                             const expression_features_t *features);
void expression_adapter_get_status(expression_adapter_status_t *status);

#ifdef __cplusplus
}
#endif
