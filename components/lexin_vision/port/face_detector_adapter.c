#include <string.h>
#include <inttypes.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "echomate_app.h"
#include "echomate_config.h"
#include "face_detector_esp_who.h"

static const char *TAG = "face_detector";

static bool s_logged;
static bool s_esp_who_ready;
static esp_err_t s_esp_who_init_ret = ESP_ERR_NOT_SUPPORTED;
static esp_err_t s_esp_who_run_ret = ESP_ERR_INVALID_STATE;
static bool s_esp_who_run_attempted;
static uint16_t s_esp_who_run_fail_count;
static uint32_t s_detector_call_count;
static uint32_t s_esp_who_run_call_count;
static uint32_t s_init_heap_before;
static uint32_t s_init_heap_after;

#define ECHOMATE_ESP_DL_BACKEND_COMPILED  0

#if defined(__has_include)
#if __has_include("human_face_detect.hpp")
#define ECHOMATE_ESP_WHO_HEADERS_AVAILABLE 1
#else
#define ECHOMATE_ESP_WHO_HEADERS_AVAILABLE 0
#endif
#else
#define ECHOMATE_ESP_WHO_HEADERS_AVAILABLE 0
#endif

#if defined(__has_include)
#if __has_include("dl_model_base.hpp") || \
    __has_include("dl_image.hpp") || \
    __has_include("esp_dl.h")
#define ECHOMATE_ESP_DL_HEADERS_AVAILABLE 1
#else
#define ECHOMATE_ESP_DL_HEADERS_AVAILABLE 0
#endif
#else
#define ECHOMATE_ESP_DL_HEADERS_AVAILABLE 0
#endif

static echomate_face_detector_backend_t requested_backend(void)
{
#if CONFIG_ECHOMATE_FACE_DETECTOR_ESP_WHO
    return ECHOMATE_FACE_DETECTOR_ESP_WHO;
#elif CONFIG_ECHOMATE_FACE_DETECTOR_ESP_DL
    return ECHOMATE_FACE_DETECTOR_ESP_DL;
#else
    return ECHOMATE_FACE_DETECTOR_HEURISTIC;
#endif
}

static echomate_face_detector_backend_t active_backend(void)
{
    const echomate_face_detector_backend_t requested = requested_backend();

    if (requested == ECHOMATE_FACE_DETECTOR_ESP_WHO) {
        return s_esp_who_ready ? ECHOMATE_FACE_DETECTOR_ESP_WHO : ECHOMATE_FACE_DETECTOR_HEURISTIC;
    }

    if (requested == ECHOMATE_FACE_DETECTOR_ESP_DL) {
#if ECHOMATE_ESP_DL_HEADERS_AVAILABLE && ECHOMATE_ESP_DL_BACKEND_COMPILED
        return ECHOMATE_FACE_DETECTOR_ESP_DL;
#else
        return ECHOMATE_FACE_DETECTOR_HEURISTIC;
#endif
    }

    return requested;
}

static const char *fallback_reason(echomate_face_detector_backend_t requested,
                                   echomate_face_detector_backend_t active)
{
    if (requested == active) {
        return "none";
    }

    if (requested == ECHOMATE_FACE_DETECTOR_ESP_WHO) {
#if !ECHOMATE_ESP_WHO_HEADERS_AVAILABLE
        return "esp-who headers missing";
#else
        if (!face_detector_esp_who_compiled()) {
            return "esp-who runtime bridge not compiled";
        }
        if (s_esp_who_init_ret != ESP_OK) {
            return "esp-who init failed";
        }
        if (s_esp_who_run_attempted && s_esp_who_run_ret != ESP_OK) {
            return "esp-who run failed";
        }
        return "esp-who not ready";
#endif
    }

    if (requested == ECHOMATE_FACE_DETECTOR_ESP_DL) {
#if !ECHOMATE_ESP_DL_HEADERS_AVAILABLE
        return "esp-dl headers missing";
#elif !ECHOMATE_ESP_DL_BACKEND_COMPILED
        return "esp-dl runtime bridge not compiled";
#else
        return "esp-dl init unavailable";
#endif
    }

    return "fallback";
}

static const char *backend_label(echomate_face_detector_backend_t backend)
{
    switch (backend) {
    case ECHOMATE_FACE_DETECTOR_ESP_WHO:
        return "esp-who";
    case ECHOMATE_FACE_DETECTOR_ESP_DL:
        return "esp-dl";
    case ECHOMATE_FACE_DETECTOR_STUB:
        return "stub";
    case ECHOMATE_FACE_DETECTOR_HEURISTIC:
    default:
        return "heuristic";
    }
}

static uint8_t clamp_u8(uint32_t value)
{
    return value > 100U ? 100U : (uint8_t)value;
}

static size_t input_min_length(const vision_input_frame_t *input)
{
    const size_t pixels = (size_t)input->width * input->height;
    return input->format == ECHOMATE_PIXEL_RGB888 ? pixels * 3U : pixels;
}

