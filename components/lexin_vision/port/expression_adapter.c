#include "expression_adapter.h"

#include <inttypes.h>

#include "esp_err.h"
#include "esp_log.h"

#include "echomate_config.h"
#include "expression_adapter_esp_dl.h"

static const char *TAG = "expression_adapter";

static expression_adapter_status_t s_status = {
#if CONFIG_ECHOMATE_EXPRESSION_BACKEND_ESP_DL
    .requested_backend = ECHOMATE_EXPRESSION_BACKEND_ESP_DL,
#elif CONFIG_ECHOMATE_EXPRESSION_BACKEND_STUB
    .requested_backend = ECHOMATE_EXPRESSION_BACKEND_STUB,
#else
    .requested_backend = ECHOMATE_EXPRESSION_BACKEND_HEURISTIC,
#endif
#if CONFIG_ECHOMATE_EXPRESSION_BACKEND_HEURISTIC
    .active_backend = ECHOMATE_EXPRESSION_BACKEND_HEURISTIC,
#elif CONFIG_ECHOMATE_EXPRESSION_BACKEND_STUB
    .active_backend = ECHOMATE_EXPRESSION_BACKEND_STUB,
#else
    .active_backend = ECHOMATE_EXPRESSION_BACKEND_HEURISTIC,
#endif
    .heuristic_enabled = CONFIG_ECHOMATE_ENABLE_EXPRESSION_HEURISTIC,
    .esp_dl_enabled = CONFIG_ECHOMATE_ENABLE_ESP_DL,
    .init_result = ESP_ERR_INVALID_STATE,
    .run_result = ESP_ERR_INVALID_STATE,
    .last_expression = ECHOMATE_EXPRESSION_UNKNOWN,
    .model_input_width = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH,
    .model_input_height = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT,
#if CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_RGB888
    .model_input_channels = 3,
    .model_input_format = ECHOMATE_PIXEL_RGB888,
#else
    .model_input_channels = 1,
    .model_input_format = ECHOMATE_PIXEL_GRAY8,
#endif
};

static expression_esp_dl_status_t s_esp_dl_status = {
    .input_width = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH,
    .input_height = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT,
#if CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_RGB888
    .input_channels = 3,
    .input_format = ECHOMATE_PIXEL_RGB888,
#else
    .input_channels = 1,
    .input_format = ECHOMATE_PIXEL_GRAY8,
#endif
};

static echomate_expression_t s_stable_expression = ECHOMATE_EXPRESSION_UNKNOWN;
static echomate_expression_t s_candidate_expression = ECHOMATE_EXPRESSION_UNKNOWN;
static uint8_t s_candidate_count;
static uint8_t s_hold_count;
static uint32_t s_happy_candidate_log_count;

static const char *backend_label(echomate_expression_backend_t backend)
{
    switch (backend) {
    case ECHOMATE_EXPRESSION_BACKEND_ESP_DL:
        return "esp-dl";
    case ECHOMATE_EXPRESSION_BACKEND_HEURISTIC:
        return "heuristic";
    case ECHOMATE_EXPRESSION_BACKEND_STUB:
    default:
        return "stub";
    }
}

static const char *fallback_reason(void)
{
    if (s_status.requested_backend == s_status.active_backend) {
        return "none";
    }
    if (s_status.requested_backend == ECHOMATE_EXPRESSION_BACKEND_ESP_DL) {
        return s_esp_dl_status.fallback_reason ?
            s_esp_dl_status.fallback_reason : "esp-dl expression unavailable";
    }
    if (s_status.active_backend == ECHOMATE_EXPRESSION_BACKEND_HEURISTIC) {
        return "heuristic fallback";
    }
    return "expression backend unavailable";
}

static void sync_esp_dl_status(void)
{
    s_status.esp_dl_headers_available = s_esp_dl_status.headers_available;
    s_status.esp_dl_model_embedded = s_esp_dl_status.model_embedded;
    s_status.model_input_width = s_esp_dl_status.input_width;
    s_status.model_input_height = s_esp_dl_status.input_height;
    s_status.model_input_channels = s_esp_dl_status.input_channels;
    s_status.model_input_format = s_esp_dl_status.input_format;
    s_status.last_inference_us = s_esp_dl_status.inference_us;
    s_status.last_confidence = s_esp_dl_status.confidence;
    s_status.score_neutral = s_esp_dl_status.score_neutral;
    s_status.score_happy = s_esp_dl_status.score_happy;
    s_status.score_sad = s_esp_dl_status.score_sad;
}

