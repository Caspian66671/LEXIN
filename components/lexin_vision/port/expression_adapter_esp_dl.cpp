#include "expression_adapter_esp_dl.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"

#include "echomate_config.h"

#if defined(__has_include)
#if __has_include("dl_model_base.hpp") && __has_include("fbs_model.hpp")
#define ECHOMATE_EXPRESSION_ESP_DL_HEADERS_AVAILABLE 1
#else
#define ECHOMATE_EXPRESSION_ESP_DL_HEADERS_AVAILABLE 0
#endif
#else
#define ECHOMATE_EXPRESSION_ESP_DL_HEADERS_AVAILABLE 0
#endif

#if CONFIG_ECHOMATE_ENABLE_ESP_DL && ECHOMATE_EXPRESSION_ESP_DL_HEADERS_AVAILABLE
#include "dl_model_base.hpp"
#include "fbs_model.hpp"
#endif

#define ECHOMATE_EXPRESSION_ESP_DL_RUNTIME_AVAILABLE \
    (CONFIG_ECHOMATE_ENABLE_ESP_DL && ECHOMATE_EXPRESSION_ESP_DL_HEADERS_AVAILABLE && \
     ECHOMATE_EXPRESSION_ESP_DL_MODEL_EMBEDDED)

static const char *TAG __attribute__((unused)) = "expression_esp_dl";

#if CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_RGB888
#define EXPRESSION_ESP_DL_CHANNELS 3
#define EXPRESSION_ESP_DL_FORMAT   ECHOMATE_PIXEL_RGB888
#else
#define EXPRESSION_ESP_DL_CHANNELS 1
#define EXPRESSION_ESP_DL_FORMAT   ECHOMATE_PIXEL_GRAY8
#endif

#define EXPRESSION_ESP_DL_INPUT_PIXELS \
    (CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH * CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT)
#define EXPRESSION_ESP_DL_INPUT_BYTES \
    (EXPRESSION_ESP_DL_INPUT_PIXELS * EXPRESSION_ESP_DL_CHANNELS)

static expression_esp_dl_status_t s_status = {
    .headers_available = ECHOMATE_EXPRESSION_ESP_DL_HEADERS_AVAILABLE,
    .model_embedded = ECHOMATE_EXPRESSION_ESP_DL_MODEL_EMBEDDED,
    .input_width = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH,
    .input_height = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT,
    .input_channels = EXPRESSION_ESP_DL_CHANNELS,
    .input_format = EXPRESSION_ESP_DL_FORMAT,
    .inference_us = 0,
    .confidence = 0,
    .score_neutral = 0,
    .score_happy = 0,
    .score_sad = 0,
    .fallback_reason = "not initialized",
};
#if ECHOMATE_EXPRESSION_ESP_DL_RUNTIME_AVAILABLE
static bool s_init_logged;
static bool s_input_logged;
static uint8_t s_face_crop[EXPRESSION_ESP_DL_INPUT_BYTES];

extern const uint8_t expression_espdl[] asm("_binary_expression_espdl_start");
static std::unique_ptr<dl::Model> s_model;

static uint8_t luma_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint8_t>(((static_cast<uint16_t>(77U) * r) +
                                 (static_cast<uint16_t>(150U) * g) +
                                 (static_cast<uint16_t>(29U) * b)) >> 8);
}

static void source_pixel(const vision_input_frame_t *input,
                         uint32_t x,
                         uint32_t y,
                         uint8_t *r,
                         uint8_t *g,
                         uint8_t *b)
{
    const size_t index = static_cast<size_t>(y) * input->width + x;
    if (input->format == ECHOMATE_PIXEL_RGB888) {
        const size_t offset = index * 3U;
        *r = input->buffer[offset];
        *g = input->buffer[offset + 1U];
        *b = input->buffer[offset + 2U];
    } else {
        *r = input->buffer[index];
        *g = *r;
        *b = *r;
    }
}

static bool prepare_face_crop(const vision_input_frame_t *input, const face_detection_result_t *face)
{
    if (!input || !face || !input->buffer || !face->detected ||
        (input->format != ECHOMATE_PIXEL_GRAY8 && input->format != ECHOMATE_PIXEL_RGB888) ||
        face->width == 0 || face->height == 0) {
        return false;
    }

    const uint32_t crop_w = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH;
    const uint32_t crop_h = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT;
    const uint32_t x0 = std::min<uint32_t>(input->width - 1U, face->x);
    const uint32_t y0 = std::min<uint32_t>(input->height - 1U, face->y);
    const uint32_t x1 = std::min<uint32_t>(input->width, x0 + face->width);
    const uint32_t y1 = std::min<uint32_t>(input->height, y0 + face->height);
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }

    for (uint32_t y = 0; y < crop_h; ++y) {
        const uint32_t src_y = y0 + (y * (y1 - y0)) / crop_h;
        for (uint32_t x = 0; x < crop_w; ++x) {
            const uint32_t src_x = x0 + (x * (x1 - x0)) / crop_w;
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            source_pixel(input, src_x, src_y, &r, &g, &b);
            const size_t dst = (static_cast<size_t>(y) * crop_w + x) * EXPRESSION_ESP_DL_CHANNELS;
#if CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_RGB888
            s_face_crop[dst] = r;
            s_face_crop[dst + 1U] = g;
            s_face_crop[dst + 2U] = b;
#else
            s_face_crop[dst] = luma_from_rgb(r, g, b);
#endif
        }
    }

    return true;
}
#endif