static bool input_format_supported(echomate_pixel_format_t format)
{
    return format == ECHOMATE_PIXEL_GRAY8 || format == ECHOMATE_PIXEL_RGB888;
}

static const char *pixel_format_label(echomate_pixel_format_t format)
{
    switch (format) {
    case ECHOMATE_PIXEL_RGB888:
        return "RGB888";
    case ECHOMATE_PIXEL_RGB565:
        return "RGB565";
    case ECHOMATE_PIXEL_GRAY8:
    default:
        return "GRAY8";
    }
}

static uint8_t input_luma_at(const vision_input_frame_t *input, uint32_t x, uint32_t y)
{
    const size_t index = (size_t)y * input->width + x;
    if (input->format == ECHOMATE_PIXEL_RGB888) {
        const size_t offset = index * 3U;
        const uint8_t r = input->buffer[offset];
        const uint8_t g = input->buffer[offset + 1U];
        const uint8_t b = input->buffer[offset + 2U];
        return (uint8_t)(((uint16_t)77U * r + (uint16_t)150U * g + (uint16_t)29U * b) >> 8);
    }
    return input->buffer[index];
}

esp_err_t face_detector_adapter_init(void)
{
    const echomate_face_detector_backend_t requested = requested_backend();
    if (requested == ECHOMATE_FACE_DETECTOR_ESP_WHO) {
        s_init_heap_before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
        s_esp_who_init_ret = face_detector_esp_who_init();
        s_init_heap_after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
        s_esp_who_ready = s_esp_who_init_ret == ESP_OK && face_detector_esp_who_compiled();
        ESP_LOGI(TAG, "esp-who init result=%s heap_before=%" PRIu32 " heap_after=%" PRIu32 " heap_delta=%" PRId32,
                 esp_err_to_name(s_esp_who_init_ret),
                 s_init_heap_before,
                 s_init_heap_after,
                 (int32_t)s_init_heap_after - (int32_t)s_init_heap_before);
    }

    const echomate_face_detector_backend_t active = active_backend();

    if (requested != active) {
        ESP_LOGW(TAG, "requested backend=%s active=%s reason=\"%s\" esp_who_headers=%d esp_dl_headers=%d",
                 backend_label(requested),
                 backend_label(active),
                 fallback_reason(requested, active),
                 ECHOMATE_ESP_WHO_HEADERS_AVAILABLE,
                 ECHOMATE_ESP_DL_HEADERS_AVAILABLE);
    } else {
        ESP_LOGI(TAG, "active backend=%s esp_who_headers=%d esp_dl_headers=%d",
                 backend_label(active),
                 ECHOMATE_ESP_WHO_HEADERS_AVAILABLE,
                 ECHOMATE_ESP_DL_HEADERS_AVAILABLE);
    }
    return ESP_OK;
}

void face_detector_adapter_get_status(face_detector_status_t *status)
{
    if (!status) {
        return;
    }

    const echomate_face_detector_backend_t requested = requested_backend();
    const echomate_face_detector_backend_t active = active_backend();

    memset(status, 0, sizeof(*status));
    status->requested_backend = requested;
    status->active_backend = active;
    status->esp_who_enabled = CONFIG_ECHOMATE_ENABLE_ESP_WHO;
    status->esp_dl_enabled = CONFIG_ECHOMATE_ENABLE_ESP_DL;
    status->esp_who_headers_available = ECHOMATE_ESP_WHO_HEADERS_AVAILABLE;
    status->esp_dl_headers_available = ECHOMATE_ESP_DL_HEADERS_AVAILABLE;
    status->real_backend_available =
        (active == ECHOMATE_FACE_DETECTOR_ESP_WHO || active == ECHOMATE_FACE_DETECTOR_ESP_DL) &&
        active == requested;
    status->init_result = requested == ECHOMATE_FACE_DETECTOR_ESP_WHO ?
        s_esp_who_init_ret : ESP_OK;
    status->run_attempted = requested == ECHOMATE_FACE_DETECTOR_ESP_WHO &&
        s_esp_who_run_attempted;
    status->run_result = requested == ECHOMATE_FACE_DETECTOR_ESP_WHO ?
        s_esp_who_run_ret : ESP_OK;
    status->run_fail_count = requested == ECHOMATE_FACE_DETECTOR_ESP_WHO ?
        s_esp_who_run_fail_count : 0;
    status->detector_call_count = s_detector_call_count;
    status->run_call_count = requested == ECHOMATE_FACE_DETECTOR_ESP_WHO ?
        s_esp_who_run_call_count : 0;
    status->init_heap_before = s_init_heap_before;
    status->init_heap_after = s_init_heap_after;
    status->fallback_reason = fallback_reason(requested, active);
}

