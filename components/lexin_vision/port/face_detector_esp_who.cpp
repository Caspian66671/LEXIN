#include "face_detector_esp_who.h"

#include <algorithm>
#include <cinttypes>
#include <list>
#include <memory>
#include <new>

#include "esp_check.h"
#include "esp_log.h"

#include "echomate_config.h"
#include "expression_adapter.h"

#if CONFIG_ECHOMATE_ENABLE_ESP_WHO && __has_include("human_face_detect.hpp")
#include "human_face_detect.hpp"

static const char *TAG = "face_detector_who";
static std::unique_ptr<HumanFaceDetect> s_model;
static face_detection_result_t s_last_result;
static bool s_last_result_valid;
static bool s_input_logged;
static bool s_filter_logged;
static uint32_t s_roi_reject_count;

static uint8_t luma_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint8_t>(((static_cast<uint16_t>(77U) * r) +
                                 (static_cast<uint16_t>(150U) * g) +
                                 (static_cast<uint16_t>(29U) * b)) >> 8);
}

static uint8_t max3_u8(uint8_t a, uint8_t b, uint8_t c)
{
    return std::max(a, std::max(b, c));
}

static uint8_t min3_u8(uint8_t a, uint8_t b, uint8_t c)
{
    return std::min(a, std::min(b, c));
}

