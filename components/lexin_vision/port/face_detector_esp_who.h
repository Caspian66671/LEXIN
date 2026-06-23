#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "echomate_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool face_detector_esp_who_compiled(void);
esp_err_t face_detector_esp_who_init(void);
esp_err_t face_detector_esp_who_run(const vision_input_frame_t *input,
                                    face_detection_result_t *face,
                                    bool *ran_inference);

#ifdef __cplusplus
}
#endif
