#include "lexin_vision.h"

#include <string.h>

#include "echomate_app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lexin_vision";

#define VISION_TASK_STACK 8192
#define VISION_TASK_PRIORITY 4
#define VISION_START_DELAY_MS 2500
#define VISION_FRAME_PERIOD_MS (1000 / 30)
#define VISION_PREVIEW_PIXELS \
    (LEXIN_VISION_PREVIEW_WIDTH * LEXIN_VISION_PREVIEW_HEIGHT)

static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static lexin_vision_snapshot_t s_snapshot;
static lexin_vision_callback_t s_callback;
static void *s_callback_user_data;
static bool s_started;
static SemaphoreHandle_t s_preview_mutex;
static uint16_t *s_preview_pixels;
static uint32_t s_preview_frame_id;
static bool s_preview_valid;

static void update_preview(const camera_frame_msg_t *frame)
{
    if (!frame || !frame->buffer || frame->format != ECHOMATE_PIXEL_RGB565 ||
        frame->width != LEXIN_VISION_PREVIEW_WIDTH ||
        frame->height != LEXIN_VISION_PREVIEW_HEIGHT ||
        frame->length < VISION_PREVIEW_PIXELS * sizeof(uint16_t) ||
        !s_preview_mutex || !s_preview_pixels) {
        return;
    }

    if (xSemaphoreTake(s_preview_mutex, pdMS_TO_TICKS(40)) == pdTRUE) {
        memcpy(s_preview_pixels, frame->buffer,
               VISION_PREVIEW_PIXELS * sizeof(uint16_t));
        s_preview_frame_id = frame->frame_id;
        s_preview_valid = true;
        xSemaphoreGive(s_preview_mutex);
    }
}

static lexin_vision_expression_t map_expression(echomate_expression_t expression)
{
    switch (expression) {
    case ECHOMATE_EXPRESSION_HAPPY:
        return LEXIN_VISION_EXPRESSION_HAPPY;
    case ECHOMATE_EXPRESSION_NEUTRAL:
        return LEXIN_VISION_EXPRESSION_NEUTRAL;
    case ECHOMATE_EXPRESSION_SAD:
        return LEXIN_VISION_EXPRESSION_SAD;
    case ECHOMATE_EXPRESSION_UNKNOWN:
    default:
        return LEXIN_VISION_EXPRESSION_UNKNOWN;
    }
}

static lexin_vision_backend_t map_backend(echomate_face_detector_backend_t backend)
{
    switch (backend) {
    case ECHOMATE_FACE_DETECTOR_ESP_WHO:
        return LEXIN_VISION_BACKEND_ESP_WHO;
    case ECHOMATE_FACE_DETECTOR_HEURISTIC:
        return LEXIN_VISION_BACKEND_HEURISTIC;
    default:
        return LEXIN_VISION_BACKEND_NONE;
    }
}

static lexin_vision_emotion_t map_emotion(echomate_emotion_t emotion)
{
    switch (emotion) {
    case ECHOMATE_EMOTION_HAPPY:
        return LEXIN_VISION_EMOTION_HAPPY;
    case ECHOMATE_EMOTION_LONELY:
        return LEXIN_VISION_EMOTION_LONELY;
    case ECHOMATE_EMOTION_ALERT:
        return LEXIN_VISION_EMOTION_ALERT;
    case ECHOMATE_EMOTION_SLEEPY:
        return LEXIN_VISION_EMOTION_SLEEPY;
    case ECHOMATE_EMOTION_CALM:
    default:
        return LEXIN_VISION_EMOTION_CALM;
    }
}

static const char *emotion_response(echomate_emotion_t emotion)
{
    switch (emotion) {
    case ECHOMATE_EMOTION_HAPPY:
        return "I SEE YOU. THAT SMILE LOOKS BRIGHT.";
    case ECHOMATE_EMOTION_ALERT:
        return "THAT SOUNDED LOUD. I AM PAYING ATTENTION.";
    case ECHOMATE_EMOTION_LONELY:
        return "I AM STILL HERE WHEN YOU WANT TO TALK.";
    case ECHOMATE_EMOTION_SLEEPY:
        return "RESTING QUIETLY.";
    case ECHOMATE_EMOTION_CALM:
    default:
        return "CALM AND LISTENING";
    }
}