static bool validate_face_roi(const vision_input_frame_t *input,
                              const face_detection_result_t *face,
                              expression_features_t *metrics)
{
    if (metrics) {
        *metrics = {};
    }

#if CONFIG_ECHOMATE_ESP_WHO_ENABLE_ROI_CHECK
    if (!input || !face || !input->buffer || !face->detected ||
        face->width < 4 || face->height < 4) {
        return false;
    }

    const uint32_t x0 = face->x;
    const uint32_t y0 = face->y;
    const uint32_t x1 = std::min<uint32_t>(input->width, x0 + face->width);
    const uint32_t y1 = std::min<uint32_t>(input->height, y0 + face->height);
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }

    uint32_t luma_sum = 0;
    uint32_t upper_luma_sum = 0;
    uint32_t lower_luma_sum = 0;
    uint32_t r_sum = 0;
    uint32_t g_sum = 0;
    uint32_t b_sum = 0;
    uint32_t sample_count = 0;
    uint32_t upper_sample_count = 0;
    uint32_t lower_sample_count = 0;
    uint8_t luma_min = 255;
    uint8_t luma_max = 0;
    uint8_t upper_luma_min = 255;
    uint8_t upper_luma_max = 0;
    uint8_t lower_luma_min = 255;
    uint8_t lower_luma_max = 0;
    uint32_t mouth_luma_sum = 0;
    uint32_t mouth_sample_count = 0;
    uint8_t mouth_luma_min = 255;
    uint8_t mouth_luma_max = 0;

    const uint32_t step_y = face->height > 18 ? 2U : 1U;
    const uint32_t step_x = face->width > 18 ? 2U : 1U;
    const uint32_t lower_start_y = y0 + ((y1 - y0) * 54U) / 100U;
    const uint32_t mouth_x0 = x0 + ((x1 - x0) * 25U) / 100U;
    const uint32_t mouth_x1 = x0 + ((x1 - x0) * 75U) / 100U;
    const uint32_t mouth_y0 = y0 + ((y1 - y0) * 58U) / 100U;
    const uint32_t mouth_y1 = y0 + ((y1 - y0) * 84U) / 100U;
    for (uint32_t y = y0; y < y1; y += step_y) {
        for (uint32_t x = x0; x < x1; x += step_x) {
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            if (input->format == ECHOMATE_PIXEL_RGB888) {
                const size_t offset = ((size_t)y * input->width + x) * 3U;
                r = input->buffer[offset];
                g = input->buffer[offset + 1U];
                b = input->buffer[offset + 2U];
            } else {
                const uint8_t gray = input->buffer[(size_t)y * input->width + x];
                r = gray;
                g = gray;
                b = gray;
            }

            const uint8_t luma = luma_from_rgb(r, g, b);
            luma_sum += luma;
            if (y >= lower_start_y) {
                lower_luma_sum += luma;
                lower_sample_count++;
                if (luma < lower_luma_min) {
                    lower_luma_min = luma;
                }
                if (luma > lower_luma_max) {
                    lower_luma_max = luma;
                }
            } else {
                upper_luma_sum += luma;
                upper_sample_count++;
                if (luma < upper_luma_min) {
                    upper_luma_min = luma;
                }
                if (luma > upper_luma_max) {
                    upper_luma_max = luma;
                }
            }
            if (x >= mouth_x0 && x < mouth_x1 && y >= mouth_y0 && y < mouth_y1) {
                mouth_luma_sum += luma;
                mouth_sample_count++;
                if (luma < mouth_luma_min) {
                    mouth_luma_min = luma;
                }
                if (luma > mouth_luma_max) {
                    mouth_luma_max = luma;
                }
            }
            r_sum += r;
            g_sum += g;
            b_sum += b;
            sample_count++;
            if (luma < luma_min) {
                luma_min = luma;
            }
            if (luma > luma_max) {
                luma_max = luma;
            }
        }
    }

    if (sample_count == 0) {
        return false;
    }

    const uint8_t mean_luma = static_cast<uint8_t>(luma_sum / sample_count);
    const uint8_t contrast = static_cast<uint8_t>(luma_max - luma_min);
    const uint8_t avg_r = static_cast<uint8_t>(r_sum / sample_count);
    const uint8_t avg_g = static_cast<uint8_t>(g_sum / sample_count);
    const uint8_t avg_b = static_cast<uint8_t>(b_sum / sample_count);
    const uint8_t channel_spread = max3_u8(avg_r, avg_g, avg_b) - min3_u8(avg_r, avg_g, avg_b);
    const uint8_t upper_luma = upper_sample_count ?
        static_cast<uint8_t>(upper_luma_sum / upper_sample_count) : mean_luma;
    const uint8_t lower_luma = lower_sample_count ?
        static_cast<uint8_t>(lower_luma_sum / lower_sample_count) : mean_luma;
    const uint8_t upper_contrast = upper_sample_count ?
        static_cast<uint8_t>(upper_luma_max - upper_luma_min) : contrast;
    const uint8_t lower_contrast = lower_sample_count ?
        static_cast<uint8_t>(lower_luma_max - lower_luma_min) : contrast;
    const uint8_t mouth_luma = mouth_sample_count ?
        static_cast<uint8_t>(mouth_luma_sum / mouth_sample_count) : lower_luma;
    const uint8_t mouth_contrast = mouth_sample_count ?
        static_cast<uint8_t>(mouth_luma_max - mouth_luma_min) : lower_contrast;

    if (metrics) {
        metrics->valid = true;
        metrics->mean_luma = mean_luma;
        metrics->contrast = contrast;
        metrics->channel_spread = channel_spread;
        metrics->upper_luma = upper_luma;
        metrics->lower_luma = lower_luma;
        metrics->upper_contrast = upper_contrast;
        metrics->lower_contrast = lower_contrast;
        metrics->mouth_luma = mouth_luma;
        metrics->mouth_contrast = mouth_contrast;
    }

    const bool high_confidence = face->confidence >= 95U;
    const bool brightness_ok = mean_luma >= CONFIG_ECHOMATE_ESP_WHO_ROI_MIN_LUMA &&
        mean_luma <= CONFIG_ECHOMATE_ESP_WHO_ROI_MAX_LUMA;
    const bool texture_ok = contrast >= CONFIG_ECHOMATE_ESP_WHO_ROI_MIN_CONTRAST;
    const bool color_ok = channel_spread <= CONFIG_ECHOMATE_ESP_WHO_ROI_MAX_CHANNEL_SPREAD;

    return brightness_ok && (texture_ok || high_confidence) && (color_ok || high_confidence);
#else
    (void)input;
    (void)face;
    return true;
#endif
}

bool face_detector_esp_who_compiled(void)
{
    return true;
}

esp_err_t face_detector_esp_who_init(void)
{
    if (!s_model) {
        s_model.reset(new (std::nothrow) HumanFaceDetect(
            static_cast<HumanFaceDetect::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL),
            true));
        if (!s_model) {
            ESP_LOGE(TAG, "HumanFaceDetect model allocation failed");
            return ESP_ERR_NO_MEM;
        }
        // Keep the detector sensitive enough for a tabletop demo while the
        // temporal confirmation and geometry filters suppress one-frame hits.
        s_model->set_score_thr(0.35f, 0);
        s_model->set_score_thr(0.45f, 1);
        ESP_LOGI(TAG, "HumanFaceDetect model wrapper created");
    }

    return ESP_OK;
}

