#include "workbuddy_vision.h"

#include <string.h>

#include "echomate_app.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "workbuddy_vision";

#define VISION_TASK_STACK 8192
#define VISION_TASK_PRIORITY 4
#define VISION_START_DELAY_MS 2500
#define VISION_FRAME_PERIOD_MS 100

static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static workbuddy_vision_snapshot_t s_snapshot;
static workbuddy_vision_callback_t s_callback;
static void *s_callback_user_data;
static bool s_started;

static workbuddy_vision_expression_t map_expression(echomate_expression_t expression)
{
    switch (expression) {
    case ECHOMATE_EXPRESSION_HAPPY:
        return WORKBUDDY_VISION_EXPRESSION_HAPPY;
    case ECHOMATE_EXPRESSION_NEUTRAL:
        return WORKBUDDY_VISION_EXPRESSION_NEUTRAL;
    case ECHOMATE_EXPRESSION_SAD:
        return WORKBUDDY_VISION_EXPRESSION_SAD;
    case ECHOMATE_EXPRESSION_UNKNOWN:
    default:
        return WORKBUDDY_VISION_EXPRESSION_UNKNOWN;
    }
}

static workbuddy_vision_backend_t map_backend(echomate_face_detector_backend_t backend)
{
    switch (backend) {
    case ECHOMATE_FACE_DETECTOR_ESP_WHO:
        return WORKBUDDY_VISION_BACKEND_ESP_WHO;
    case ECHOMATE_FACE_DETECTOR_HEURISTIC:
        return WORKBUDDY_VISION_BACKEND_HEURISTIC;
    default:
        return WORKBUDDY_VISION_BACKEND_NONE;
    }
}

static void publish_snapshot(const workbuddy_vision_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = *snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);

    if (s_callback) {
        s_callback(snapshot, s_callback_user_data);
    }
}

static void vision_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(VISION_START_DELAY_MS));

    workbuddy_vision_snapshot_t snapshot = {0};
    snapshot.last_error = camera_port_init();
    camera_status_msg_t camera_status = {0};
    if (camera_port_get_status(&camera_status) == ESP_OK) {
        snapshot.camera_ready = camera_status.state == ECHOMATE_CAMERA_STATE_OK;
        if (!snapshot.camera_ready && camera_status.last_error != ESP_OK) {
            snapshot.last_error = camera_status.last_error;
        }
    }

    esp_err_t model_ret = vision_model_init();
    snapshot.service_ready = model_ret == ESP_OK;
    if (snapshot.last_error == ESP_OK && model_ret != ESP_OK) {
        snapshot.last_error = model_ret;
    }
    snapshot.updated_at_ms = esp_timer_get_time() / 1000;
    publish_snapshot(&snapshot);

    ESP_LOGI(TAG, "vision service started camera=%d model=%s",
             snapshot.camera_ready,
             esp_err_to_name(model_ret));

    while (true) {
        camera_frame_msg_t frame = {0};
        vision_result_msg_t result = {0};
        esp_err_t capture_ret = camera_port_capture(&frame);
        esp_err_t run_ret = capture_ret == ESP_OK ? vision_model_run(&frame, &result) : capture_ret;

        if (camera_port_get_status(&camera_status) == ESP_OK) {
            snapshot.camera_ready = camera_status.state == ECHOMATE_CAMERA_STATE_OK;
            if (!snapshot.camera_ready && camera_status.last_error != ESP_OK) {
                run_ret = camera_status.last_error;
            }
        }
        snapshot.service_ready = run_ret == ESP_OK;
        snapshot.last_error = run_ret;
        snapshot.updated_at_ms = esp_timer_get_time() / 1000;

        if (run_ret == ESP_OK && snapshot.camera_ready) {
            snapshot.face_detected = result.face_detected;
            snapshot.expression = map_expression(result.expression);
            snapshot.backend = map_backend(result.detector_backend);
            snapshot.confidence = result.confidence;
            snapshot.inference_ms = (result.inference_us + 999U) / 1000U;
            snapshot.frame_id = result.frame_id;
        } else {
            snapshot.face_detected = false;
            snapshot.expression = WORKBUDDY_VISION_EXPRESSION_UNKNOWN;
            snapshot.backend = WORKBUDDY_VISION_BACKEND_NONE;
            snapshot.confidence = 0;
            snapshot.inference_ms = 0;
        }

        publish_snapshot(&snapshot);
        vTaskDelay(pdMS_TO_TICKS(VISION_FRAME_PERIOD_MS));
    }
}

esp_err_t workbuddy_vision_start(workbuddy_vision_callback_t callback, void *user_data)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_callback = callback;
    s_callback_user_data = user_data;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.last_error = ESP_ERR_INVALID_STATE;

    BaseType_t created = xTaskCreate(vision_task,
                                     "workbuddy_vision",
                                     VISION_TASK_STACK,
                                     NULL,
                                     VISION_TASK_PRIORITY,
                                     NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

void workbuddy_vision_get_snapshot(workbuddy_vision_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    portENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
}
