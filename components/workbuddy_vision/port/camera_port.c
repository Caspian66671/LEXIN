#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_check.h"

#include "echomate_app.h"
#include "echomate_config.h"

#if CONFIG_ECHOMATE_ENABLE_CAMERA_DRIVER
#include "esp_video_device.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "linux/videodev2.h"
#endif

static const char *TAG = "camera_port";

#define DIAG_FRAME_WIDTH       288
#define DIAG_FRAME_HEIGHT      216
#define DIAG_FRAME_PIXELS      (DIAG_FRAME_WIDTH * DIAG_FRAME_HEIGHT)
#define CAMERA_BUFFER_COUNT    2
#define CAMERA_STATUS_PERIOD_US 1000000
#define CAMERA_RECOVERY_ERROR_LIMIT 5
#define CAMERA_RECOVERY_COOLDOWN_US 2000000

typedef struct {
    uint8_t *ptr;
    size_t length;
} camera_mmap_buffer_t;

typedef struct {
    echomate_bayer_pattern_t pattern;
    const char *label;
    uint8_t r_row;
    uint8_t r_col;
    uint8_t b_row;
    uint8_t b_col;
    uint8_t g0_row;
    uint8_t g0_col;
    uint8_t g1_row;
    uint8_t g1_col;
} camera_bayer_layout_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint64_t score;
    bool valid;
} camera_format_candidate_t;

static uint16_t *s_diag_frames[2];
static uint8_t s_diag_write_index;
static uint8_t s_raw_lut[256];
static bool s_raw_lut_ready;
static uint32_t s_frame_id;
static camera_status_msg_t s_status = {
    .backend = ECHOMATE_CAMERA_BACKEND_STUB,
    .state = ECHOMATE_CAMERA_STATE_DISABLED,
};

#if CONFIG_ECHOMATE_ENABLE_CAMERA_DRIVER
static int s_camera_fd = -1;
static camera_mmap_buffer_t s_buffers[CAMERA_BUFFER_COUNT];
static uint32_t s_camera_width;
static uint32_t s_camera_height;
static uint32_t s_camera_fourcc;
static int64_t s_fps_window_start_us;
static uint32_t s_fps_window_frames;
static uint16_t s_r_gain_q8 = 256;
static uint16_t s_b_gain_q8 = 256;
static uint8_t s_r_lut[256];
static uint8_t s_b_lut[256];
static uint16_t s_r_lut_gain_q8;
static uint16_t s_b_lut_gain_q8;
static bool s_color_path_logged;
static uint8_t s_consecutive_errors;
static int64_t s_last_recovery_us;
static uint32_t s_preview_src_x[DIAG_FRAME_WIDTH];
static uint32_t s_preview_src_y[DIAG_FRAME_HEIGHT];
static bool s_preview_maps_ready;
static uint32_t s_convert_window_frames;
static uint32_t s_convert_sum_us;
static uint32_t s_convert_max_us;
#endif