static void copy_status(expression_esp_dl_status_t *status)
{
    if (status) {
        *status = s_status;
    }
}

#if ECHOMATE_EXPRESSION_ESP_DL_RUNTIME_AVAILABLE
typedef enum {
    TENSOR_LAYOUT_HWC,
    TENSOR_LAYOUT_CHW,
    TENSOR_LAYOUT_FLAT,
} tensor_layout_t;

static void infer_input_layout(dl::TensorBase *tensor,
                               uint16_t *width,
                               uint16_t *height,
                               uint8_t *channels,
                               tensor_layout_t *layout)
{
    *width = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH;
    *height = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT;
    *channels = EXPRESSION_ESP_DL_CHANNELS;
    *layout = TENSOR_LAYOUT_HWC;

    if (!tensor) {
        return;
    }

    const std::vector<int> shape = tensor->get_shape();
    if (shape.size() == 4U) {
        if (shape[3] == 1 || shape[3] == 3) {
            *height = static_cast<uint16_t>(shape[1]);
            *width = static_cast<uint16_t>(shape[2]);
            *channels = static_cast<uint8_t>(shape[3]);
            *layout = TENSOR_LAYOUT_HWC;
        } else if (shape[1] == 1 || shape[1] == 3) {
            *channels = static_cast<uint8_t>(shape[1]);
            *height = static_cast<uint16_t>(shape[2]);
            *width = static_cast<uint16_t>(shape[3]);
            *layout = TENSOR_LAYOUT_CHW;
        }
    } else if (shape.size() == 3U) {
        if (shape[2] == 1 || shape[2] == 3) {
            *height = static_cast<uint16_t>(shape[0]);
            *width = static_cast<uint16_t>(shape[1]);
            *channels = static_cast<uint8_t>(shape[2]);
            *layout = TENSOR_LAYOUT_HWC;
        } else if (shape[0] == 1 || shape[0] == 3) {
            *channels = static_cast<uint8_t>(shape[0]);
            *height = static_cast<uint16_t>(shape[1]);
            *width = static_cast<uint16_t>(shape[2]);
            *layout = TENSOR_LAYOUT_CHW;
        }
    } else if (shape.size() == 2U) {
        *height = static_cast<uint16_t>(shape[0]);
        *width = static_cast<uint16_t>(shape[1]);
        *channels = 1;
        *layout = TENSOR_LAYOUT_FLAT;
    }

    if (*width == 0 || *height == 0 || (*channels != 1 && *channels != 3)) {
        *width = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH;
        *height = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT;
        *channels = EXPRESSION_ESP_DL_CHANNELS;
        *layout = TENSOR_LAYOUT_HWC;
    }
}

static uint8_t crop_value(uint16_t x, uint16_t y, uint8_t c)
{
    const uint32_t src_x = ((uint32_t)x * CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH) / s_status.input_width;
    const uint32_t src_y = ((uint32_t)y * CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT) / s_status.input_height;
    const size_t base = (static_cast<size_t>(src_y) * CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH + src_x) *
        EXPRESSION_ESP_DL_CHANNELS;
#if CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_RGB888
    if (s_status.input_channels == 1) {
        return luma_from_rgb(s_face_crop[base], s_face_crop[base + 1U], s_face_crop[base + 2U]);
    }
    return s_face_crop[base + std::min<uint8_t>(c, 2U)];
#else
    (void)c;
    return s_face_crop[base];
#endif
}

static size_t tensor_offset(uint16_t x,
                            uint16_t y,
                            uint8_t c,
                            uint16_t width,
                            uint16_t height,
                            uint8_t channels,
                            tensor_layout_t layout)
{
    if (layout == TENSOR_LAYOUT_CHW) {
        return static_cast<size_t>(c) * width * height + static_cast<size_t>(y) * width + x;
    }
    return (static_cast<size_t>(y) * width + x) * channels + c;
}