static void record_expression(echomate_expression_t expression)
{
    if (s_status.classify_count < UINT32_MAX) {
        s_status.classify_count++;
    }
    s_status.last_expression = expression;

    switch (expression) {
    case ECHOMATE_EXPRESSION_HAPPY:
        if (s_status.happy_count < UINT32_MAX) {
            s_status.happy_count++;
        }
        break;
    case ECHOMATE_EXPRESSION_NEUTRAL:
        if (s_status.neutral_count < UINT32_MAX) {
            s_status.neutral_count++;
        }
        break;
    case ECHOMATE_EXPRESSION_SAD:
        if (s_status.sad_count < UINT32_MAX) {
            s_status.sad_count++;
        }
        break;
    case ECHOMATE_EXPRESSION_UNKNOWN:
    default:
        if (s_status.unknown_count < UINT32_MAX) {
            s_status.unknown_count++;
        }
        break;
    }
}

static void record_features(echomate_expression_t raw, const expression_features_t *features)
{
    s_status.raw_expression = raw;
    if (!features || !features->valid) {
        s_status.last_mean_luma = 0;
        s_status.last_contrast = 0;
        s_status.last_upper_luma = 0;
        s_status.last_lower_luma = 0;
        s_status.last_upper_contrast = 0;
        s_status.last_lower_contrast = 0;
        s_status.last_mouth_luma = 0;
        s_status.last_mouth_contrast = 0;
        return;
    }

    s_status.last_mean_luma = features->mean_luma;
    s_status.last_contrast = features->contrast;
    s_status.last_upper_luma = features->upper_luma;
    s_status.last_lower_luma = features->lower_luma;
    s_status.last_upper_contrast = features->upper_contrast;
    s_status.last_lower_contrast = features->lower_contrast;
    s_status.last_mouth_luma = features->mouth_luma;
    s_status.last_mouth_contrast = features->mouth_contrast;
}

static echomate_expression_t stabilize_expression(echomate_expression_t raw)
{
    if (raw == ECHOMATE_EXPRESSION_UNKNOWN) {
        s_stable_expression = ECHOMATE_EXPRESSION_UNKNOWN;
        s_candidate_expression = ECHOMATE_EXPRESSION_UNKNOWN;
        s_candidate_count = 0;
        s_hold_count = 0;
        return raw;
    }

    if (s_stable_expression == ECHOMATE_EXPRESSION_UNKNOWN) {
        s_stable_expression = ECHOMATE_EXPRESSION_NEUTRAL;
        s_candidate_expression = raw == ECHOMATE_EXPRESSION_NEUTRAL ?
            ECHOMATE_EXPRESSION_UNKNOWN : raw;
        s_candidate_count = raw == ECHOMATE_EXPRESSION_NEUTRAL ? 0 : 1;
        s_hold_count = 0;
        return s_stable_expression;
    }

    if (raw == s_stable_expression) {
        s_candidate_expression = ECHOMATE_EXPRESSION_UNKNOWN;
        s_candidate_count = 0;
        s_hold_count = 0;
        return s_stable_expression;
    }

    if (raw == ECHOMATE_EXPRESSION_NEUTRAL) {
#if CONFIG_ECHOMATE_EXPRESSION_HOLD_FRAMES > 0
        if (s_stable_expression != ECHOMATE_EXPRESSION_NEUTRAL &&
            s_hold_count < CONFIG_ECHOMATE_EXPRESSION_HOLD_FRAMES) {
            s_hold_count++;
            return s_stable_expression;
        }
#endif
        if (s_stable_expression != ECHOMATE_EXPRESSION_NEUTRAL &&
            s_hold_count + 1U < CONFIG_ECHOMATE_EXPRESSION_NEUTRAL_CONFIRM_FRAMES) {
            s_hold_count++;
            s_candidate_expression = ECHOMATE_EXPRESSION_NEUTRAL;
            s_candidate_count = 0;
            return s_stable_expression;
        }
        s_stable_expression = ECHOMATE_EXPRESSION_NEUTRAL;
        s_candidate_expression = ECHOMATE_EXPRESSION_UNKNOWN;
        s_candidate_count = 0;
        s_hold_count = 0;
        return s_stable_expression;
    }

    if (s_candidate_expression != raw) {
        s_candidate_expression = raw;
        s_candidate_count = 1;
        s_hold_count = 0;
    } else if (s_candidate_count < UINT8_MAX) {
        s_candidate_count++;
    }

    if (s_candidate_count >= CONFIG_ECHOMATE_EXPRESSION_NON_NEUTRAL_CONFIRM_FRAMES) {
        s_stable_expression = raw;
        s_candidate_expression = ECHOMATE_EXPRESSION_UNKNOWN;
        s_candidate_count = 0;
        s_hold_count = 0;
    }

    return s_stable_expression;
}

