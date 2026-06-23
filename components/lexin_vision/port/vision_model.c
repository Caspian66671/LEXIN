#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "echomate_app.h"
#include "echomate_config.h"
#include "expression_adapter.h"

static const char *TAG = "vision_model";

#define VISION_GRID_W     24
#define VISION_GRID_H     24
#define VISION_GRID_SIZE  (VISION_GRID_W * VISION_GRID_H)

static uint8_t s_prev_luma[VISION_GRID_SIZE];
static bool s_prev_luma_valid;
static bool s_frame_stats_logged;
static face_detection_result_t s_tracked_face;
static uint8_t s_face_hit_count;
static uint8_t s_face_miss_count;
#if CONFIG_ECHOMATE_EXPRESSION_SAMPLE_LOG_ENABLE
static int64_t s_last_expression_sample_log_us;
static echomate_expression_t s_last_expression_sample = ECHOMATE_EXPRESSION_UNKNOWN;
static uint32_t s_expression_sample_count;
#endif
static vision_pipeline_status_t s_status = {
    .last_preprocess_result = ESP_ERR_INVALID_STATE,
    .last_detector_result = ESP_ERR_INVALID_STATE,
};

static uint8_t clamp_u8_from_u32(uint32_t value)
{
    return value > 255U ? 255U : (uint8_t)value;
}