static void fill_input_tensor(dl::TensorBase *tensor, tensor_layout_t layout)
{
    const uint16_t width = s_status.input_width;
    const uint16_t height = s_status.input_height;
    const uint8_t channels = s_status.input_channels;
    const size_t max_elements = static_cast<size_t>(tensor->get_size());

    if (tensor->get_dtype() == dl::DATA_TYPE_FLOAT) {
        float *dst = tensor->get_element_ptr<float>();
        for (uint16_t y = 0; y < height; ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                for (uint8_t c = 0; c < channels; ++c) {
                    const size_t offset = tensor_offset(x, y, c, width, height, channels, layout);
                    if (offset < max_elements) {
                        dst[offset] = ((float)crop_value(x, y, c) - 127.5f) / 127.5f;
                    }
                }
            }
        }
    } else if (tensor->get_dtype() == dl::DATA_TYPE_UINT8) {
        uint8_t *dst = tensor->get_element_ptr<uint8_t>();
        for (uint16_t y = 0; y < height; ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                for (uint8_t c = 0; c < channels; ++c) {
                    const size_t offset = tensor_offset(x, y, c, width, height, channels, layout);
                    if (offset < max_elements) {
                        dst[offset] = crop_value(x, y, c);
                    }
                }
            }
        }
    } else {
        int8_t *dst = tensor->get_element_ptr<int8_t>();
        for (uint16_t y = 0; y < height; ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                for (uint8_t c = 0; c < channels; ++c) {
                    const size_t offset = tensor_offset(x, y, c, width, height, channels, layout);
                    if (offset < max_elements) {
                        dst[offset] = static_cast<int8_t>((int)crop_value(x, y, c) - 128);
                    }
                }
            }
        }
    }
}

static int32_t output_value(dl::TensorBase *tensor, size_t index)
{
    if (!tensor || index >= static_cast<size_t>(tensor->get_size())) {
        return INT32_MIN;
    }

    switch (tensor->get_dtype()) {
    case dl::DATA_TYPE_FLOAT:
        return static_cast<int32_t>(tensor->get_element_ptr<float>()[index] * 1000.0f);
    case dl::DATA_TYPE_UINT8:
        return tensor->get_element_ptr<uint8_t>()[index];
    case dl::DATA_TYPE_INT8:
        return tensor->get_element_ptr<int8_t>()[index];
    case dl::DATA_TYPE_INT16:
        return tensor->get_element_ptr<int16_t>()[index];
    case dl::DATA_TYPE_INT32:
        return tensor->get_element_ptr<int32_t>()[index];
    default:
        return INT32_MIN;
    }
}

static uint8_t confidence_from_scores(int32_t top, int32_t second)
{
    if (top == INT32_MIN) {
        return 0;
    }
    const int32_t margin = top - second;
    if (top <= 100 && top >= 0) {
        return static_cast<uint8_t>(std::min<int32_t>(100, top));
    }
    return static_cast<uint8_t>(std::min<int32_t>(100, 50 + std::max<int32_t>(0, margin)));
}
#endif

esp_err_t expression_esp_dl_init(expression_esp_dl_status_t *status)
{
    s_status.headers_available = ECHOMATE_EXPRESSION_ESP_DL_HEADERS_AVAILABLE;
    s_status.model_embedded = ECHOMATE_EXPRESSION_ESP_DL_MODEL_EMBEDDED;
    s_status.input_width = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_WIDTH;
    s_status.input_height = CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_HEIGHT;
    s_status.input_channels = EXPRESSION_ESP_DL_CHANNELS;
    s_status.input_format = EXPRESSION_ESP_DL_FORMAT;
    s_status.fallback_reason = "none";

#if !CONFIG_ECHOMATE_ENABLE_ESP_DL
    s_status.fallback_reason = "esp-dl disabled";
    copy_status(status);
    return ESP_ERR_NOT_SUPPORTED;
#elif !ECHOMATE_EXPRESSION_ESP_DL_HEADERS_AVAILABLE
    s_status.fallback_reason = "esp-dl headers missing";
    copy_status(status);
    return ESP_ERR_NOT_SUPPORTED;
#elif !ECHOMATE_EXPRESSION_ESP_DL_MODEL_EMBEDDED
    s_status.fallback_reason = "main/models/expression.espdl missing";
    copy_status(status);
    return ESP_ERR_NOT_FOUND;
#else
    if (!s_model) {
        s_model.reset(new (std::nothrow) dl::Model(
            reinterpret_cast<const char *>(expression_espdl),
            fbs::MODEL_LOCATION_IN_FLASH_RODATA,
            0,
            dl::MEMORY_MANAGER_GREEDY,
            nullptr,
            false));
        if (!s_model) {
            s_status.fallback_reason = "esp-dl model allocation failed";
            copy_status(status);
            return ESP_ERR_NO_MEM;
        }
        s_model->minimize();
    }

    dl::TensorBase *input_tensor = s_model->get_input();
    tensor_layout_t layout = TENSOR_LAYOUT_HWC;
    infer_input_layout(input_tensor,
                       &s_status.input_width,
                       &s_status.input_height,
                       &s_status.input_channels,
                       &layout);
    s_status.input_format = s_status.input_channels == 3 ? ECHOMATE_PIXEL_RGB888 : ECHOMATE_PIXEL_GRAY8;
    copy_status(status);

    if (!s_init_logged) {
        ESP_LOGI(TAG, "ESP-DL expression model ready input=%ux%u c=%u headers=%d embedded=%d",
                 s_status.input_width,
                 s_status.input_height,
                 s_status.input_channels,
                 s_status.headers_available,
                 s_status.model_embedded);
        s_init_logged = true;
    }
    return ESP_OK;
#endif
}