static void publish_snapshot(const lexin_vision_snapshot_t *snapshot)
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

    lexin_vision_snapshot_t snapshot = {0};
    snapshot.emotion = LEXIN_VISION_EMOTION_CALM;
    snapshot.emotion_confidence = 68;
    strlcpy(snapshot.response, "CALM AND LISTENING", sizeof(snapshot.response));
    snapshot.last_error = camera_port_init();
    camera_status_msg_t camera_status = {0};
    if (camera_port_get_status(&camera_status) == ESP_OK) {
        snapshot.camera_ready = camera_status.state == ECHOMATE_CAMERA_STATE_OK;
        if (!snapshot.camera_ready && camera_status.last_error != ESP_OK) {
            snapshot.last_error = camera_status.last_error;
        }
    }

    esp_err_t model_ret = vision_model_init();
    esp_err_t emotion_ret = emotion_engine_init();
    snapshot.service_ready = model_ret == ESP_OK;
    if (snapshot.last_error == ESP_OK && model_ret != ESP_OK) {
        snapshot.last_error = model_ret;
    }
    snapshot.updated_at_ms = esp_timer_get_time() / 1000;
    publish_snapshot(&snapshot);

    ESP_LOGI(TAG, "vision service started camera=%d model=%s",
             snapshot.camera_ready,
             esp_err_to_name(model_ret));

    TickType_t last_wake = xTaskGetTickCount();
    audio_feature_msg_t audio = {0};

    while (true) {
        camera_frame_msg_t frame = {0};
        vision_result_msg_t result = {0};
        esp_err_t run_ret = ESP_ERR_INVALID_STATE;
        if (camera_port_get_status(&camera_status) == ESP_OK) {
            snapshot.camera_ready = camera_status.state == ECHOMATE_CAMERA_STATE_OK;
            if (snapshot.camera_ready) {
                esp_err_t capture_ret = camera_port_capture(&frame);
                run_ret = capture_ret == ESP_OK ? vision_model_run(&frame, &result) : capture_ret;
                if (capture_ret == ESP_OK) {
                    update_preview(&frame);
                }
            } else if (camera_status.last_error != ESP_OK) {
                run_ret = camera_status.last_error;
            }
        }
        snapshot.camera_fps_x10 = camera_status.fps_x10;
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
            snapshot.face_x = result.face_x;
            snapshot.face_y = result.face_y;
            snapshot.face_width = result.face_width;
            snapshot.face_height = result.face_height;
            snapshot.input_width = result.input_width;
            snapshot.input_height = result.input_height;

            emotion_state_msg_t emotion_state = {0};
            if (emotion_ret == ESP_OK &&
                emotion_engine_update(&result, &audio, &emotion_state) == ESP_OK) {
                snapshot.emotion = map_emotion(emotion_state.emotion);
                snapshot.emotion_confidence = emotion_state.confidence;
                strlcpy(snapshot.response, emotion_response(emotion_state.emotion),
                        sizeof(snapshot.response));
            }
        } else {
            snapshot.face_detected = false;
            snapshot.expression = LEXIN_VISION_EXPRESSION_UNKNOWN;
            snapshot.backend = LEXIN_VISION_BACKEND_NONE;
            snapshot.confidence = 0;
            snapshot.inference_ms = 0;
            snapshot.face_x = 0;
            snapshot.face_y = 0;
            snapshot.face_width = 0;
            snapshot.face_height = 0;
            snapshot.input_width = 0;
            snapshot.input_height = 0;
        }

        publish_snapshot(&snapshot);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(VISION_FRAME_PERIOD_MS));
    }
}

esp_err_t lexin_vision_start(lexin_vision_callback_t callback, void *user_data)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_callback = callback;
    s_callback_user_data = user_data;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.emotion = LEXIN_VISION_EMOTION_CALM;
    s_snapshot.emotion_confidence = 68;
    strlcpy(s_snapshot.response, "CALM AND LISTENING", sizeof(s_snapshot.response));
    s_snapshot.last_error = ESP_ERR_INVALID_STATE;

    s_preview_mutex = xSemaphoreCreateMutex();
    s_preview_pixels = heap_caps_malloc(VISION_PREVIEW_PIXELS * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_preview_pixels) {
        s_preview_pixels = heap_caps_malloc(VISION_PREVIEW_PIXELS * sizeof(uint16_t),
                                            MALLOC_CAP_8BIT);
    }
    if (!s_preview_mutex || !s_preview_pixels) {
        if (s_preview_mutex) {
            vSemaphoreDelete(s_preview_mutex);
            s_preview_mutex = NULL;
        }
        if (s_preview_pixels) {
            heap_caps_free(s_preview_pixels);
            s_preview_pixels = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreate(vision_task,
                                     "lexin_vision",
                                     VISION_TASK_STACK,
                                     NULL,
                                     VISION_TASK_PRIORITY,
                                     NULL);
    if (created != pdPASS) {
        vSemaphoreDelete(s_preview_mutex);
        s_preview_mutex = NULL;
        heap_caps_free(s_preview_pixels);
        s_preview_pixels = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

void lexin_vision_get_snapshot(lexin_vision_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    portENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

esp_err_t lexin_vision_copy_preview(uint16_t *pixels,
                                        size_t pixel_count,
                                        uint32_t *frame_id)
{
    if (!pixels || pixel_count < VISION_PREVIEW_PIXELS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_preview_mutex || !s_preview_pixels) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_preview_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_preview_valid) {
        xSemaphoreGive(s_preview_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(pixels, s_preview_pixels, VISION_PREVIEW_PIXELS * sizeof(uint16_t));
    if (frame_id) {
        *frame_id = s_preview_frame_id;
    }
    xSemaphoreGive(s_preview_mutex);
    return ESP_OK;
}
