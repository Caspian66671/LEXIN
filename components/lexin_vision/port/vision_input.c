#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "echomate_app.h"
#include "echomate_config.h"

static const char *TAG = "vision_input";

#define VISION_INPUT_PIXELS (CONFIG_ECHOMATE_VISION_INPUT_WIDTH * CONFIG_ECHOMATE_VISION_INPUT_HEIGHT)

#if CONFIG_ECHOMATE_VISION_MODEL_INPUT_RGB888
#define VISION_INPUT_BYTES (VISION_INPUT_PIXELS * 3)
#define VISION_INPUT_FORMAT ECHOMATE_PIXEL_RGB888
#define VISION_INPUT_FORMAT_NAME "RGB888"
#else
#define VISION_INPUT_BYTES VISION_INPUT_PIXELS
#define VISION_INPUT_FORMAT ECHOMATE_PIXEL_GRAY8
#define VISION_INPUT_FORMAT_NAME "GRAY8"
#endif

static uint8_t s_model_input[VISION_INPUT_BYTES];
static bool s_input_logged;

static uint8_t rgb888_to_luma(uint8_t r, uint8_t g, uint8_t b);

static uint8_t clamp_u8_from_u32(uint32_t value)
{
    return value > 255U ? 255U : (uint8_t)value;
}

static void adjust_model_pixel(size_t pixel_index, uint16_t gain_q8)
{
#if CONFIG_ECHOMATE_VISION_MODEL_INPUT_RGB888
    const size_t offset = pixel_index * 3U;
    s_model_input[offset] = clamp_u8_from_u32(((uint32_t)s_model_input[offset] * gain_q8 + 128U) >> 8);
    s_model_input[offset + 1U] = clamp_u8_from_u32(((uint32_t)s_model_input[offset + 1U] * gain_q8 + 128U) >> 8);
    s_model_input[offset + 2U] = clamp_u8_from_u32(((uint32_t)s_model_input[offset + 2U] * gain_q8 + 128U) >> 8);
#else
    s_model_input[pixel_index] = clamp_u8_from_u32(((uint32_t)s_model_input[pixel_index] * gain_q8 + 128U) >> 8);
#endif
}

static void auto_expose_model_input(uint8_t raw_mean_luma,
                                    uint8_t *adjusted_mean_luma,
                                    uint8_t *adjusted_contrast)
{
    uint16_t gain_q8 = 256;
    if (raw_mean_luma > 0 && raw_mean_luma < 105U) {
        gain_q8 = (uint16_t)((120U * 256U) / raw_mean_luma);
        if (gain_q8 > 768U) {
            gain_q8 = 768U;
        }
    } else if (raw_mean_luma > 165U) {
        gain_q8 = (uint16_t)((138U * 256U) / raw_mean_luma);
        if (gain_q8 < 128U) {
            gain_q8 = 128U;
        }
    }

    uint32_t luma_sum = 0;
    uint8_t luma_min = 255;
    uint8_t luma_max = 0;
    for (size_t i = 0; i < VISION_INPUT_PIXELS; ++i) {
        adjust_model_pixel(i, gain_q8);
#if CONFIG_ECHOMATE_VISION_MODEL_INPUT_RGB888
        const size_t offset = i * 3U;
        const uint8_t luma = rgb888_to_luma(s_model_input[offset],
                                            s_model_input[offset + 1U],
                                            s_model_input[offset + 2U]);
#else
        const uint8_t luma = s_model_input[i];
#endif
        luma_sum += luma;
        if (luma < luma_min) {
            luma_min = luma;
        }
        if (luma > luma_max) {
            luma_max = luma;
        }
    }

    if (adjusted_mean_luma) {
        *adjusted_mean_luma = (uint8_t)(luma_sum / VISION_INPUT_PIXELS);
    }
    if (adjusted_contrast) {
        *adjusted_contrast = (uint8_t)(luma_max - luma_min);
    }
}

static void rgb565_to_rgb888(uint16_t pixel, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    *red = (uint8_t)(((pixel >> 11) & 0x1f) * 255U / 31U);
    *green = (uint8_t)(((pixel >> 5) & 0x3f) * 255U / 63U);
    *blue = (uint8_t)((pixel & 0x1f) * 255U / 31U);
}

static uint8_t rgb888_to_luma(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)(((uint16_t)77U * r + (uint16_t)150U * g + (uint16_t)29U * b) >> 8);
}