esp_err_t face_detector_adapter_run(const vision_input_frame_t *input, face_detection_result_t *face)
{
    if (!input || !face || !input->buffer ||
        !input_format_supported(input->format) ||
        input->length < input_min_length(input)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(face, 0, sizeof(*face));
    if (s_detector_call_count < UINT32_MAX) {
        s_detector_call_count++;
    }
    face->backend = active_backend();

    if (face->backend == ECHOMATE_FACE_DETECTOR_ESP_WHO) {
        bool ran_inference = false;
        esp_err_t who_ret = face_detector_esp_who_run(input, face, &ran_inference);
        if (ran_inference && s_esp_who_run_call_count < UINT32_MAX) {
            s_esp_who_run_call_count++;
        }
        s_esp_who_run_attempted = true;
        s_esp_who_run_ret = who_ret;
        if (who_ret == ESP_OK) {
            return ESP_OK;
        }

        if (s_esp_who_run_fail_count < UINT16_MAX) {
            s_esp_who_run_fail_count++;
        }
        s_esp_who_ready = false;
        static bool s_who_run_fallback_logged;
        if (!s_who_run_fallback_logged) {
            ESP_LOGW(TAG, "ESP-WHO run failed (%s), disabling real backend and falling back to heuristic",
                     esp_err_to_name(who_ret));
            s_who_run_fallback_logged = true;
        }
        memset(face, 0, sizeof(*face));
        face->backend = ECHOMATE_FACE_DETECTOR_HEURISTIC;
    } else if (requested_backend() == ECHOMATE_FACE_DETECTOR_ESP_WHO) {
        static bool s_who_not_active_logged;
        if (!s_who_not_active_logged) {
            ESP_LOGW(TAG, "ESP-WHO requested but not active during run: active=%s reason=\"%s\" init=%s",
                     backend_label(face->backend),
                     fallback_reason(ECHOMATE_FACE_DETECTOR_ESP_WHO, face->backend),
                     esp_err_to_name(s_esp_who_init_ret));
            s_who_not_active_logged = true;
        }
    }

    const uint32_t cx0 = input->width / 4U;
    const uint32_t cx1 = input->width - cx0;
    const uint32_t cy0 = input->height / 5U;
    const uint32_t cy1 = input->height - cy0;
    uint32_t center_sum = 0;
    uint32_t center_count = 0;
    uint8_t center_min = 255;
    uint8_t center_max = 0;
    uint32_t bright_count = 0;

    for (uint32_t y = cy0; y < cy1; y += 2U) {
        for (uint32_t x = cx0; x < cx1; x += 2U) {
            const uint8_t gray = input_luma_at(input, x, y);
            center_sum += gray;
            center_count++;
            if (gray < center_min) {
                center_min = gray;
            }
            if (gray > center_max) {
                center_max = gray;
            }
            if (gray > input->mean_luma + 10U) {
                bright_count++;
            }
        }
    }

    const uint32_t center_mean = center_count ? center_sum / center_count : input->mean_luma;
    const uint32_t center_contrast = center_max - center_min;
    const uint32_t bright_ratio_x100 = center_count ? (bright_count * 100U) / center_count : 0;
    const bool scene_ok = input->mean_luma > CONFIG_ECHOMATE_FACE_HEURISTIC_MIN_LUMA &&
        input->contrast > CONFIG_ECHOMATE_FACE_HEURISTIC_MIN_CONTRAST;
    const bool center_ok = center_mean + 8U >= input->mean_luma &&
        center_contrast > CONFIG_ECHOMATE_FACE_HEURISTIC_MIN_CENTER_CONTRAST;
    const bool texture_ok = bright_ratio_x100 > 8U && bright_ratio_x100 < 82U;

    face->fresh = true;
    if (scene_ok && center_ok && texture_ok) {
        face->detected = true;
        face->confidence = clamp_u8(42U + input->contrast / 3U + center_contrast / 4U);
        face->width = (uint16_t)((input->width * 46U) / 100U);
        face->height = (uint16_t)((input->height * 54U) / 100U);
        face->x = (uint16_t)((input->width - face->width) / 2U);
        face->y = (uint16_t)((input->height * 18U) / 100U);

        if (center_mean > 88U && center_contrast > 32U) {
            face->expression = ECHOMATE_EXPRESSION_HAPPY;
        } else if (center_mean < 58U) {
            face->expression = ECHOMATE_EXPRESSION_SAD;
        } else {
            face->expression = ECHOMATE_EXPRESSION_NEUTRAL;
        }
    } else {
        face->expression = ECHOMATE_EXPRESSION_UNKNOWN;
    }

    if (!s_logged) {
        ESP_LOGI(TAG, "face detector input=%ux%u %s requested=%s active=%s fallback=\"%s\" thresholds luma=%d contrast=%d center=%d",
                 input->width,
                 input->height,
                 pixel_format_label(input->format),
                 backend_label(requested_backend()),
                 backend_label(face->backend),
                 fallback_reason(requested_backend(), face->backend),
                 CONFIG_ECHOMATE_FACE_HEURISTIC_MIN_LUMA,
                 CONFIG_ECHOMATE_FACE_HEURISTIC_MIN_CONTRAST,
                 CONFIG_ECHOMATE_FACE_HEURISTIC_MIN_CENTER_CONTRAST);
        s_logged = true;
    }

    return ESP_OK;
}