static uint16_t rgb565_from_u8(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static uint8_t clamp_u8(uint32_t value)
{
    return value > 255 ? 255 : (uint8_t)value;
}

static uint32_t abs_diff_u32(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static uint16_t clamp_gain_q8(uint32_t gain)
{
    if (gain < 154U) {
        return 154U;
    }
    if (gain > 563U) {
        return 563U;
    }
    return (uint16_t)gain;
}

static void raw_lut_init(void)
{
    if (s_raw_lut_ready) {
        return;
    }

    const uint32_t black = CONFIG_ECHOMATE_CAMERA_RAW_BLACK_LEVEL;
    const uint32_t range = black < 255 ? 255U - black : 1U;
    for (uint32_t i = 0; i < 256; ++i) {
        if (i <= black) {
            s_raw_lut[i] = 0;
        } else {
            s_raw_lut[i] = clamp_u8(((i - black) * 255U + (range / 2U)) / range);
        }
    }
    s_raw_lut_ready = true;
}

static uint8_t raw_linear_at(const uint8_t *src, size_t index)
{
    return s_raw_lut[src[index]];
}

#if CONFIG_ECHOMATE_ENABLE_CAMERA_DRIVER
static void camera_preview_gain_luts_update(void)
{
    if (s_r_lut_gain_q8 == s_r_gain_q8 && s_b_lut_gain_q8 == s_b_gain_q8) {
        return;
    }

    for (uint32_t i = 0; i < 256; ++i) {
        const uint8_t linear = s_raw_lut[i];
        s_r_lut[i] = clamp_u8(((uint32_t)linear * s_r_gain_q8 + 128U) >> 8);
        s_b_lut[i] = clamp_u8(((uint32_t)linear * s_b_gain_q8 + 128U) >> 8);
    }
    s_r_lut_gain_q8 = s_r_gain_q8;
    s_b_lut_gain_q8 = s_b_gain_q8;
}
#endif

static const camera_bayer_layout_t *camera_bayer_layout(void)
{
#if CONFIG_ECHOMATE_CAMERA_BAYER_RGGB
    static const camera_bayer_layout_t layout = {
        .pattern = ECHOMATE_BAYER_RGGB,
        .label = "RGGB",
        .r_row = 0, .r_col = 0,
        .b_row = 1, .b_col = 1,
        .g0_row = 0, .g0_col = 1,
        .g1_row = 1, .g1_col = 0,
    };
#elif CONFIG_ECHOMATE_CAMERA_BAYER_GRBG
    static const camera_bayer_layout_t layout = {
        .pattern = ECHOMATE_BAYER_GRBG,
        .label = "GRBG",
        .r_row = 0, .r_col = 1,
        .b_row = 1, .b_col = 0,
        .g0_row = 0, .g0_col = 0,
        .g1_row = 1, .g1_col = 1,
    };
#elif CONFIG_ECHOMATE_CAMERA_BAYER_GBRG
    static const camera_bayer_layout_t layout = {
        .pattern = ECHOMATE_BAYER_GBRG,
        .label = "GBRG",
        .r_row = 1, .r_col = 0,
        .b_row = 0, .b_col = 1,
        .g0_row = 0, .g0_col = 0,
        .g1_row = 1, .g1_col = 1,
    };
#else
    static const camera_bayer_layout_t layout = {
        .pattern = ECHOMATE_BAYER_BGGR,
        .label = "BGGR",
        .r_row = 1, .r_col = 1,
        .b_row = 0, .b_col = 0,
        .g0_row = 0, .g0_col = 1,
        .g1_row = 1, .g1_col = 0,
    };
#endif
    return &layout;
}

static uint16_t *diag_write_frame(void)
{
    return s_diag_frames[s_diag_write_index];
}

static esp_err_t camera_port_alloc_diag_frames(void)
{
    raw_lut_init();

    for (size_t i = 0; i < 2; ++i) {
        if (s_diag_frames[i]) {
            continue;
        }

        s_diag_frames[i] = heap_caps_calloc(DIAG_FRAME_PIXELS,
                                            sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_diag_frames[i]) {
            s_diag_frames[i] = heap_caps_calloc(DIAG_FRAME_PIXELS,
                                                sizeof(uint16_t),
                                                MALLOC_CAP_8BIT);
        }
        if (!s_diag_frames[i]) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static void camera_status_set(echomate_camera_backend_t backend,
                              echomate_camera_state_t state,
                              esp_err_t error)
{
    s_status.timestamp_us = esp_timer_get_time();
    s_status.backend = backend;
    s_status.state = state;
    s_status.bayer_pattern = camera_bayer_layout()->pattern;
    s_status.raw_black_level = CONFIG_ECHOMATE_CAMERA_RAW_BLACK_LEVEL;
#if CONFIG_ECHOMATE_ENABLE_CAMERA_DRIVER
    s_status.awb_r_gain_q8 = s_r_gain_q8;
    s_status.awb_b_gain_q8 = s_b_gain_q8;
#else
    s_status.awb_r_gain_q8 = 256;
    s_status.awb_b_gain_q8 = 256;
#endif
    s_status.last_error = error;
}

static void fill_diag_frame_from_bytes(const uint8_t *src, size_t src_len)
{
    uint16_t *dst = diag_write_frame();
    if (!dst) {
        return;
    }

    if (!src || src_len == 0) {
        memset(dst, 0, DIAG_FRAME_PIXELS * sizeof(uint16_t));
        return;
    }

    const size_t frame_bytes = DIAG_FRAME_PIXELS * sizeof(uint16_t);
    const size_t step = src_len > frame_bytes ? src_len / frame_bytes : 1;
    for (size_t i = 0; i < DIAG_FRAME_PIXELS; ++i) {
        const uint8_t gray = src[(i * step) % src_len];
        dst[i] = rgb565_from_u8(gray, gray, gray);
    }
}

#if CONFIG_ECHOMATE_ENABLE_CAMERA_DRIVER
static void fill_diag_frame_from_bggr8(const uint8_t *src, size_t src_len)
{
    uint16_t *dst = diag_write_frame();
    if (!dst) {
        return;
    }

    if (!src || src_len == 0 || s_camera_width == 0 || s_camera_height == 0 ||
        src_len < (size_t)s_camera_width * s_camera_height) {
        fill_diag_frame_from_bytes(src, src_len);
        return;
    }

    if (!s_color_path_logged) {
        ESP_LOGI(TAG, "camera preview color: RAW8 %s black=%d awb=smoothed",
                 camera_bayer_layout()->label,
                 CONFIG_ECHOMATE_CAMERA_RAW_BLACK_LEVEL);
        s_color_path_logged = true;
    }

    const camera_bayer_layout_t *layout = camera_bayer_layout();
    if (!s_preview_maps_ready) {
        for (uint32_t x = 0; x < DIAG_FRAME_WIDTH; ++x) {
            uint32_t src_x = (x * s_camera_width) / DIAG_FRAME_WIDTH;
            src_x &= ~1U;
            if (src_x + 1 >= s_camera_width) {
                src_x = s_camera_width > 1 ? s_camera_width - 2 : 0;
            }
            s_preview_src_x[x] = src_x;
        }
        for (uint32_t y = 0; y < DIAG_FRAME_HEIGHT; ++y) {
            uint32_t src_y = (y * s_camera_height) / DIAG_FRAME_HEIGHT;
            src_y &= ~1U;
            if (src_y + 1 >= s_camera_height) {
                src_y = s_camera_height > 1 ? s_camera_height - 2 : 0;
            }
            s_preview_src_y[y] = src_y;
        }
        s_preview_maps_ready = true;
        ESP_LOGI(TAG, "camera preview maps built: %" PRIu32 "x%" PRIu32 " -> %dx%d",
                 s_camera_width, s_camera_height, DIAG_FRAME_WIDTH, DIAG_FRAME_HEIGHT);
    }

    uint32_t r_sum = 0;
    uint32_t g_sum = 0;
    uint32_t b_sum = 0;
    uint32_t sample_count = 0;

    for (uint32_t sy = 0; sy + 1 < s_camera_height; sy += 32) {
        for (uint32_t sx = 0; sx + 1 < s_camera_width; sx += 32) {
            const size_t row0 = (size_t)sy * s_camera_width;
            const size_t row1 = (size_t)(sy + 1) * s_camera_width;
            b_sum += raw_linear_at(src, (layout->b_row ? row1 : row0) + sx + layout->b_col);
            g_sum += raw_linear_at(src, (layout->g0_row ? row1 : row0) + sx + layout->g0_col);
            g_sum += raw_linear_at(src, (layout->g1_row ? row1 : row0) + sx + layout->g1_col);
            r_sum += raw_linear_at(src, (layout->r_row ? row1 : row0) + sx + layout->r_col);
            sample_count++;
        }
    }

    const uint32_t r_mean = sample_count ? r_sum / sample_count : 128;
    const uint32_t g_mean = sample_count ? g_sum / (sample_count * 2U) : 128;
    const uint32_t b_mean = sample_count ? b_sum / sample_count : 128;
    const uint16_t target_r_gain = clamp_gain_q8((g_mean * 256U) / (r_mean ? r_mean : 1U));
    const uint16_t target_b_gain = clamp_gain_q8((g_mean * 256U) / (b_mean ? b_mean : 1U));
    s_r_gain_q8 = (uint16_t)((s_r_gain_q8 * 7U + target_r_gain + 4U) / 8U);
    s_b_gain_q8 = (uint16_t)((s_b_gain_q8 * 7U + target_b_gain + 4U) / 8U);
    camera_preview_gain_luts_update();

    for (uint32_t y = 0; y < DIAG_FRAME_HEIGHT; ++y) {
        const uint32_t src_y = s_preview_src_y[y];
        const uint32_t src_y_next = src_y + 1U;
        const size_t row0 = (size_t)src_y * s_camera_width;
        const size_t row1 = (size_t)src_y_next * s_camera_width;
        const size_t r_row = layout->r_row ? row1 : row0;
        const size_t b_row = layout->b_row ? row1 : row0;
        const size_t g0_row = layout->g0_row ? row1 : row0;
        const size_t g1_row = layout->g1_row ? row1 : row0;
        const uint32_t dst_row = y * DIAG_FRAME_WIDTH;
        for (uint32_t x = 0; x < DIAG_FRAME_WIDTH; ++x) {
            const uint32_t src_x = s_preview_src_x[x];
            const uint8_t g0 = s_raw_lut[src[g0_row + src_x + layout->g0_col]];
            const uint8_t g1 = s_raw_lut[src[g1_row + src_x + layout->g1_col]];
            const uint8_t r = s_r_lut[src[r_row + src_x + layout->r_col]];
            uint8_t g = (uint8_t)(((uint16_t)g0 + g1) / 2);
            const uint8_t b = s_b_lut[src[b_row + src_x + layout->b_col]];

            dst[dst_row + x] = rgb565_from_u8(r, g, b);
        }
    }
}
#endif

static void fill_stub_frame(uint32_t id)
{
    uint16_t *dst = diag_write_frame();
    if (!dst) {
        return;
    }

    for (uint32_t y = 0; y < DIAG_FRAME_HEIGHT; ++y) {
        for (uint32_t x = 0; x < DIAG_FRAME_WIDTH; ++x) {
            const uint8_t r = (uint8_t)((x + id) & 0xff);
            const uint8_t g = (uint8_t)(((y * 2) + id) & 0xff);
            const uint8_t b = (uint8_t)(((x + y) + id) & 0xff);
            dst[y * DIAG_FRAME_WIDTH + x] = rgb565_from_u8(r, g, b);
        }
    }
}

#if CONFIG_ECHOMATE_ENABLE_CAMERA_DRIVER
static void camera_port_reset_real_state(void)
{
    s_camera_width = 0;
    s_camera_height = 0;
    s_camera_fourcc = 0;
    s_fps_window_start_us = 0;
    s_fps_window_frames = 0;
    s_r_gain_q8 = 256;
    s_b_gain_q8 = 256;
    s_r_lut_gain_q8 = 0;
    s_b_lut_gain_q8 = 0;
    s_color_path_logged = false;
    s_preview_maps_ready = false;
    s_convert_window_frames = 0;
    s_convert_sum_us = 0;
    s_convert_max_us = 0;
}

static void camera_port_close_real(void)
{
    if (s_camera_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)ioctl(s_camera_fd, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < CAMERA_BUFFER_COUNT; ++i) {
            if (s_buffers[i].ptr) {
                (void)munmap(s_buffers[i].ptr, s_buffers[i].length);
                s_buffers[i].ptr = NULL;
                s_buffers[i].length = 0;
            }
        }
        close(s_camera_fd);
        s_camera_fd = -1;
    }
    camera_port_reset_real_state();
}

static uint64_t camera_format_score(uint32_t width, uint32_t height)
{
    const uint32_t target_w = CONFIG_ECHOMATE_CAMERA_TARGET_WIDTH;
    const uint32_t target_h = CONFIG_ECHOMATE_CAMERA_TARGET_HEIGHT;
    const uint64_t area = (uint64_t)width * height;
    const uint64_t target_area = (uint64_t)target_w * target_h;
    const uint64_t area_penalty = area > target_area ? (area - target_area) / 8U
                                                      : (target_area - area) / 32U;

    return (uint64_t)abs_diff_u32(width, target_w) * 2048U +
           (uint64_t)abs_diff_u32(height, target_h) * 2048U +
           area_penalty;
}

static void camera_port_consider_candidate(camera_format_candidate_t *best,
                                           uint32_t pixelformat,
                                           uint32_t width,
                                           uint32_t height)
{
    if (!width || !height) {
        return;
    }

    const uint64_t score = camera_format_score(width, height);
    ESP_LOGI(TAG, "camera candidate: %" PRIu32 "x%" PRIu32 " fourcc=0x%08" PRIx32 " score=%" PRIu64,
             width, height, pixelformat, score);

    if (!best->valid || score < best->score) {
        best->width = width;
        best->height = height;
        best->pixelformat = pixelformat;
        best->score = score;
        best->valid = true;
    }
}

static esp_err_t camera_port_choose_format(int fd, struct v4l2_format *format)
{
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    camera_format_candidate_t best = {0};

    memset(format, 0, sizeof(*format));
    format->type = type;

    ESP_LOGI(TAG, "camera target: %dx%d @ %d fps",
             CONFIG_ECHOMATE_CAMERA_TARGET_WIDTH,
             CONFIG_ECHOMATE_CAMERA_TARGET_HEIGHT,
             CONFIG_ECHOMATE_CAMERA_FPS);

    for (int index = 0;; ++index) {
        struct v4l2_fmtdesc desc = {
            .index = index,
            .type = type,
        };
        if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) {
            break;
        }

        ESP_LOGI(TAG, "camera fmt[%d]: fourcc=0x%08" PRIx32 " desc=%s",
                 index, desc.pixelformat, desc.description);

        bool found_size = false;
        for (int size_index = 0;; ++size_index) {
            struct v4l2_frmsizeenum size = {
                .index = size_index,
                .pixel_format = desc.pixelformat,
            };

            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) != 0) {
                break;
            }

            found_size = true;
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                camera_port_consider_candidate(&best, desc.pixelformat,
                                               size.discrete.width,
                                               size.discrete.height);
            } else {
                camera_port_consider_candidate(&best, desc.pixelformat,
                                               CONFIG_ECHOMATE_CAMERA_TARGET_WIDTH,
                                               CONFIG_ECHOMATE_CAMERA_TARGET_HEIGHT);
                break;
            }
        }

        if (!found_size) {
            struct v4l2_format current = {
                .type = type,
            };
            if (ioctl(fd, VIDIOC_G_FMT, &current) == 0) {
                camera_port_consider_candidate(&best, desc.pixelformat,
                                               current.fmt.pix.width,
                                               current.fmt.pix.height);
            } else {
                camera_port_consider_candidate(&best, desc.pixelformat,
                                               CONFIG_ECHOMATE_CAMERA_TARGET_WIDTH,
                                               CONFIG_ECHOMATE_CAMERA_TARGET_HEIGHT);
            }
        }
    }

    if (best.valid) {
        format->fmt.pix.width = best.width;
        format->fmt.pix.height = best.height;
        format->fmt.pix.pixelformat = best.pixelformat;
        ESP_LOGI(TAG, "camera chosen candidate: %" PRIu32 "x%" PRIu32 " fourcc=0x%08" PRIx32,
                 best.width, best.height, best.pixelformat);
        return ESP_OK;
    }

    return ioctl(fd, VIDIOC_G_FMT, format) == 0 ? ESP_OK : ESP_FAIL;
}