esp_err_t expression_esp_dl_classify(const vision_input_frame_t *input,
                                     const face_detection_result_t *face,
                                     echomate_expression_t *expression,
                                     expression_esp_dl_status_t *status)
{
    if (expression) {
        *expression = ECHOMATE_EXPRESSION_UNKNOWN;
    }
    s_status.inference_us = 0;
    s_status.confidence = 0;
    s_status.score_neutral = 0;
    s_status.score_happy = 0;
    s_status.score_sad = 0;

#if !ECHOMATE_EXPRESSION_ESP_DL_RUNTIME_AVAILABLE
    esp_err_t init_ret = expression_esp_dl_init(&s_status);
    copy_status(status);
    return init_ret;
#else
    if (!expression || !prepare_face_crop(input, face)) {
        s_status.fallback_reason = "invalid face crop";
        copy_status(status);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_model) {
        esp_err_t init_ret = expression_esp_dl_init(&s_status);
        if (init_ret != ESP_OK) {
            copy_status(status);
            return init_ret;
        }
    }

    dl::TensorBase *input_tensor = s_model->get_input();
    dl::TensorBase *output_tensor = s_model->get_output();
    if (!input_tensor || !output_tensor || output_tensor->get_size() < 3) {
        s_status.fallback_reason = "model tensor shape unsupported";
        copy_status(status);
        return ESP_ERR_INVALID_SIZE;
    }

    tensor_layout_t layout = TENSOR_LAYOUT_HWC;
    infer_input_layout(input_tensor,
                       &s_status.input_width,
                       &s_status.input_height,
                       &s_status.input_channels,
                       &layout);
    fill_input_tensor(input_tensor, layout);

    const int64_t start_us = esp_timer_get_time();
    s_model->run();
    s_status.inference_us = static_cast<uint32_t>(esp_timer_get_time() - start_us);

    const int32_t scores[3] = {
        output_value(output_tensor, 0),
        output_value(output_tensor, 1),
        output_value(output_tensor, 2),
    };
    s_status.score_neutral = static_cast<int16_t>(std::max<int32_t>(INT16_MIN, std::min<int32_t>(INT16_MAX, scores[0])));
    s_status.score_happy = static_cast<int16_t>(std::max<int32_t>(INT16_MIN, std::min<int32_t>(INT16_MAX, scores[1])));
    s_status.score_sad = static_cast<int16_t>(std::max<int32_t>(INT16_MIN, std::min<int32_t>(INT16_MAX, scores[2])));

    int best = 0;
    int second = 1;
    for (int i = 1; i < 3; ++i) {
        if (scores[i] > scores[best]) {
            second = best;
            best = i;
        } else if (i != best && scores[i] > scores[second]) {
            second = i;
        }
    }
    s_status.confidence = confidence_from_scores(scores[best], scores[second]);

    if (s_status.confidence < CONFIG_ECHOMATE_EXPRESSION_ESP_DL_MIN_CONFIDENCE) {
        *expression = ECHOMATE_EXPRESSION_NEUTRAL;
    } else if (best == 1) {
        *expression = ECHOMATE_EXPRESSION_HAPPY;
    } else if (best == 2) {
        *expression = ECHOMATE_EXPRESSION_SAD;
    } else {
        *expression = ECHOMATE_EXPRESSION_NEUTRAL;
    }

    s_status.fallback_reason = "none";
    if (!s_input_logged) {
        ESP_LOGI(TAG,
                 "ESP-DL expression run input=%ux%u c=%u min_conf=%d out=%" PRId32 ",%" PRId32 ",%" PRId32 " conf=%u inf=%" PRIu32 "us",
                 s_status.input_width,
                 s_status.input_height,
                 s_status.input_channels,
                 CONFIG_ECHOMATE_EXPRESSION_ESP_DL_MIN_CONFIDENCE,
                 scores[0],
                 scores[1],
                 scores[2],
                 s_status.confidence,
                 s_status.inference_us);
        s_input_logged = true;
    }

    copy_status(status);
    return ESP_OK;
#endif
}