esp_err_t face_detector_esp_who_run(const vision_input_frame_t *input,
                                    face_detection_result_t *face,
                                    bool *ran_inference)
{
    if (ran_inference) {
        *ran_inference = false;
    }
    if (!input || !face || !input->buffer ||
        (input->format != ECHOMATE_PIXEL_GRAY8 && input->format != ECHOMATE_PIXEL_RGB888) ||
        input->width == 0 || input->height == 0 ||
        input->length < static_cast<size_t>(input->width) * input->height *
            (input->format == ECHOMATE_PIXEL_RGB888 ? 3U : 1U)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_model) {
        ESP_RETURN_ON_ERROR(face_detector_esp_who_init(), TAG, "ESP-WHO model init failed");
    }

    if (CONFIG_ECHOMATE_ESP_WHO_INFERENCE_INTERVAL > 1 &&
        (input->frame_id % CONFIG_ECHOMATE_ESP_WHO_INFERENCE_INTERVAL) != 0 &&
        s_last_result_valid) {
        *face = s_last_result;
        face->fresh = false;
        return ESP_OK;
    }

    dl::image::img_t img = {
        .data = const_cast<uint8_t *>(input->buffer),
        .width = input->width,
        .height = input->height,
        .pix_type = input->format == ECHOMATE_PIXEL_RGB888 ?
            dl::image::DL_IMAGE_PIX_TYPE_RGB888 : dl::image::DL_IMAGE_PIX_TYPE_GRAY,
    };
    if (!s_input_logged) {
        ESP_LOGI(TAG, "HumanFaceDetect input=%ux%u %s interval=%d",
                 input->width,
                 input->height,
                 input->format == ECHOMATE_PIXEL_RGB888 ? "RGB888" : "GRAY8",
                 CONFIG_ECHOMATE_ESP_WHO_INFERENCE_INTERVAL);
        s_input_logged = true;
    }

    if (ran_inference) {
        *ran_inference = true;
    }
    std::list<dl::detect::result_t> &results = s_model->run(img);

    face->backend = ECHOMATE_FACE_DETECTOR_ESP_WHO;
    face->fresh = true;
    face->expression = ECHOMATE_EXPRESSION_UNKNOWN;
    face->detected = false;
    face->confidence = 0;

    if (results.empty()) {
        s_last_result = *face;
        s_last_result_valid = true;
        return ESP_OK;
    }

    const auto best = std::max_element(results.begin(), results.end(),
                                       [](const dl::detect::result_t &a,
                                          const dl::detect::result_t &b) {
                                           return a.score < b.score;
                                       });
    if (best == results.end() || best->box.size() < 4) {
        s_last_result = *face;
        s_last_result_valid = true;
        return ESP_OK;
    }

    const int x0 = std::max(0, std::min<int>(best->box[0], input->width - 1));
    const int y0 = std::max(0, std::min<int>(best->box[1], input->height - 1));
    const int x1 = std::max(0, std::min<int>(best->box[2], input->width - 1));
    const int y1 = std::max(0, std::min<int>(best->box[3], input->height - 1));

    face->confidence = static_cast<uint8_t>(std::max(0.0f, std::min(100.0f, best->score * 100.0f)));
    face->x = static_cast<uint16_t>(x0);
    face->y = static_cast<uint16_t>(y0);
    face->width = static_cast<uint16_t>(x1 - x0);
    face->height = static_cast<uint16_t>(y1 - y0);
    const uint32_t input_area = static_cast<uint32_t>(input->width) * input->height;
    const uint32_t box_area = static_cast<uint32_t>(face->width) * face->height;
    const uint32_t box_percent = input_area ? (box_area * 100U) / input_area : 0;
    const uint32_t aspect_q8 = face->height ?
        (static_cast<uint32_t>(face->width) * 256U) / face->height : 0;
    face->detected = x1 > x0 && y1 > y0 &&
        face->confidence >= CONFIG_ECHOMATE_ESP_WHO_MIN_CONFIDENCE &&
        box_percent >= CONFIG_ECHOMATE_ESP_WHO_MIN_BOX_PERCENT &&
        box_percent <= CONFIG_ECHOMATE_ESP_WHO_MAX_BOX_PERCENT &&
        aspect_q8 >= CONFIG_ECHOMATE_ESP_WHO_MIN_ASPECT_Q8 &&
        aspect_q8 <= CONFIG_ECHOMATE_ESP_WHO_MAX_ASPECT_Q8;

    expression_features_t roi = {};
    const bool geometry_detected = face->detected;
    const bool roi_ok = !geometry_detected || validate_face_roi(input, face, &roi);
    // A valid model box is a face detection. ROI statistics describe whether
    // expression classification is trustworthy; they must not erase the box.
    face->detected = geometry_detected;
    face->expression = face->detected && roi_ok ?
        expression_adapter_classify_from_input(input, face, &roi) : ECHOMATE_EXPRESSION_UNKNOWN;

    if (!s_filter_logged) {
        ESP_LOGI(TAG,
                 "ESP-WHO filter min_conf=%d area=%d..%d%% aspect_q8=%d..%d roi=%d luma=%d..%d contrast>=%d spread<=%d expr=%d first_score=%u first_area=%" PRIu32 "%% first_aspect_q8=%" PRIu32 " roi_luma=%u roi_contrast=%u roi_spread=%u upper=%u lower=%u uc=%u lc=%u mouth_luma=%u mouth_contrast=%u",
                 CONFIG_ECHOMATE_ESP_WHO_MIN_CONFIDENCE,
                 CONFIG_ECHOMATE_ESP_WHO_MIN_BOX_PERCENT,
                 CONFIG_ECHOMATE_ESP_WHO_MAX_BOX_PERCENT,
                 CONFIG_ECHOMATE_ESP_WHO_MIN_ASPECT_Q8,
                 CONFIG_ECHOMATE_ESP_WHO_MAX_ASPECT_Q8,
                 CONFIG_ECHOMATE_ESP_WHO_ENABLE_ROI_CHECK,
                 CONFIG_ECHOMATE_ESP_WHO_ROI_MIN_LUMA,
                 CONFIG_ECHOMATE_ESP_WHO_ROI_MAX_LUMA,
                 CONFIG_ECHOMATE_ESP_WHO_ROI_MIN_CONTRAST,
                 CONFIG_ECHOMATE_ESP_WHO_ROI_MAX_CHANNEL_SPREAD,
                 CONFIG_ECHOMATE_ENABLE_EXPRESSION_HEURISTIC,
                 face->confidence,
                 box_percent,
                 aspect_q8,
                 roi.mean_luma,
                 roi.contrast,
                 roi.channel_spread,
                 roi.upper_luma,
                 roi.lower_luma,
                 roi.upper_contrast,
                 roi.lower_contrast,
                 roi.mouth_luma,
                 roi.mouth_contrast);
        s_filter_logged = true;
    }
    if (geometry_detected && !roi_ok) {
        s_roi_reject_count++;
        if (s_roi_reject_count <= 8U || (s_roi_reject_count % 32U) == 0U) {
            ESP_LOGI(TAG,
                     "ESP-WHO expression ROI reject count=%" PRIu32 " score=%u area=%" PRIu32 "%% aspect_q8=%" PRIu32 " luma=%u contrast=%u spread=%u upper=%u lower=%u uc=%u lc=%u mouth_luma=%u mouth_contrast=%u",
                     s_roi_reject_count,
                     face->confidence,
                     box_percent,
                     aspect_q8,
                     roi.mean_luma,
                     roi.contrast,
                     roi.channel_spread,
                     roi.upper_luma,
                     roi.lower_luma,
                     roi.upper_contrast,
                     roi.lower_contrast,
                     roi.mouth_luma,
                     roi.mouth_contrast);
        }
    }

    s_last_result = *face;
    s_last_result_valid = true;

    return ESP_OK;
}

#else

bool face_detector_esp_who_compiled(void)
{
    return false;
}

esp_err_t face_detector_esp_who_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t face_detector_esp_who_run(const vision_input_frame_t *input,
                                    face_detection_result_t *face,
                                    bool *ran_inference)
{
    (void)input;
    (void)face;
    if (ran_inference) {
        *ran_inference = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