static void camera_port_try_set_fps(int fd)
{
    struct v4l2_streamparm parm = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };

    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = CONFIG_ECHOMATE_CAMERA_FPS;
    if (ioctl(fd, VIDIOC_S_PARM, &parm) != 0) {
        ESP_LOGW(TAG, "VIDIOC_S_PARM fps=%d failed, errno=%d",
                 CONFIG_ECHOMATE_CAMERA_FPS, errno);
        return;
    }

    ESP_LOGI(TAG, "camera stream fps request accepted: %u/%u s per frame",
             parm.parm.capture.timeperframe.numerator,
             parm.parm.capture.timeperframe.denominator);
}

static esp_err_t camera_port_start_real(void)
{
    esp_err_t ret = ESP_OK;
    struct v4l2_capability capability = {0};
    struct v4l2_format format = {0};
    struct v4l2_requestbuffers req = {0};
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    camera_status_set(ECHOMATE_CAMERA_BACKEND_MIPI_CSI, ECHOMATE_CAMERA_STATE_INIT, ESP_OK);

    ret = bsp_camera_start(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BSP camera start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_camera_fd = open(BSP_CAMERA_DEVICE, O_RDWR);
    if (s_camera_fd < 0) {
        ESP_LOGW(TAG, "open %s failed, errno=%d", BSP_CAMERA_DEVICE, errno);
        return ESP_FAIL;
    }

    if (ioctl(s_camera_fd, VIDIOC_QUERYCAP, &capability) != 0) {
        ESP_LOGW(TAG, "VIDIOC_QUERYCAP failed, errno=%d", errno);
        ret = ESP_FAIL;
        goto fail;
    }
    ESP_LOGI(TAG, "camera driver=%s card=%s bus=%s caps=0x%08" PRIx32,
             capability.driver, capability.card, capability.bus_info, capability.capabilities);

    ret = camera_port_choose_format(s_camera_fd, &format);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "camera format selection failed");
        goto fail;
    }

    if (ioctl(s_camera_fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGW(TAG, "VIDIOC_S_FMT failed, errno=%d", errno);
        ret = ESP_FAIL;
        goto fail;
    }

    camera_port_try_set_fps(s_camera_fd);

    s_camera_width = format.fmt.pix.width;
    s_camera_height = format.fmt.pix.height;
    s_camera_fourcc = format.fmt.pix.pixelformat;
    ESP_LOGI(TAG, "camera selected: %" PRIu32 "x%" PRIu32 " fourcc=0x%08" PRIx32,
             s_camera_width, s_camera_height, s_camera_fourcc);

    req.count = CAMERA_BUFFER_COUNT;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_camera_fd, VIDIOC_REQBUFS, &req) != 0 || req.count < CAMERA_BUFFER_COUNT) {
        ESP_LOGW(TAG, "VIDIOC_REQBUFS failed, errno=%d count=%" PRIu32, errno, req.count);
        ret = ESP_FAIL;
        goto fail;
    }

    for (int i = 0; i < CAMERA_BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf = {
            .type = type,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(s_camera_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_QUERYBUF[%d] failed, errno=%d", i, errno);
            ret = ESP_FAIL;
            goto fail;
        }

        s_buffers[i].ptr = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, s_camera_fd, buf.m.offset);
        s_buffers[i].length = buf.length;
        if (s_buffers[i].ptr == MAP_FAILED) {
            ESP_LOGW(TAG, "mmap[%d] failed", i);
            s_buffers[i].ptr = NULL;
            s_buffers[i].length = 0;
            ret = ESP_FAIL;
            goto fail;
        }

        if (ioctl(s_camera_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_QBUF[%d] failed, errno=%d", i, errno);
            ret = ESP_FAIL;
            goto fail;
        }
    }

    if (ioctl(s_camera_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGW(TAG, "VIDIOC_STREAMON failed, errno=%d", errno);
        ret = ESP_FAIL;
        goto fail;
    }

    s_fps_window_start_us = esp_timer_get_time();
    s_fps_window_frames = 0;
    s_status.width = (uint16_t)s_camera_width;
    s_status.height = (uint16_t)s_camera_height;
    s_status.fourcc = s_camera_fourcc;
    camera_status_set(ECHOMATE_CAMERA_BACKEND_MIPI_CSI, ECHOMATE_CAMERA_STATE_OK, ESP_OK);
    s_consecutive_errors = 0;
    ESP_LOGI(TAG, "MIPI-CSI camera stream started");
    return ESP_OK;

fail:
    camera_port_close_real();
    return ret;
}

static void camera_port_try_recover(const char *stage)
{
    const int64_t now = esp_timer_get_time();

    if (s_consecutive_errors < CAMERA_RECOVERY_ERROR_LIMIT ||
        now - s_last_recovery_us < CAMERA_RECOVERY_COOLDOWN_US) {
        return;
    }

    ESP_LOGW(TAG, "camera recovery: restarting stream after %u consecutive errors at %s",
             s_consecutive_errors,
             stage);
    s_last_recovery_us = now;
    camera_port_close_real();

    esp_err_t restart_ret = camera_port_start_real();
    if (restart_ret == ESP_OK) {
        ESP_LOGI(TAG, "camera recovery: stream restarted");
        return;
    }

    camera_status_set(ECHOMATE_CAMERA_BACKEND_STUB, ECHOMATE_CAMERA_STATE_ERROR, restart_ret);
    ESP_LOGW(TAG, "camera recovery: restart failed (%s), using stub preview until next reset",
             esp_err_to_name(restart_ret));
}

static void camera_port_mark_real_error(const char *stage, esp_err_t err)
{
    s_status.dropped_frames++;
    s_consecutive_errors++;
    camera_status_set(ECHOMATE_CAMERA_BACKEND_MIPI_CSI, ECHOMATE_CAMERA_STATE_ERROR, err);
    camera_port_try_recover(stage);
}
#endif

esp_err_t camera_port_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    ESP_RETURN_ON_ERROR(camera_port_alloc_diag_frames(), TAG, "allocate camera preview buffers failed");

#if CONFIG_ECHOMATE_ENABLE_CAMERA_DRIVER
    esp_err_t ret = camera_port_start_real();
    if (ret == ESP_OK) {
        return ESP_OK;
    }

    camera_status_set(ECHOMATE_CAMERA_BACKEND_STUB, ECHOMATE_CAMERA_STATE_ERROR, ret);
    ESP_LOGW(TAG, "falling back to stub camera frames after real camera failure");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "using stub camera frames");
    camera_status_set(ECHOMATE_CAMERA_BACKEND_STUB, ECHOMATE_CAMERA_STATE_DISABLED, ESP_OK);
    return ESP_OK;