static echomate_expression_t finish_expression(echomate_expression_t raw,
                                               const expression_features_t *features)
{
    record_features(raw, features);
    const echomate_expression_t expression = stabilize_expression(raw);
    record_expression(expression);
    return expression;
}

static echomate_expression_t run_heuristic(const face_detection_result_t *face,
                                           const expression_features_t *features)
{
    echomate_expression_t raw = ECHOMATE_EXPRESSION_UNKNOWN;

#if CONFIG_ECHOMATE_ENABLE_EXPRESSION_HEURISTIC
    if (!face || !features || !face->detected || !features->valid) {
        s_status.run_result = ESP_ERR_INVALID_ARG;
        return finish_expression(raw, features);
    }

    const bool confident_face =
        face->confidence >= CONFIG_ECHOMATE_EXPRESSION_HAPPY_MIN_CONFIDENCE;
    const bool lower_face_dark =
        features->upper_luma >= features->lower_luma &&
        features->upper_luma - features->lower_luma >=
            CONFIG_ECHOMATE_EXPRESSION_HAPPY_LOWER_DARK_DELTA;
    const bool lower_face_bright =
        features->lower_luma >= features->upper_luma &&
        features->lower_luma - features->upper_luma >=
            CONFIG_ECHOMATE_EXPRESSION_HAPPY_LOWER_DARK_DELTA;
    const bool expressive_contrast =
        features->contrast >= CONFIG_ECHOMATE_EXPRESSION_HAPPY_MIN_CONTRAST;
    const bool lower_face_detail =
        features->lower_contrast >= CONFIG_ECHOMATE_EXPRESSION_HAPPY_MIN_LOWER_CONTRAST;
    const bool mouth_detail =
        features->mouth_contrast >= CONFIG_ECHOMATE_EXPRESSION_HAPPY_MIN_MOUTH_CONTRAST;
    const bool mouth_prominent =
        features->mouth_contrast >=
            features->upper_contrast + CONFIG_ECHOMATE_EXPRESSION_HAPPY_MOUTH_PROMINENCE &&
        features->mouth_contrast >=
            features->lower_contrast + CONFIG_ECHOMATE_EXPRESSION_HAPPY_MOUTH_OVER_LOWER;
    const bool broad_smile_signature =
        features->upper_contrast >= CONFIG_ECHOMATE_EXPRESSION_HAPPY_BROAD_MIN_UPPER_CONTRAST &&
        features->lower_contrast >= CONFIG_ECHOMATE_EXPRESSION_HAPPY_BROAD_MIN_LOWER_CONTRAST &&
        features->mouth_contrast >= CONFIG_ECHOMATE_EXPRESSION_HAPPY_BROAD_MIN_MOUTH_CONTRAST;
    const bool lower_face_changed =
        lower_face_dark || lower_face_bright ||
        features->lower_contrast >=
            CONFIG_ECHOMATE_EXPRESSION_HAPPY_MIN_LOWER_CONTRAST +
            CONFIG_ECHOMATE_EXPRESSION_HAPPY_LOWER_DARK_DELTA;

    if (confident_face && expressive_contrast && lower_face_detail &&
        ((mouth_detail && mouth_prominent && lower_face_changed) || broad_smile_signature)) {
        raw = ECHOMATE_EXPRESSION_HAPPY;
        s_happy_candidate_log_count++;
        if (s_happy_candidate_log_count <= 6U || (s_happy_candidate_log_count % 32U) == 0U) {
            ESP_LOGI(TAG,
                     "happy candidate count=%" PRIu32 " conf=%u luma=%u contrast=%u upper=%u lower=%u upper_contrast=%u lower_contrast=%u mouth_luma=%u mouth_contrast=%u",
                     s_happy_candidate_log_count,
                     face->confidence,
                     features->mean_luma,
                     features->contrast,
                     features->upper_luma,
                     features->lower_luma,
                     features->upper_contrast,
                     features->lower_contrast,
                     features->mouth_luma,
                     features->mouth_contrast);
        }
    } else if (features->mean_luma <= CONFIG_ECHOMATE_EXPRESSION_SAD_MAX_LUMA &&
               features->contrast <= CONFIG_ECHOMATE_EXPRESSION_SAD_MAX_CONTRAST) {
        raw = ECHOMATE_EXPRESSION_SAD;
    } else {
        raw = ECHOMATE_EXPRESSION_NEUTRAL;
    }
#else
    (void)face;
    (void)features;
    raw = ECHOMATE_EXPRESSION_NEUTRAL;
#endif

    s_status.run_result = ESP_OK;
    return finish_expression(raw, features);
}