static uint32_t clamp_u32_from_i64(int64_t value)
{
    if (value <= 0) {
        return 0;
    }
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint8_t rgb565_luma(uint16_t pixel, uint8_t *red, uint8_t *blue)
{
    const uint8_t r = (uint8_t)(((pixel >> 11) & 0x1f) * 255U / 31U);
    const uint8_t g = (uint8_t)(((pixel >> 5) & 0x3f) * 255U / 63U);
    const uint8_t b = (uint8_t)((pixel & 0x1f) * 255U / 31U);

    if (red) {
        *red = r;
    }
    if (blue) {
        *blue = b;
    }

    return (uint8_t)(((uint16_t)77U * r + (uint16_t)150U * g + (uint16_t)29U * b) >> 8);
}

static size_t vision_input_min_length(const vision_input_frame_t *input)
{
    const size_t pixels = (size_t)input->width * input->height;
    return input->format == ECHOMATE_PIXEL_RGB888 ? pixels * 3U : pixels;
}

static bool vision_input_format_supported(echomate_pixel_format_t format)
{
    return format == ECHOMATE_PIXEL_GRAY8 || format == ECHOMATE_PIXEL_RGB888;
}

static uint8_t vision_input_luma_at(const vision_input_frame_t *input, uint32_t x, uint32_t y)
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

#if CONFIG_ECHOMATE_EXPRESSION_SAMPLE_LOG_ENABLE
static const char *expression_name(echomate_expression_t expression)
{
    switch (expression) {
    case ECHOMATE_EXPRESSION_HAPPY:
        return "HAPPY";
    case ECHOMATE_EXPRESSION_NEUTRAL:
        return "NEUTRAL";
    case ECHOMATE_EXPRESSION_SAD:
        return "SAD";
    case ECHOMATE_EXPRESSION_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static void expression_sample_log(const vision_input_frame_t *input,
                                  const face_detection_result_t *raw_face,
                                  const face_detection_result_t *smoothed_face,
                                  uint32_t inference_us)
{
    if (!input || !raw_face || !smoothed_face || !input->buffer ||
        !raw_face->fresh || !smoothed_face->detected ||
        smoothed_face->width == 0 || smoothed_face->height == 0) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    const int64_t period_us =
        (int64_t)CONFIG_ECHOMATE_EXPRESSION_SAMPLE_LOG_PERIOD_MS * 1000;
    const bool expression_changed = smoothed_face->expression != s_last_expression_sample;
    if (!expression_changed && now - s_last_expression_sample_log_us < period_us) {
        return;
    }

    const uint32_t x0 = smoothed_face->x;
    const uint32_t y0 = smoothed_face->y;
    const uint32_t x1 = x0 + smoothed_face->width > input->width ?
        input->width : x0 + smoothed_face->width;
    const uint32_t y1 = y0 + smoothed_face->height > input->height ?
        input->height : y0 + smoothed_face->height;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    enum {
        SAMPLE_GRID = CONFIG_ECHOMATE_EXPRESSION_SAMPLE_GRID_SIZE,
        HEX_CHARS_PER_SAMPLE = 2,
        SAMPLE_HEX_LEN = SAMPLE_GRID * SAMPLE_GRID * HEX_CHARS_PER_SAMPLE,
    };
    static const char hex[] = "0123456789ABCDEF";
    char signature[SAMPLE_HEX_LEN + 1];
    size_t out = 0;
    for (uint32_t gy = 0; gy < SAMPLE_GRID; ++gy) {
        const uint32_t y = y0 + (gy * (y1 - y0)) / SAMPLE_GRID;
        for (uint32_t gx = 0; gx < SAMPLE_GRID; ++gx) {
            const uint32_t x = x0 + (gx * (x1 - x0)) / SAMPLE_GRID;
            const uint8_t luma = vision_input_luma_at(input, x, y);
            signature[out++] = hex[luma >> 4];
            signature[out++] = hex[luma & 0x0f];
        }
    }
    signature[out] = '\0';

    expression_adapter_status_t expression_status = {0};
    expression_adapter_get_status(&expression_status);
    s_expression_sample_count++;
    s_last_expression_sample_log_us = now;
    s_last_expression_sample = smoothed_face->expression;

    ESP_LOGI(TAG,
             "expr_sample idx=%" PRIu32 " frame=%" PRIu32 " backend=%d raw=%s stable=%s conf=%u bbox=%u,%u,%u,%u input=%ux%u inf=%" PRIu32 "us M=%u U=%u L=%u mean=%u contrast=%u dl_conf=%u score=%d,%d,%d grid=%ux%u hex=%s",
             s_expression_sample_count,
             input->frame_id,
             smoothed_face->backend,
             expression_name(raw_face->expression),
             expression_name(smoothed_face->expression),
             smoothed_face->confidence,
             smoothed_face->x,
             smoothed_face->y,
             smoothed_face->width,
             smoothed_face->height,
             input->width,
             input->height,
             expression_status.last_inference_us ? expression_status.last_inference_us : inference_us,
             expression_status.last_mouth_contrast,
             expression_status.last_upper_contrast,
             expression_status.last_lower_contrast,
             expression_status.last_mean_luma,
             expression_status.last_contrast,
             expression_status.last_confidence,
             expression_status.score_neutral,
             expression_status.score_happy,
             expression_status.score_sad,
             SAMPLE_GRID,
             SAMPLE_GRID,
             signature);
}
#else
static void expression_sample_log(const vision_input_frame_t *input,
                                  const face_detection_result_t *raw_face,
                                  const face_detection_result_t *smoothed_face,
                                  uint32_t inference_us)
{
    (void)input;
    (void)raw_face;
    (void)smoothed_face;
    (void)inference_us;
}
#endif

static uint16_t smooth_u16(uint16_t previous, uint16_t current)
{
    const uint32_t alpha = CONFIG_ECHOMATE_VISION_FACE_SMOOTH_Q8;
    const uint32_t keep = 256U - alpha;
    return (uint16_t)(((uint32_t)previous * keep + (uint32_t)current * alpha + 128U) >> 8);
}

static uint8_t smooth_u8(uint8_t previous, uint8_t current)
{
    return (uint8_t)smooth_u16(previous, current);
}

static void smooth_face_result(const face_detection_result_t *raw, face_detection_result_t *smoothed)
{
    if (!raw || !smoothed) {
        return;
    }

    if (raw->detected) {
        if (!raw->fresh) {
            if (s_tracked_face.detected) {
                *smoothed = s_tracked_face;
                return;
            }
            *smoothed = *raw;
            smoothed->detected = false;
            smoothed->confidence = 0;
            return;
        }

        if (s_face_hit_count < UINT8_MAX) {
            s_face_hit_count++;
        }
        s_face_miss_count = 0;

        if (!s_tracked_face.detected ||
            s_face_hit_count < CONFIG_ECHOMATE_VISION_FACE_CONFIRM_FRAMES) {
            s_tracked_face = *raw;
            s_tracked_face.detected =
                s_face_hit_count >= CONFIG_ECHOMATE_VISION_FACE_CONFIRM_FRAMES;
        } else {
            s_tracked_face.detected = true;
            s_tracked_face.backend = raw->backend;
            s_tracked_face.confidence = smooth_u8(s_tracked_face.confidence, raw->confidence);
            s_tracked_face.x = smooth_u16(s_tracked_face.x, raw->x);
            s_tracked_face.y = smooth_u16(s_tracked_face.y, raw->y);
            s_tracked_face.width = smooth_u16(s_tracked_face.width, raw->width);
            s_tracked_face.height = smooth_u16(s_tracked_face.height, raw->height);
            if (raw->expression != ECHOMATE_EXPRESSION_UNKNOWN || raw->confidence > 70U) {
                s_tracked_face.expression = raw->expression;
            }
        }

        *smoothed = s_tracked_face;
        return;
    }

    if (!raw->fresh) {
        if (s_tracked_face.detected) {
            *smoothed = s_tracked_face;
            return;
        }
        *smoothed = *raw;
        return;
    }

    s_face_hit_count = 0;
    if (s_tracked_face.detected && s_face_miss_count < CONFIG_ECHOMATE_VISION_FACE_HOLD_FRAMES) {
        s_face_miss_count++;
        if (s_tracked_face.confidence > 12U) {
            s_tracked_face.confidence -= 12U;
        } else {
            s_tracked_face.confidence = 1U;
        }
        *smoothed = s_tracked_face;
        return;
    }

    s_face_miss_count = 0;
    s_tracked_face = *raw;
    *smoothed = *raw;
}

static esp_err_t vision_model_run_input_stats(const vision_input_frame_t *input,
                                              vision_result_msg_t *result)
{
    if (!input || !result ||
        !vision_input_format_supported(input->format) ||
        !input->buffer ||
        input->length < vision_input_min_length(input)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t diff_sum = 0;
    uint32_t sample_count = 0;

    for (uint32_t gy = 0; gy < VISION_GRID_H; ++gy) {
        const uint32_t y = (gy * input->height) / VISION_GRID_H;
        for (uint32_t gx = 0; gx < VISION_GRID_W; ++gx) {
            const uint32_t x = (gx * input->width) / VISION_GRID_W;
            const size_t grid_index = (size_t)gy * VISION_GRID_W + gx;
            const uint8_t luma = vision_input_luma_at(input, x, y);

            if (s_prev_luma_valid) {
                diff_sum += luma > s_prev_luma[grid_index] ? luma - s_prev_luma[grid_index]
                                                            : s_prev_luma[grid_index] - luma;
            }
            s_prev_luma[grid_index] = luma;
            sample_count++;
        }
    }

    const uint32_t motion = s_prev_luma_valid && sample_count ? diff_sum / sample_count : 0;
    s_prev_luma_valid = true;

    face_detection_result_t face = {0};
    esp_err_t face_ret = face_detector_adapter_run(input, &face);
    s_status.last_detector_result = face_ret;
    if (face_ret != ESP_OK) {
        if (s_status.detector_fail_count < UINT32_MAX) {
            s_status.detector_fail_count++;
        }
        return face_ret;
    }
    if (s_status.detector_ok_count < UINT32_MAX) {
        s_status.detector_ok_count++;
    }

    face_detection_result_t smoothed_face = {0};
    smooth_face_result(&face, &smoothed_face);

    result->face_detected = smoothed_face.detected;
    result->detector_backend = smoothed_face.backend;
    result->scene_luma = input->mean_luma;
    result->scene_motion = clamp_u8_from_u32(motion * 4U);
    result->gesture = motion > 18U ? ECHOMATE_GESTURE_WAVE : ECHOMATE_GESTURE_NONE;
    result->confidence = smoothed_face.confidence;
    result->expression = smoothed_face.expression;
    result->face_x = smoothed_face.x;
    result->face_y = smoothed_face.y;
    result->face_width = smoothed_face.width;
    result->face_height = smoothed_face.height;
    result->input_width = input->width;
    result->input_height = input->height;

    expression_sample_log(input, &face, &smoothed_face, result->inference_us);

    if (!s_frame_stats_logged) {
        ESP_LOGI(TAG, "using lightweight %s vision path with face detector adapter",
                 input->format == ECHOMATE_PIXEL_RGB888 ? "RGB888" : "GRAY8");
        s_frame_stats_logged = true;
    }

    return ESP_OK;
}

static esp_err_t vision_model_run_frame_stats(const camera_frame_msg_t *frame,
                                              vision_result_msg_t *result)
{
    if (frame->format != ECHOMATE_PIXEL_RGB565 ||
        !frame->buffer ||
        frame->length < (size_t)frame->width * frame->height * sizeof(uint16_t)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint16_t *pixels = (const uint16_t *)frame->buffer;
    uint32_t luma_sum = 0;
    uint32_t red_sum = 0;
    uint32_t blue_sum = 0;
    uint32_t diff_sum = 0;
    uint8_t luma_min = 255;
    uint8_t luma_max = 0;
    uint32_t sample_count = 0;

    for (uint32_t gy = 0; gy < VISION_GRID_H; ++gy) {
        const uint32_t y = (gy * frame->height) / VISION_GRID_H;
        for (uint32_t gx = 0; gx < VISION_GRID_W; ++gx) {
            const uint32_t x = (gx * frame->width) / VISION_GRID_W;
            const size_t index = (size_t)y * frame->width + x;
            const size_t grid_index = (size_t)gy * VISION_GRID_W + gx;
            uint8_t red = 0;
            uint8_t blue = 0;
            const uint8_t luma = rgb565_luma(pixels[index], &red, &blue);

            luma_sum += luma;
            red_sum += red;
            blue_sum += blue;
            if (luma < luma_min) {
                luma_min = luma;
            }
            if (luma > luma_max) {
                luma_max = luma;
            }

            if (s_prev_luma_valid) {
                diff_sum += luma > s_prev_luma[grid_index] ? luma - s_prev_luma[grid_index]
                                                            : s_prev_luma[grid_index] - luma;
            }
            s_prev_luma[grid_index] = luma;
            sample_count++;
        }
    }

    s_prev_luma_valid = true;

    const uint32_t avg_luma = sample_count ? luma_sum / sample_count : 0;
    const uint32_t avg_red = sample_count ? red_sum / sample_count : 0;
    const uint32_t avg_blue = sample_count ? blue_sum / sample_count : 0;
    const uint32_t contrast = luma_max - luma_min;
    const uint32_t motion = s_prev_luma_valid && sample_count ? diff_sum / sample_count : 0;
    const bool usable_scene = avg_luma > 24U && contrast > 10U;
    const bool warm_scene = avg_red > avg_blue + 8U;
    const bool cool_or_dark_scene = avg_luma < 58U || avg_blue > avg_red + 10U;

    result->face_detected = usable_scene;
    result->scene_luma = clamp_u8_from_u32(avg_luma);
    result->scene_motion = clamp_u8_from_u32(motion * 4U);
    result->gesture = motion > 18U ? ECHOMATE_GESTURE_WAVE : ECHOMATE_GESTURE_NONE;
    result->confidence = usable_scene ? clamp_u8_from_u32(48U + contrast / 4U) : 0;
    if (result->confidence > 100U) {
        result->confidence = 100U;
    }

    if (!usable_scene) {
        result->expression = ECHOMATE_EXPRESSION_UNKNOWN;
    } else if (warm_scene && avg_luma > 76U) {
        result->expression = ECHOMATE_EXPRESSION_HAPPY;
    } else if (cool_or_dark_scene) {
        result->expression = ECHOMATE_EXPRESSION_SAD;
    } else {
        result->expression = ECHOMATE_EXPRESSION_NEUTRAL;
    }

    if (!s_frame_stats_logged) {
        ESP_LOGI(TAG, "using lightweight RGB565 frame statistics vision path");
        s_frame_stats_logged = true;
    }

    return ESP_OK;
}

esp_err_t vision_model_init(void)
{
    ESP_ERROR_CHECK(expression_adapter_init());

    esp_err_t ret = face_detector_adapter_init();
    if (ret != ESP_OK) {
        return ret;
    }

    face_detector_status_t detector = {0};
    face_detector_adapter_get_status(&detector);

#if CONFIG_ECHOMATE_ENABLE_ESP_WHO || CONFIG_ECHOMATE_ENABLE_ESP_DL
    if (detector.real_backend_available) {
        ESP_LOGI(TAG, "real vision backend active");
    } else {
        ESP_LOGW(TAG, "real vision backend unavailable, using fallback: %s",
                 detector.fallback_reason ? detector.fallback_reason : "unknown");
    }
#else
    ESP_LOGI(TAG, "using lightweight local vision adapter");
#endif
    ESP_LOGI(TAG, "face smoothing confirm=%d hold=%d smooth_q8=%d",
             CONFIG_ECHOMATE_VISION_FACE_CONFIRM_FRAMES,
             CONFIG_ECHOMATE_VISION_FACE_HOLD_FRAMES,
             CONFIG_ECHOMATE_VISION_FACE_SMOOTH_Q8);
#if CONFIG_ECHOMATE_VISION_MODEL_INPUT_RGB888
    ESP_LOGI(TAG, "vision model input format=RGB888");
#else
    ESP_LOGI(TAG, "vision model input format=GRAY8");
#endif
    return ESP_OK;
}

void vision_model_get_status(vision_pipeline_status_t *status)
{
    if (!status) {
        return;
    }
    *status = s_status;
}

esp_err_t vision_model_run(const camera_frame_msg_t *frame, vision_result_msg_t *result)
{
    if (!frame || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->frame_id = frame->frame_id;
    result->timestamp_us = esp_timer_get_time();
    if (s_status.frame_count < UINT32_MAX) {
        s_status.frame_count++;
    }
    s_status.last_frame_id = frame->frame_id;

    vision_input_frame_t input = {0};
    const int64_t preprocess_start_us = esp_timer_get_time();
    esp_err_t prepare_ret = vision_input_prepare(frame, &input);
    s_status.last_preprocess_result = prepare_ret;
    const int64_t inference_start_us = esp_timer_get_time();
    if (prepare_ret == ESP_OK) {
        if (s_status.preprocess_ok_count < UINT32_MAX) {
            s_status.preprocess_ok_count++;
        }
        esp_err_t run_ret = vision_model_run_input_stats(&input, result);
        const int64_t done_us = esp_timer_get_time();
        result->preprocess_us = clamp_u32_from_i64(inference_start_us - preprocess_start_us);
        result->inference_us = clamp_u32_from_i64(done_us - inference_start_us);
        if (run_ret == ESP_OK) {
            return ESP_OK;
        }
    } else if (s_status.preprocess_fail_count < UINT32_MAX) {
        s_status.preprocess_fail_count++;
    }

    if (vision_model_run_frame_stats(frame, result) == ESP_OK) {
        if (s_status.frame_fallback_count < UINT32_MAX) {
            s_status.frame_fallback_count++;
        }
        return ESP_OK;
    }

    const uint32_t phase = frame->frame_id % 600U;

    result->face_detected = phase < 240U;
    result->gesture = (phase > 150U && phase < 165U) ? ECHOMATE_GESTURE_WAVE : ECHOMATE_GESTURE_NONE;
    result->confidence = result->face_detected ? 82 : 0;
    result->scene_luma = 128;
    result->scene_motion = 0;
    result->input_width = frame->width;
    result->input_height = frame->height;

    if (!result->face_detected) {
        result->expression = ECHOMATE_EXPRESSION_UNKNOWN;
    } else if (phase < 60U) {
        result->expression = ECHOMATE_EXPRESSION_HAPPY;
    } else if (phase < 160U) {
        result->expression = ECHOMATE_EXPRESSION_NEUTRAL;
    } else {
        result->expression = ECHOMATE_EXPRESSION_SAD;
    }

    return ESP_OK;
}