#endif
}

esp_err_t camera_port_capture(camera_frame_msg_t *frame)
{
    if (!frame) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t *ready_frame = diag_write_frame();
    if (!ready_frame) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t id = ++s_frame_id;

#if CONFIG_ECHOMATE_ENABLE_CAMERA_DRIVER
    if (s_camera_fd >= 0) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(s_camera_fd, VIDIOC_DQBUF, &buf) == 0) {
            const bool frame_done = (buf.flags & V4L2_BUF_FLAG_DONE) != 0;
            bool capture_ok = false;
            if (frame_done && buf.index < CAMERA_BUFFER_COUNT && s_buffers[buf.index].ptr) {
                const size_t used = buf.bytesused ? buf.bytesused : s_buffers[buf.index].length;
                const int64_t convert_start_us = esp_timer_get_time();
                fill_diag_frame_from_bggr8(s_buffers[buf.index].ptr, used);
                const uint32_t convert_us = (uint32_t)(esp_timer_get_time() - convert_start_us);
                s_convert_window_frames++;
                s_convert_sum_us += convert_us;
                if (convert_us > s_convert_max_us) {
                    s_convert_max_us = convert_us;
                }
                s_status.frame_count++;
                s_fps_window_frames++;
                s_consecutive_errors = 0;
                capture_ok = true;

                const int64_t now = esp_timer_get_time();
                const int64_t elapsed = now - s_fps_window_start_us;
                if (elapsed >= CAMERA_STATUS_PERIOD_US) {
                    s_status.fps_x10 = (uint16_t)((s_fps_window_frames * 10000000ULL) / (uint64_t)elapsed);
                    const uint32_t avg_convert_us = s_convert_window_frames ?
                        s_convert_sum_us / s_convert_window_frames : 0;
                    s_fps_window_start_us = now;
                    s_fps_window_frames = 0;
                    ESP_LOGI(TAG, "camera fps=%u.%u frame=%" PRIu32 " size=%" PRIu32 "x%" PRIu32 " bytes=%" PRIu32 " cvt_avg=%" PRIu32 "us cvt_max=%" PRIu32 "us",
                             s_status.fps_x10 / 10, s_status.fps_x10 % 10,
                             s_status.frame_count, s_camera_width, s_camera_height, buf.bytesused,
                             avg_convert_us, s_convert_max_us);
                    s_convert_window_frames = 0;
                    s_convert_sum_us = 0;
                    s_convert_max_us = 0;
                }
            } else {
                s_status.dropped_frames++;
                s_consecutive_errors++;
                camera_status_set(ECHOMATE_CAMERA_BACKEND_MIPI_CSI,
                                  ECHOMATE_CAMERA_STATE_ERROR,
                                  ESP_ERR_INVALID_RESPONSE);
                fill_diag_frame_from_bytes(NULL, 0);
            }

            if (ioctl(s_camera_fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGW(TAG, "VIDIOC_QBUF after capture failed, errno=%d", errno);
                camera_port_mark_real_error("VIDIOC_QBUF", ESP_FAIL);
            } else if (capture_ok) {
                camera_status_set(ECHOMATE_CAMERA_BACKEND_MIPI_CSI, ECHOMATE_CAMERA_STATE_OK, ESP_OK);
            } else {
                camera_port_try_recover("frame flags");
            }
        } else {
            ESP_LOGW(TAG, "VIDIOC_DQBUF failed, errno=%d", errno);
            camera_port_mark_real_error("VIDIOC_DQBUF", ESP_FAIL);
            fill_diag_frame_from_bytes(NULL, 0);
        }
    } else
#endif
    {
        fill_stub_frame(id);
        s_status.frame_count++;
        s_status.width = DIAG_FRAME_WIDTH;
        s_status.height = DIAG_FRAME_HEIGHT;
        s_status.fourcc = 0;
        if (s_status.state != ECHOMATE_CAMERA_STATE_ERROR) {
            camera_status_set(ECHOMATE_CAMERA_BACKEND_STUB, ECHOMATE_CAMERA_STATE_DISABLED, ESP_OK);
        } else {
            s_status.timestamp_us = esp_timer_get_time();
        }
    }

    frame->frame_id = id;
    frame->timestamp_us = esp_timer_get_time();
    frame->width = DIAG_FRAME_WIDTH;
    frame->height = DIAG_FRAME_HEIGHT;
    frame->format = ECHOMATE_PIXEL_RGB565;
    frame->buffer = (const uint8_t *)ready_frame;
    frame->length = DIAG_FRAME_PIXELS * sizeof(uint16_t);

    s_diag_write_index = (uint8_t)((s_diag_write_index + 1U) % 2U);

    return ESP_OK;
}

esp_err_t camera_port_get_status(camera_status_msg_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    *status = s_status;
    status->timestamp_us = esp_timer_get_time();
    return ESP_OK;
}