static void store_model_pixel(size_t pixel_index, uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_ECHOMATE_VISION_MODEL_INPUT_RGB888
    const size_t offset = pixel_index * 3U;
    s_model_input[offset] = r;
    s_model_input[offset + 1U] = g;
    s_model_input[offset + 2U] = b;
#else
    s_model_input[pixel_index] = rgb888_to_luma(r, g, b);
#endif
}

esp_err_t vision_input_prepare(const camera_frame_msg_t *frame, vision_input_frame_t *input)
{
    if (!frame || !input || !frame->buffer || frame->width == 0 || frame->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t luma_sum = 0;
    uint8_t luma_min = 255;
    uint8_t luma_max = 0;

    if (frame->format == ECHOMATE_PIXEL_RGB565) {
        const size_t source_pixels = frame->length / sizeof(uint16_t);
        const uint16_t *source = (const uint16_t *)frame->buffer;
        if (source_pixels < (size_t)frame->width * frame->height) {
            return ESP_ERR_INVALID_SIZE;
        }

        for (uint32_t y = 0; y < CONFIG_ECHOMATE_VISION_INPUT_HEIGHT; ++y) {
            const uint32_t src_y = (y * frame->height) / CONFIG_ECHOMATE_VISION_INPUT_HEIGHT;
            for (uint32_t x = 0; x < CONFIG_ECHOMATE_VISION_INPUT_WIDTH; ++x) {
                const uint32_t src_x = (x * frame->width) / CONFIG_ECHOMATE_VISION_INPUT_WIDTH;
                uint8_t r = 0;
                uint8_t g = 0;
                uint8_t b = 0;
                rgb565_to_rgb888(source[(size_t)src_y * frame->width + src_x], &r, &g, &b);
                const uint8_t luma = rgb888_to_luma(r, g, b);
                store_model_pixel((size_t)y * CONFIG_ECHOMATE_VISION_INPUT_WIDTH + x, r, g, b);
                luma_sum += luma;
                if (luma < luma_min) {
                    luma_min = luma;
                }
                if (luma > luma_max) {
                    luma_max = luma;
                }
            }
        }
    } else if (frame->format == ECHOMATE_PIXEL_GRAY8) {
        if (frame->length < (size_t)frame->width * frame->height) {
            return ESP_ERR_INVALID_SIZE;
        }

        for (uint32_t y = 0; y < CONFIG_ECHOMATE_VISION_INPUT_HEIGHT; ++y) {
            const uint32_t src_y = (y * frame->height) / CONFIG_ECHOMATE_VISION_INPUT_HEIGHT;
            for (uint32_t x = 0; x < CONFIG_ECHOMATE_VISION_INPUT_WIDTH; ++x) {
                const uint32_t src_x = (x * frame->width) / CONFIG_ECHOMATE_VISION_INPUT_WIDTH;
                const uint8_t luma = frame->buffer[(size_t)src_y * frame->width + src_x];
                store_model_pixel((size_t)y * CONFIG_ECHOMATE_VISION_INPUT_WIDTH + x, luma, luma, luma);
                luma_sum += luma;
                if (luma < luma_min) {
                    luma_min = luma;
                }
                if (luma > luma_max) {
                    luma_max = luma;
                }
            }
        }
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint8_t raw_mean_luma = (uint8_t)(luma_sum / VISION_INPUT_PIXELS);
    const uint8_t raw_contrast = (uint8_t)(luma_max - luma_min);
    uint8_t adjusted_mean_luma = raw_mean_luma;
    uint8_t adjusted_contrast = raw_contrast;
    auto_expose_model_input(raw_mean_luma, &adjusted_mean_luma, &adjusted_contrast);

    memset(input, 0, sizeof(*input));
    input->frame_id = frame->frame_id;
    input->timestamp_us = esp_timer_get_time();
    input->width = CONFIG_ECHOMATE_VISION_INPUT_WIDTH;
    input->height = CONFIG_ECHOMATE_VISION_INPUT_HEIGHT;
    input->format = VISION_INPUT_FORMAT;
    input->buffer = s_model_input;
    input->length = sizeof(s_model_input);
    input->mean_luma = adjusted_mean_luma;
    input->contrast = adjusted_contrast;

    if (!s_input_logged) {
        ESP_LOGI(TAG, "vision input prepared as %dx%d %s, %u bytes auto_exp raw_mean=%u raw_contrast=%u",
                 CONFIG_ECHOMATE_VISION_INPUT_WIDTH,
                 CONFIG_ECHOMATE_VISION_INPUT_HEIGHT,
                 VISION_INPUT_FORMAT_NAME,
                 (unsigned int)sizeof(s_model_input),
                 raw_mean_luma,
                 raw_contrast);
        s_input_logged = true;
    }

    return ESP_OK;
}