esp_err_t expression_adapter_init(void)
{
    s_status.real_backend_available = false;
    s_status.init_result = ESP_OK;
    s_status.run_result = ESP_OK;

    (void)expression_esp_dl_init(&s_esp_dl_status);
    sync_esp_dl_status();

    if (s_status.requested_backend == ECHOMATE_EXPRESSION_BACKEND_ESP_DL) {
        s_status.init_result = expression_esp_dl_init(&s_esp_dl_status);
        sync_esp_dl_status();
        if (s_status.init_result == ESP_OK) {
            s_status.active_backend = ECHOMATE_EXPRESSION_BACKEND_ESP_DL;
            s_status.real_backend_available = true;
            s_status.fallback_reason = fallback_reason();
            ESP_LOGI(TAG, "requested backend=%s active=%s input=%ux%u c=%u headers=%d embedded=%d",
                     backend_label(s_status.requested_backend),
                     backend_label(s_status.active_backend),
                     s_status.model_input_width,
                     s_status.model_input_height,
                     s_status.model_input_channels,
                     s_status.esp_dl_headers_available,
                     s_status.esp_dl_model_embedded);
            return ESP_OK;
        }

#if CONFIG_ECHOMATE_ENABLE_EXPRESSION_HEURISTIC
        s_status.active_backend = ECHOMATE_EXPRESSION_BACKEND_HEURISTIC;
#else
        s_status.active_backend = ECHOMATE_EXPRESSION_BACKEND_STUB;
#endif
        s_status.real_backend_available = false;
        s_status.fallback_reason = fallback_reason();
        ESP_LOGW(TAG, "requested backend=%s active=%s init=%s reason=\"%s\" headers=%d embedded=%d",
                 backend_label(s_status.requested_backend),
                 backend_label(s_status.active_backend),
                 esp_err_to_name(s_status.init_result),
                 s_status.fallback_reason,
                 s_status.esp_dl_headers_available,
                 s_status.esp_dl_model_embedded);
        return ESP_OK;
    }

    s_status.fallback_reason = fallback_reason();
    ESP_LOGI(TAG, "active backend=%s heuristic=%d esp_dl=%d dl_headers=%d dl_model=%d",
             backend_label(s_status.active_backend),
             s_status.heuristic_enabled,
             s_status.esp_dl_enabled,
             s_status.esp_dl_headers_available,
             s_status.esp_dl_model_embedded);
    return ESP_OK;
}

echomate_expression_t expression_adapter_classify_from_input(const vision_input_frame_t *input,
                                                             const face_detection_result_t *face,
                                                             const expression_features_t *features)
{
    s_status.run_attempted = true;

    if (s_status.active_backend == ECHOMATE_EXPRESSION_BACKEND_STUB) {
        s_status.run_result = ESP_ERR_NOT_SUPPORTED;
        if (s_status.run_fail_count < UINT16_MAX) {
            s_status.run_fail_count++;
        }
        record_expression(ECHOMATE_EXPRESSION_UNKNOWN);
        return ECHOMATE_EXPRESSION_UNKNOWN;
    }

    if (s_status.active_backend == ECHOMATE_EXPRESSION_BACKEND_ESP_DL) {
        echomate_expression_t raw = ECHOMATE_EXPRESSION_UNKNOWN;
        esp_err_t dl_ret = expression_esp_dl_classify(input, face, &raw, &s_esp_dl_status);
        sync_esp_dl_status();
        s_status.run_result = dl_ret;
        if (dl_ret == ESP_OK) {
            return finish_expression(raw, features);
        }

        if (s_status.run_fail_count < UINT16_MAX) {
            s_status.run_fail_count++;
        }
#if CONFIG_ECHOMATE_ENABLE_EXPRESSION_HEURISTIC
        s_status.active_backend = ECHOMATE_EXPRESSION_BACKEND_HEURISTIC;
        s_status.real_backend_available = false;
        s_status.fallback_reason = fallback_reason();
        ESP_LOGW(TAG, "ESP-DL expression run failed (%s), falling back to heuristic: %s",
                 esp_err_to_name(dl_ret),
                 s_status.fallback_reason);
#else
        s_status.active_backend = ECHOMATE_EXPRESSION_BACKEND_STUB;
        s_status.fallback_reason = fallback_reason();
        return ECHOMATE_EXPRESSION_UNKNOWN;
#endif
    }

    return run_heuristic(face, features);
}

echomate_expression_t expression_adapter_classify(const face_detection_result_t *face,
                                                  const expression_features_t *features)
{
    return expression_adapter_classify_from_input(NULL, face, features);
}

void expression_adapter_get_status(expression_adapter_status_t *status)
{
    if (!status) {
        return;
    }
    *status = s_status;
}
