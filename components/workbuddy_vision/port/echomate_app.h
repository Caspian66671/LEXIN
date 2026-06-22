#pragma once

#include "esp_err.h"
#include "echomate_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t camera_port_init(void);
esp_err_t camera_port_capture(camera_frame_msg_t *frame);
esp_err_t camera_port_get_status(camera_status_msg_t *status);

esp_err_t vision_input_prepare(const camera_frame_msg_t *frame,
                               vision_input_frame_t *input);
esp_err_t face_detector_adapter_init(void);
void face_detector_adapter_get_status(face_detector_status_t *status);
esp_err_t face_detector_adapter_run(const vision_input_frame_t *input,
                                    face_detection_result_t *face);
esp_err_t vision_model_init(void);
void vision_model_get_status(vision_pipeline_status_t *status);
esp_err_t vision_model_run(const camera_frame_msg_t *frame,
                           vision_result_msg_t *result);
esp_err_t emotion_engine_init(void);
esp_err_t emotion_engine_update(const vision_result_msg_t *vision,
                                const audio_feature_msg_t *audio,
                                emotion_state_msg_t *state);
const char *echomate_emotion_to_string(echomate_emotion_t emotion);

#ifdef __cplusplus
}
#endif
