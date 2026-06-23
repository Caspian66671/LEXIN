#include "lexin_display_test.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "lexin_actions.h"
#include "lexin_interaction.h"
#include "lexin_launcher.h"
#include "lexin_triggers.h"

static const char *TAG = "screen_ui";

LV_FONT_DECLARE(lexin_cn_20);
LV_FONT_DECLARE(lexin_cn_28);

#define LCD_H_RES 1024
#define LCD_V_RES 600
#define LCD_DSI_LANES 2
#define LCD_PHY_LDO_CHAN 3
#define LCD_PHY_LDO_VOLTAGE_MV 2500
#define LCD_BACKLIGHT_GPIO GPIO_NUM_22
#define LCD_BACKLIGHT_ON_LEVEL 1
#define LCD_RESET_GPIO GPIO_NUM_5

#define TOUCH_I2C_SDA GPIO_NUM_7
#define TOUCH_I2C_SCL GPIO_NUM_8
#define TOUCH_I2C_FREQ_HZ 400000
#define TOUCH_POLL_MS 40
#define TOUCH_RELEASE_MS 280
#define EMOTION_PREVIEW_PIXELS \
    (LEXIN_VISION_PREVIEW_WIDTH * LEXIN_VISION_PREVIEW_HEIGHT)
#define EMOTION_PREVIEW_X 20
#define EMOTION_PREVIEW_Y 58
#define RESULT_CACHE_SIZE 1024

static esp_ldo_channel_handle_t s_mipi_phy_ldo;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_lvgl_disp;
static bool s_lvgl_ready;
static const char *s_pet_state = "待机中";
static const char *s_pet_tip = "学习前先喝水";
static const char *s_pet_reason = "天气和日程待更新";
static const char *s_pet_model_tip = "已准备好";
static uint32_t s_pet_accent = 0x1c98d2;
static int s_pet_service_count;
static lexin_action_id_t s_pet_last_action = LEXIN_ACTION_TIME;
static char s_pet_weather_summary[96] = "天气待更新";
static char s_pet_calendar_summary[96] = "日程待更新";
static char s_pet_combined_reason[256] = "天气和日程待更新";
static char s_pet_combined_tip[256] = "学习前先喝水";
static char s_pet_edge_summary[128] = "等待推理";
static char s_pet_edge_meta[96] = "ESP-DL 21维  置信度--";
static char s_pet_cloud_summary[128] = "未接入";
static char s_pet_cloud_meta[96] = "等待云端模型";
static const char *s_pet_weather_scene = "天气待更新";
static const char *s_pet_time_scene = "日程待更新";
static const char *s_pet_emotion_scene = "状态待更新";
static lexin_emotion_state_t s_emotion_state = LEXIN_EMOTION_UNKNOWN;
static int64_t s_last_vision_page_refresh_ms;
static uint16_t *s_emotion_preview_pixels;
static uint16_t *s_emotion_preview_back_pixels;
static uint32_t s_emotion_preview_frame_id;
static lv_image_dsc_t s_emotion_preview_dsc;
static lv_obj_t *s_emotion_preview_image;
static lv_obj_t *s_emotion_waiting_label;
static lv_obj_t *s_emotion_face_box;
static lv_obj_t *s_emotion_camera_meta_label;
static lv_obj_t *s_emotion_expression_label;
static lv_obj_t *s_emotion_meta_label;
static lv_obj_t *s_emotion_system_meta_label;
static lv_obj_t *s_emotion_response_label;
static lv_obj_t *s_emotion_online_label;
static char s_cached_weather[RESULT_CACHE_SIZE];
static char s_cached_calendar[RESULT_CACHE_SIZE];

static void copy_field_value(const char *text, const char *key, char *out, size_t out_size);
static int parse_percent_value(const char *text);
static int parse_hour_value(const char *time_value);
static void refresh_pet_combined_tip(void);
static bool lvgl_show_pet_ai_page(void);
static bool lvgl_show_suggestion_page(void);
static bool lvgl_show_emotion_page(void);
static bool lvgl_refresh_emotion_live(const lexin_vision_snapshot_t *snapshot);
static void update_pet_from_ai_context(void);

static void lvgl_set_bg(lv_obj_t *obj, uint32_t color)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

static void lvgl_set_vertical_gradient(lv_obj_t *obj, uint32_t top, uint32_t bottom)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

static lv_obj_t *lvgl_label(lv_obj_t *parent, const char *text, int x, int y,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static void lvgl_label_width(lv_obj_t *label, int width)
{
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

static lv_obj_t *lvgl_center_label(lv_obj_t *parent, const char *text, int x, int y, int width,
                                   const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lvgl_label(parent, text, x, y, font, color);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static lv_obj_t *lvgl_card(lv_obj_t *parent, int x, int y, int w, int h,
                           uint32_t color, int radius)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lvgl_set_bg(card, color);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    return card;
}

static lv_obj_t *lvgl_glass_card(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_80, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x2d6f9d), 0);
    lv_obj_set_style_shadow_width(card, 18, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(card, 10, 0);
    return card;
}

static void lvgl_card_border(lv_obj_t *card, uint32_t color, int width)
{
    lv_obj_set_style_border_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(card, width, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
}

static void copy_field_value(const char *text, const char *key, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    snprintf(out, out_size, "UNKNOWN");
    if (text == NULL || key == NULL) {
        return;
    }

    const char *p = strstr(text, key);
    if (p == NULL) {
        return;
    }
    p += strlen(key);
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    size_t i = 0;
    while (*p != '\0' && *p != '\r' && *p != '\n' && i + 1 < out_size) {
        unsigned char raw = (unsigned char)*p++;
        if (raw < 0x80) {
            out[i++] = (char)toupper(raw);
        }
    }
    while (i > 0 && out[i - 1] == ' ') {
        i--;
    }
    out[i] = '\0';
    if (i == 0) {
        snprintf(out, out_size, "UNKNOWN");
    }
}

static char ascii_upper_char(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static bool ascii_contains_ci(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }

    for (const char *p = text; *p != '\0'; p++) {
        const char *a = p;
        const char *b = needle;
        while (*a != '\0' && *b != '\0' && ascii_upper_char(*a) == ascii_upper_char(*b)) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bool field_has_value(const char *value)
{
    return value != NULL && value[0] != '\0' && !ascii_contains_ci(value, "UNKNOWN");
}

static const char *weather_to_cn(const char *weather)
{
    if (ascii_contains_ci(weather, "SUN") || ascii_contains_ci(weather, "CLEAR")) {
        return "晴";
    }
    if (ascii_contains_ci(weather, "CLOUD")) {
        return "多云";
    }
    if (ascii_contains_ci(weather, "OVERCAST") || ascii_contains_ci(weather, "YIN")) {
        return "阴";
    }
    if (ascii_contains_ci(weather, "SNOW")) {
        return "雪";
    }
    if (ascii_contains_ci(weather, "RAIN") || ascii_contains_ci(weather, "SHOWER")) {
        return "雨";
    }
    if (ascii_contains_ci(weather, "THUNDER") || ascii_contains_ci(weather, "STORM")) {
        return "雷雨";
    }
    if (ascii_contains_ci(weather, "FOG") || ascii_contains_ci(weather, "HAZE")) {
        return "雾";
    }
    return "未知";
}

static const char *weather_pet_suggestion(const char *weather, const char *rain, const char *advice)
{
    int rain_percent = parse_percent_value(rain);
    if (ascii_contains_ci(advice, "UMBRELLA") || ascii_contains_ci(weather, "RAIN") || rain_percent >= 50) {
        return "降雨较高记得带伞";
    }
    if (ascii_contains_ci(advice, "HOT") || ascii_contains_ci(advice, "COOL") ||
        ascii_contains_ci(advice, "SUN")) {
        return "阳光较强注意防晒";
    }
    if (ascii_contains_ci(advice, "COLD") || ascii_contains_ci(advice, "WARM")) {
        return "气温偏低注意保暖";
    }
    if (ascii_contains_ci(advice, "CHECK") || ascii_contains_ci(advice, "NETWORK")) {
        return "网络异常稍后再试";
    }
    return "天气不错适合出门";
}

static void refresh_pet_combined_tip(void)
{
    snprintf(s_pet_combined_reason, sizeof(s_pet_combined_reason), "%s  %s  %s",
             s_pet_weather_scene, s_pet_time_scene, s_pet_emotion_scene);
    snprintf(s_pet_combined_tip, sizeof(s_pet_combined_tip), "%s", s_pet_tip);
}

static void update_pet_from_ai_context(void)
{
    switch (s_emotion_state) {
    case LEXIN_EMOTION_HAPPY:
        s_pet_emotion_scene = "陪伴状态积极";
        s_pet_state = "运行良好";
        s_pet_tip = "先专注学习一小时";
        s_pet_accent = 0x1c98d2;
        break;
    case LEXIN_EMOTION_TIRED:
        s_pet_emotion_scene = "陪伴状态疲惫";
        s_pet_state = "风险提醒";
        s_pet_tip = "休息五分钟再继续";
        s_pet_accent = 0xff9f22;
        break;
    case LEXIN_EMOTION_FOCUSED:
        s_pet_emotion_scene = "专注推进中";
        s_pet_state = "重点推进";
        s_pet_tip = "先专注学习一小时";
        s_pet_accent = 0x2f86ff;
        break;
    case LEXIN_EMOTION_NEUTRAL:
        s_pet_emotion_scene = "陪伴状态平稳";
        break;
    default:
        s_pet_emotion_scene = "状态待更新";
        break;
    }
    refresh_pet_combined_tip();
}

static int parse_percent_value(const char *text)
{
    if (text == NULL) {
        return -1;
    }
    int value = 0;
    bool seen_digit = false;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            seen_digit = true;
            value = value * 10 + (*p - '0');
        } else if (seen_digit) {
            break;
        }
    }
    return seen_digit ? value : -1;
}

static int parse_int_value(const char *text, int fallback)
{
    if (text == NULL) {
        return fallback;
    }
    int sign = 1;
    int value = 0;
    bool seen_digit = false;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '-' && !seen_digit) {
            sign = -1;
        } else if (*p >= '0' && *p <= '9') {
            seen_digit = true;
            value = value * 10 + (*p - '0');
        } else if (seen_digit) {
            break;
        }
    }
    return seen_digit ? value * sign : fallback;
}

static const char *weather_feel_text(const char *temp_value, const char *advice)
{
    int temp_c = parse_int_value(temp_value, 24);
    if (ascii_contains_ci(advice, "HOT") || temp_c >= 30) {
        return "体感偏热";
    }
    if (ascii_contains_ci(advice, "COLD") || temp_c <= 8) {
        return "注意保暖";
    }
    return "体感舒适";
}

static const char *weather_outdoor_text(const char *weather, const char *rain, const char *advice)
{
    int rain_percent = parse_percent_value(rain);
    if (ascii_contains_ci(weather, "RAIN") || rain_percent >= 50) {
        return "出门带伞";
    }
    if (ascii_contains_ci(advice, "HOT") || ascii_contains_ci(advice, "SUN")) {
        return "少晒多补水";
    }
    return "适合短时出门";
}

static const char *weather_study_text(const char *weather, const char *rain, const char *advice)
{
    int rain_percent = parse_percent_value(rain);
    if (ascii_contains_ci(weather, "RAIN") || rain_percent >= 50) {
        return "室内学习更稳";
    }
    if (ascii_contains_ci(advice, "HOT") || ascii_contains_ci(advice, "SUN")) {
        return "补水后专注学习";
    }
    return "适合专注学习";
}

static int parse_hour_value(const char *time_value)
{
    if (time_value == NULL || time_value[0] == '\0') {
        return -1;
    }
    int hour = 0;
    bool seen_digit = false;
    for (const char *p = time_value; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            seen_digit = true;
            hour = hour * 10 + (*p - '0');
            if (hour > 23) {
                return -1;
            }
        } else if (*p == ':' && seen_digit) {
            return hour;
        } else if (seen_digit) {
            break;
        }
    }
    return seen_digit ? hour : -1;
}

static bool parse_date_parts(const char *date, int *year, int *month, int *day)
{
    int y = 0;
    int m = 0;
    int d = 0;
    if (date == NULL || sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) {
        return false;
    }
    if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    *year = y;
    *month = m;
    *day = d;
    return true;
}

static bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

static bool valid_date_parts(int year, int month, int day)
{
    return year >= 2000 && month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
}

static int weekday_monday0(int year, int month, int day)
{
    if (month < 3) {
        month += 12;
        year--;
    }
    int k = year % 100;
    int j = year / 100;
    int h = (day + (13 * (month + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    int sunday0 = (h + 6) % 7;
    return (sunday0 + 6) % 7;
}

static const char *month_name_cn(int month)
{
    static const char *names[] = {
        "一月", "二月", "三月", "四月", "五月", "六月",
        "七月", "八月", "九月", "十月", "十一月", "十二月",
    };
    if (month < 1 || month > 12) {
        return "五月";
    }
    return names[month - 1];
}

static bool calendar_rest_day(const char *holiday_cn, const char *day_type)
{
    return (holiday_cn != NULL && strcmp(holiday_cn, "无") != 0) ||
           ascii_contains_ci(day_type, "HOLIDAY") ||
           ascii_contains_ci(day_type, "WEEKEND");
}

static const char *calendar_pet_suggestion(const char *time_value, const char *holiday_cn, const char *day_type)
{
    int hour = parse_hour_value(time_value);
    if (calendar_rest_day(holiday_cn, day_type)) {
        if (hour >= 0 && hour < 7) {
            return "休息日早点睡";
        }
        if (hour >= 7 && hour < 11) {
            return "休息日先活动身体";
        }
        if (hour >= 11 && hour < 14) {
            return "好好吃饭再散步";
        }
        if (hour >= 14 && hour < 18) {
            return "轻松读点论文";
        }
        if (hour >= 18 && hour < 22) {
            return "放松复盘明日计划";
        }
        return "早点休息养状态";
    }
    if (hour >= 0 && hour < 6) {
        return "夜深了早点休息";
    }
    if (hour >= 6 && hour < 9) {
        return "早餐后专注学习";
    }
    if (hour >= 9 && hour < 11) {
        return "先读论文做笔记";
    }
    if (hour >= 11 && hour < 14) {
        return "到点吃饭别硬扛";
    }
    if (hour >= 14 && hour < 17) {
        return "下午专注学习";
    }
    if (hour >= 17 && hour < 19) {
        return "晚饭后散步放松";
    }
    if (hour >= 19 && hour < 22) {
        return "复盘论文笔记";
    }
    return "早点休息养状态";
}

static void update_pet_tip_from_weather(const char *weather, const char *rain, const char *advice)
{
    s_pet_service_count++;
    s_pet_last_action = LEXIN_ACTION_WEATHER;
    int rain_percent = parse_percent_value(rain);
    const char *weather_cn = weather_to_cn(weather);
    const char *suggestion_cn = weather_pet_suggestion(weather, rain, advice);
    snprintf(s_pet_weather_summary, sizeof(s_pet_weather_summary), "天气%s，降雨%d%%",
             weather_cn, rain_percent >= 0 ? rain_percent : 0);
    if (ascii_contains_ci(advice, "UMBRELLA") || ascii_contains_ci(weather, "RAIN") || rain_percent >= 50) {
        s_pet_state = "关心提醒";
        s_pet_tip = "雨天记得带伞";
        s_pet_reason = "天气和降雨概率";
        s_pet_weather_scene = "可能下雨";
        s_pet_accent = 0x2f86ff;
    } else if (ascii_contains_ci(advice, "HOT") || ascii_contains_ci(advice, "COOL") ||
               ascii_contains_ci(advice, "SUN")) {
        s_pet_state = "关心提醒";
        s_pet_tip = "天气热注意防晒";
        s_pet_reason = "天气和出行建议";
        s_pet_weather_scene = "天气炎热";
        s_pet_accent = 0xff9f22;
    } else if (ascii_contains_ci(advice, "COLD") || ascii_contains_ci(advice, "WARM")) {
        s_pet_state = "关心提醒";
        s_pet_tip = "天气冷注意保暖";
        s_pet_reason = "天气和出行建议";
        s_pet_weather_scene = "天气偏冷";
        s_pet_accent = 0x5f75ff;
    } else {
        s_pet_state = "开心";
        s_pet_tip = "天气稳定适合学习";
        s_pet_reason = "天气和降雨概率";
        s_pet_weather_scene = strcmp(weather_cn, "未知") == 0 ? "天气待更新" : "天气稳定";
        s_pet_accent = 0x1c98d2;
    }
    s_pet_model_tip = suggestion_cn;
    refresh_pet_combined_tip();
}

static void update_pet_tip_from_calendar(const char *time_value, const char *holiday_cn, const char *day_type)
{
    s_pet_service_count++;
    s_pet_last_action = LEXIN_ACTION_TIME;
    int hour = parse_hour_value(time_value);
    const char *suggestion_cn = calendar_pet_suggestion(time_value, holiday_cn, day_type);
    const bool rest_day = calendar_rest_day(holiday_cn, day_type);
    snprintf(s_pet_calendar_summary, sizeof(s_pet_calendar_summary), "时间%s，节假日%s",
             time_value != NULL && time_value[0] != '\0' ? time_value : "未知",
             holiday_cn != NULL ? holiday_cn : "未知");
    if (rest_day) {
        s_pet_state = "休息提醒";
        s_pet_tip = suggestion_cn;
        s_pet_reason = "日期类型";
        s_pet_time_scene = "休息节奏";
        s_pet_accent = 0xff9f22;
    } else if (hour >= 0 && hour < 6) {
        s_pet_state = "关心";
        s_pet_tip = suggestion_cn;
        s_pet_reason = "北京时间";
        s_pet_time_scene = "夜间";
        s_pet_accent = 0x5f75ff;
    } else if (hour >= 6 && hour < 9) {
        s_pet_state = "健康提醒";
        s_pet_tip = suggestion_cn;
        s_pet_reason = "北京时间";
        s_pet_time_scene = "早间";
        s_pet_accent = 0x1c98d2;
    } else if (hour >= 9 && hour < 11) {
        s_pet_state = "学习提醒";
        s_pet_tip = suggestion_cn;
        s_pet_reason = "北京时间";
        s_pet_time_scene = "论文阅读";
        s_pet_accent = 0x1c98d2;
    } else if (hour >= 11 && hour < 14) {
        s_pet_state = "健康提醒";
        s_pet_tip = suggestion_cn;
        s_pet_reason = "北京时间";
        s_pet_time_scene = "午间";
        s_pet_accent = 0xff9f22;
    } else if (hour >= 14 && hour < 17) {
        s_pet_state = "学习提醒";
        s_pet_tip = suggestion_cn;
        s_pet_reason = "北京时间";
        s_pet_time_scene = "专注学习";
        s_pet_accent = 0x1c98d2;
    } else if (hour >= 17 && hour < 19) {
        s_pet_state = "健康提醒";
        s_pet_tip = suggestion_cn;
        s_pet_reason = "北京时间";
        s_pet_time_scene = "晚间运动";
        s_pet_accent = 0xff9f22;
    } else if (hour >= 19 && hour < 22) {
        s_pet_state = "复盘提醒";
        s_pet_tip = suggestion_cn;
        s_pet_reason = "北京时间";
        s_pet_time_scene = "晚间复盘";
        s_pet_accent = 0x9465ff;
    } else {
        s_pet_state = "关心";
        s_pet_tip = suggestion_cn;
        s_pet_reason = "时间和节假日";
        s_pet_time_scene = "休息";
        s_pet_accent = 0x5f75ff;
    }
    s_pet_model_tip = suggestion_cn;
    refresh_pet_combined_tip();
}

static const char *enterprise_insight_tip(const char *insight)
{
    if (ascii_contains_ci(insight, "UMBRELLA") || ascii_contains_ci(insight, "RISK")) {
        return "出门带伞，路上慢点";
    }
    if (ascii_contains_ci(insight, "BREAKFAST")) {
        return "先吃早餐再进组";
    }
    if (ascii_contains_ci(insight, "LUNCH")) {
        return "到点吃饭，别硬扛";
    }
    if (ascii_contains_ci(insight, "DINNER")) {
        return "先吃晚饭再学习";
    }
    if (ascii_contains_ci(insight, "RESEARCH_FOCUS")) {
        return "先专注学习一小时";
    }
    if (ascii_contains_ci(insight, "PAPER_READING")) {
        return "读一篇核心论文";
    }
    if (ascii_contains_ci(insight, "EXPERIMENT")) {
        return "整理学习笔记";
    }
    if (ascii_contains_ci(insight, "WRITE_THESIS")) {
        return "复盘论文要点";
    }
    if (ascii_contains_ci(insight, "EXERCISE")) {
        return "今天适合运动放松";
    }
    if (ascii_contains_ci(insight, "HYDRATE") || ascii_contains_ci(insight, "CARE")) {
        return "喝水活动一下";
    }
    if (ascii_contains_ci(insight, "BREAK") || ascii_contains_ci(insight, "RHYTHM")) {
        return "休息五分钟再继续";
    }
    if (ascii_contains_ci(insight, "FOCUS")) {
        return "先完成一段学习任务";
    }
    if (ascii_contains_ci(insight, "PLAN")) {
        return "整理明天学习计划";
    }
    if (ascii_contains_ci(insight, "TASK_SPLIT")) {
        return "把任务拆成三步";
    }
    if (ascii_contains_ci(insight, "COMMUTE")) {
        return "出门前确认路线";
    }
    if (ascii_contains_ci(insight, "SLEEP")) {
        return "早点睡，明天再战";
    }
    if (ascii_contains_ci(insight, "REST")) {
        return "休息一下再继续";
    }
    if (ascii_contains_ci(insight, "SUN")) {
        return "天气热，记得补水";
    }
    return "按节奏专注学习";
}

static const char *deepseek_insight_tip(const char *insight)
{
    if (ascii_contains_ci(insight, "UMBRELLA") || ascii_contains_ci(insight, "RISK")) {
        return "天气有变，通勤留余量";
    }
    if (ascii_contains_ci(insight, "BREAKFAST")) {
        return "先吃早餐，上午效率更稳";
    }
    if (ascii_contains_ci(insight, "LUNCH")) {
        return "先吃午饭，下午再学习";
    }
    if (ascii_contains_ci(insight, "DINNER")) {
        return "先补晚饭，再做复盘";
    }
    if (ascii_contains_ci(insight, "RESEARCH_FOCUS")) {
        return "把论文阅读推进一小步";
    }
    if (ascii_contains_ci(insight, "PAPER_READING")) {
        return "读论文时先抓方法和结论";
    }
    if (ascii_contains_ci(insight, "EXPERIMENT")) {
        return "先整理资料和笔记";
    }
    if (ascii_contains_ci(insight, "WRITE_THESIS")) {
        return "复盘一段学习笔记";
    }
    if (ascii_contains_ci(insight, "EXERCISE")) {
        return "今天安排运动，给大脑换气";
    }
    if (ascii_contains_ci(insight, "HYDRATE") || ascii_contains_ci(insight, "CARE")) {
        return "喝水伸展，状态会回来";
    }
    if (ascii_contains_ci(insight, "BREAK") || ascii_contains_ci(insight, "RHYTHM")) {
        return "暂停几分钟，再继续学习";
    }
    if (ascii_contains_ci(insight, "FOCUS")) {
        return "先完成最重要的一件事";
    }
    if (ascii_contains_ci(insight, "PLAN")) {
        return "今晚把明天学习排清楚";
    }
    if (ascii_contains_ci(insight, "TASK_SPLIT")) {
        return "先拆任务，再开始做";
    }
    if (ascii_contains_ci(insight, "COMMUTE")) {
        return "看下路线和天气再出门";
    }
    if (ascii_contains_ci(insight, "SLEEP")) {
        return "早点休息，明天效率更高";
    }
    if (ascii_contains_ci(insight, "REST")) {
        return "休息一下，别把自己耗空";
    }
    if (ascii_contains_ci(insight, "SUN")) {
        return "天气偏热，补水别拖";
    }
    return "按当前节奏专注学习";
}

static bool insight_is_study(const char *insight)
{
    return ascii_contains_ci(insight, "RESEARCH_FOCUS") ||
           ascii_contains_ci(insight, "PAPER_READING") ||
           ascii_contains_ci(insight, "WRITE_THESIS") ||
           ascii_contains_ci(insight, "PLAN") ||
           ascii_contains_ci(insight, "TASK_SPLIT") ||
           ascii_contains_ci(insight, "FOCUS");
}

static bool insight_is_care(const char *insight)
{
    return ascii_contains_ci(insight, "BREAKFAST") ||
           ascii_contains_ci(insight, "LUNCH") ||
           ascii_contains_ci(insight, "DINNER") ||
           ascii_contains_ci(insight, "HYDRATE") ||
           ascii_contains_ci(insight, "REST") ||
           ascii_contains_ci(insight, "SLEEP") ||
           ascii_contains_ci(insight, "EXERCISE");
}

static const char *aligned_deepseek_tip(const char *edge_insight, const char *cloud_insight)
{
    if (insight_is_study(edge_insight)) {
        return insight_is_study(cloud_insight) ? deepseek_insight_tip(cloud_insight) :
               "先整理资料和笔记";
    }
    if (ascii_contains_ci(edge_insight, "HYDRATE")) {
        return "补水伸展后再学习";
    }
    if (ascii_contains_ci(edge_insight, "BREAKFAST") ||
        ascii_contains_ci(edge_insight, "LUNCH") ||
        ascii_contains_ci(edge_insight, "DINNER")) {
        return "吃好后再回到学习";
    }
    if (ascii_contains_ci(edge_insight, "UMBRELLA") ||
        ascii_contains_ci(edge_insight, "COMMUTE_CHECK")) {
        return "出门留意天气变化";
    }
    if (insight_is_care(edge_insight)) {
        return insight_is_care(cloud_insight) ? deepseek_insight_tip(cloud_insight) :
               "照顾状态再学习";
    }
    return deepseek_insight_tip(cloud_insight);
}

static const char *edge_basis_text(const char *basis)
{
    if (ascii_contains_ci(basis, "TOUCH") || ascii_contains_ci(basis, "FOCUS_") ||
        ascii_contains_ci(basis, "IDLE_")) {
        return "ESP-DL量化模型  触摸交互  专注计时";
    }
    if (ascii_contains_ci(basis, "HOLIDAY") || ascii_contains_ci(basis, "WEEKEND")) {
        return "ESP-DL量化模型  节假日  休息运动";
    }
    if (ascii_contains_ci(basis, "WEATHER_RAIN")) {
        return "ESP-DL量化模型  天气风险  日历";
    }
    if (ascii_contains_ci(basis, "WORKDAY")) {
        return "ESP-DL量化模型  工作日  学习节奏";
    }
    return "ESP-DL量化模型  天气日历  学习节奏";
}

static void update_pet_tip_from_insight(const char *text)
{
    char insight[64];
    char basis[128];
    char model[48];
    char risk[32];
    char edge_insight[64];
    char edge_conf[32];
    char edge_lat[32];
    char cloud_model[48];
    char cloud_insight[64];
    copy_field_value(text, "INSIGHT:", insight, sizeof(insight));
    copy_field_value(text, "BASIS:", basis, sizeof(basis));
    copy_field_value(text, "MODEL:", model, sizeof(model));
    copy_field_value(text, "RISK:", risk, sizeof(risk));
    copy_field_value(text, "EDGE_INSIGHT:", edge_insight, sizeof(edge_insight));
    copy_field_value(text, "EDGE_CONF:", edge_conf, sizeof(edge_conf));
    copy_field_value(text, "EDGE_LAT:", edge_lat, sizeof(edge_lat));
    copy_field_value(text, "CLOUD_MODEL:", cloud_model, sizeof(cloud_model));
    copy_field_value(text, "CLOUD_INSIGHT:", cloud_insight, sizeof(cloud_insight));

    s_pet_service_count++;
    s_pet_last_action = LEXIN_ACTION_AI_INSIGHT;
    const char *edge_value = field_has_value(edge_insight) ? edge_insight : insight;
    const bool cloud_ready = ascii_contains_ci(cloud_model, "DEEPSEEK") && field_has_value(cloud_insight);
    s_pet_tip = enterprise_insight_tip(edge_value);
    s_pet_reason = "硕士研伴建议";
    s_pet_weather_scene = ascii_contains_ci(basis, "WEATHER_RAIN") ? "天气风险" : "天气稳定";
    s_pet_time_scene = (ascii_contains_ci(basis, "HOLIDAY") || ascii_contains_ci(basis, "WEEKEND")) ? "休息节奏" : "学习节奏";
    s_pet_emotion_scene = "研伴建议";
    s_pet_model_tip = (ascii_contains_ci(model, "DEEPSEEK") && ascii_contains_ci(model, "ESP-DL")) ? "ESP-DL + DeepSeek 双模型" :
                      ascii_contains_ci(model, "ESP-DL") ? "ESP-DL 本地推理" :
                      ascii_contains_ci(model, "EDGE-INT8") ? "本地量化推理" :
                      ascii_contains_ci(model, "DEEPSEEK") ? "DeepSeek 已接入" : "离线建议";
    snprintf(s_pet_edge_summary, sizeof(s_pet_edge_summary), "%s", enterprise_insight_tip(edge_value));
    snprintf(s_pet_edge_meta, sizeof(s_pet_edge_meta), "ESP-DL 21维  置信度%.8s  延迟%.12s",
             field_has_value(edge_conf) ? edge_conf : "--",
             field_has_value(edge_lat) ? edge_lat : "--");
    snprintf(s_pet_cloud_summary, sizeof(s_pet_cloud_summary), "%s",
             cloud_ready ? aligned_deepseek_tip(edge_value, cloud_insight) : "未接入");
    snprintf(s_pet_cloud_meta, sizeof(s_pet_cloud_meta), "%s",
             cloud_ready ? "DeepSeek云端研判" : "请先连接DeepSeek");

    if (ascii_contains_ci(risk, "HIGH")) {
        s_pet_state = "贴心提醒";
        s_pet_accent = 0xff9f22;
    } else if (ascii_contains_ci(risk, "MEDIUM")) {
        s_pet_state = "节奏提醒";
        s_pet_accent = 0x9465ff;
    } else {
        s_pet_state = "学习状态";
        s_pet_accent = 0x1c98d2;
    }

    snprintf(s_pet_combined_reason, sizeof(s_pet_combined_reason), "%s", edge_basis_text(basis));
    snprintf(s_pet_combined_tip, sizeof(s_pet_combined_tip), "%s", s_pet_edge_summary);
}

static void update_pet_tip_querying(lexin_action_id_t action_id)
{
    s_pet_last_action = action_id;
    if (action_id == LEXIN_ACTION_WEATHER) {
        s_pet_state = "查询天气";
        s_pet_tip = "正在看天气";
        s_pet_reason = "查询请求";
        s_pet_accent = 0x1c98d2;
    } else if (action_id == LEXIN_ACTION_TIME) {
        s_pet_state = "查询日程";
        s_pet_tip = "正在同步日程";
        s_pet_reason = "查询请求";
        s_pet_accent = 0xff9f22;
    } else {
        s_pet_state = "思考中";
        s_pet_tip = "DeepSeek 正在研判";
        s_pet_reason = "硕士研伴";
        s_pet_accent = 0x9465ff;
    }
    s_pet_model_tip = action_id == LEXIN_ACTION_AI_INSIGHT ? "DeepSeek 思考中" : "已准备好";
}

static int lunar_token_value(const char *token)
{
    if (token == NULL) {
        return 0;
    }

    char upper[12];
    size_t i = 0;
    for (; token[i] != '\0' && i + 1 < sizeof(upper); i++) {
        upper[i] = ascii_upper_char(token[i]);
    }
    upper[i] = '\0';

    if (strcmp(upper, "ZHENG") == 0) {
        return 1;
    }
    if (strcmp(upper, "YI") == 0) {
        return 1;
    }
    if (strcmp(upper, "ER") == 0) {
        return 2;
    }
    if (strcmp(upper, "SAN") == 0) {
        return 3;
    }
    if (strcmp(upper, "SI") == 0) {
        return 4;
    }
    if (strcmp(upper, "WU") == 0) {
        return 5;
    }
    if (strcmp(upper, "LIU") == 0) {
        return 6;
    }
    if (strcmp(upper, "QI") == 0) {
        return 7;
    }
    if (strcmp(upper, "BA") == 0) {
        return 8;
    }
    if (strcmp(upper, "JIU") == 0) {
        return 9;
    }
    if (strcmp(upper, "DONG") == 0) {
        return 11;
    }
    if (strcmp(upper, "LA") == 0) {
        return 12;
    }
    return 0;
}

static const char *lunar_day_cn(int day)
{
    static const char *days[] = {
        "", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
        "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
        "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
    };
    return (day >= 1 && day <= 30) ? days[day] : "未知";
}

static const char *lunar_month_cn(int month)
{
    static const char *months[] = {
        "", "正月", "二月", "三月", "四月", "五月", "六月",
        "七月", "八月", "九月", "十月", "冬月", "腊月",
    };
    return (month >= 1 && month <= 12) ? months[month] : "未知";
}

static const char *lunar_to_cn(const char *lunar)
{
    static char buf[32];
    if (lunar == NULL || strstr(lunar, "UNKNOWN") != NULL || lunar[0] == '\0') {
        return "未知";
    }

    char work[64];
    snprintf(work, sizeof(work), "%s", lunar);
    char *tokens[8] = {0};
    int token_count = 0;
    char *saveptr = NULL;
    for (char *token = strtok_r(work, " ", &saveptr);
         token != NULL && token_count < (int)(sizeof(tokens) / sizeof(tokens[0]));
         token = strtok_r(NULL, " ", &saveptr)) {
        tokens[token_count++] = token;
    }

    int yue_index = -1;
    for (int i = 0; i < token_count; i++) {
        if (ascii_contains_ci(tokens[i], "YUE")) {
            yue_index = i;
            break;
        }
    }
    if (yue_index <= 0 || yue_index + 1 >= token_count) {
        return lunar;
    }

    int month = lunar_token_value(tokens[yue_index - 1]);
    int day = 0;
    int day_start = yue_index + 1;
    if (ascii_contains_ci(tokens[day_start], "SHI")) {
        day = 10 + ((day_start + 1 < token_count) ? lunar_token_value(tokens[day_start + 1]) : 0);
    } else if (lunar_token_value(tokens[day_start]) == 2 && day_start + 1 < token_count &&
               ascii_contains_ci(tokens[day_start + 1], "SHI")) {
        day = 20 + ((day_start + 2 < token_count) ? lunar_token_value(tokens[day_start + 2]) : 0);
    } else if (lunar_token_value(tokens[day_start]) == 3 && day_start + 1 < token_count &&
               ascii_contains_ci(tokens[day_start + 1], "SHI")) {
        day = 30;
    } else {
        day = lunar_token_value(tokens[day_start]);
    }

    if (month == 0 || day == 0) {
        return "未知";
    }
    snprintf(buf, sizeof(buf), "%s%s", lunar_month_cn(month), lunar_day_cn(day));
    return buf;
}

static const char *holiday_to_cn(const char *holiday)
{
    if (holiday == NULL || holiday[0] == '\0' || ascii_contains_ci(holiday, "NONE") ||
        ascii_contains_ci(holiday, "UNKNOWN")) {
        return "无";
    }
    if (ascii_contains_ci(holiday, "LABOR")) {
        return "劳动节";
    }
    if (ascii_contains_ci(holiday, "NATIONAL")) {
        return "国庆节";
    }
    if (ascii_contains_ci(holiday, "NEW YEAR")) {
        return "元旦";
    }
    return "无";
}

static const char *calendar_effective_day_type(const char *day_type, const char *holiday_cn,
                                               int year, int month, int day)
{
    if (holiday_cn != NULL && strcmp(holiday_cn, "无") != 0) {
        return "HOLIDAY";
    }
    if (ascii_contains_ci(day_type, "WORKDAY") ||
        ascii_contains_ci(day_type, "WEEKEND") ||
        ascii_contains_ci(day_type, "HOLIDAY")) {
        return day_type;
    }
    return weekday_monday0(year, month, day) >= 5 ? "WEEKEND" : "WORKDAY";
}

static const char *calendar_day_type_cn(const char *holiday_cn, const char *day_type)
{
    if (holiday_cn != NULL && strcmp(holiday_cn, "无") != 0) {
        return holiday_cn;
    }
    if (ascii_contains_ci(day_type, "WEEKEND") || ascii_contains_ci(day_type, "HOLIDAY")) {
        return "休息日";
    }
    if (ascii_contains_ci(day_type, "WORKDAY")) {
        return "工作日";
    }
    return "未知";
}

static void lvgl_draw_sun_cloud(lv_obj_t *parent)
{
    lv_obj_t *sun = lv_obj_create(parent);
    lv_obj_remove_style_all(sun);
    lv_obj_set_pos(sun, 752, 142);
    lv_obj_set_size(sun, 118, 118);
    lvgl_set_bg(sun, 0xffd23f);
    lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *cloud = lv_obj_create(parent);
    lv_obj_remove_style_all(cloud);
    lv_obj_set_pos(cloud, 650, 240);
    lv_obj_set_size(cloud, 284, 82);
    lvgl_set_bg(cloud, 0xffffff);
    lv_obj_set_style_radius(cloud, 38, 0);

    lv_obj_t *c1 = lv_obj_create(parent);
    lv_obj_remove_style_all(c1);
    lv_obj_set_pos(c1, 634, 198);
    lv_obj_set_size(c1, 108, 108);
    lvgl_set_bg(c1, 0xffffff);
    lv_obj_set_style_radius(c1, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *c2 = lv_obj_create(parent);
    lv_obj_remove_style_all(c2);
    lv_obj_set_pos(c2, 734, 180);
    lv_obj_set_size(c2, 134, 134);
    lvgl_set_bg(c2, 0xffffff);
    lv_obj_set_style_radius(c2, LV_RADIUS_CIRCLE, 0);
}

static void lvgl_draw_weather_app_icon(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *icon = lvgl_card(parent, x, y, 104, 104, 0x238dff, 24);
    lvgl_set_vertical_gradient(icon, 0x30c9ff, 0x1b82ff);
    lv_obj_set_style_border_width(icon, 1, 0);
    lv_obj_set_style_border_color(icon, lv_color_hex(0x8fe2ff), 0);

    lvgl_card(icon, 18, 18, 44, 44, 0xffd449, LV_RADIUS_CIRCLE);
    lvgl_card(icon, 35, 54, 54, 24, 0xffffff, 12);
    lvgl_card(icon, 26, 44, 30, 30, 0xffffff, LV_RADIUS_CIRCLE);
    lvgl_card(icon, 48, 38, 38, 38, 0xffffff, LV_RADIUS_CIRCLE);
    lvgl_center_label(icon, "晴", 0, 78, 104, &lexin_cn_20, 0xffffff);
}

static void lvgl_draw_calendar_app_icon(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *icon = lvgl_card(parent, x, y, 104, 104, 0xfffbf4, 24);
    lv_obj_set_style_border_width(icon, 1, 0);
    lv_obj_set_style_border_color(icon, lv_color_hex(0xffd997), 0);

    lvgl_card(icon, 0, 0, 104, 36, 0xffa600, 24);
    lvgl_card(icon, 0, 22, 104, 20, 0xffa600, 0);
    lvgl_center_label(icon, "JUN", 0, 8, 104, &lv_font_montserrat_20, 0xffffff);
    lvgl_center_label(icon, "15", 0, 42, 104, &lv_font_montserrat_32, 0x10283e);
    lvgl_center_label(icon, "日程", 0, 78, 104, &lexin_cn_20, 0x9a6500);
}

static void lvgl_draw_ai_app_icon(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *icon = lvgl_card(parent, x, y, 104, 104, 0x9465ff, 24);
    lvgl_set_vertical_gradient(icon, 0xa886ff, 0x7052f5);
    lv_obj_set_style_border_width(icon, 1, 0);
    lv_obj_set_style_border_color(icon, lv_color_hex(0xc9b8ff), 0);

    lvgl_center_label(icon, "AI", 0, 28, 104, &lv_font_montserrat_32, 0xffffff);
    lvgl_center_label(icon, "研伴", 0, 68, 104, &lexin_cn_20, 0xf3efff);
}

static void lvgl_draw_emotion_app_icon(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *icon = lvgl_card(parent, x, y, 104, 104, 0x24b8a6, 24);
    lvgl_set_vertical_gradient(icon, 0x42d7c5, 0x178fba);
    lv_obj_set_style_border_width(icon, 1, 0);
    lv_obj_set_style_border_color(icon, lv_color_hex(0x9aeee2), 0);

    lv_obj_t *face = lvgl_card(icon, 23, 17, 58, 58, 0xffdf72, LV_RADIUS_CIRCLE);
    lvgl_card(face, 15, 20, 7, 7, 0x16324f, LV_RADIUS_CIRCLE);
    lvgl_card(face, 36, 20, 7, 7, 0x16324f, LV_RADIUS_CIRCLE);
    lvgl_card(face, 20, 39, 20, 5, 0x16324f, 3);
    lvgl_center_label(icon, "情绪研伴", 0, 78, 104, &lexin_cn_20, 0xffffff);
}

static void lvgl_draw_pet_avatar(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *halo = lvgl_card(parent, x, y, 122, 122, 0xe8f8ff, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(halo, LV_OPA_80, 0);
    lv_obj_set_style_border_width(halo, 2, 0);
    lv_obj_set_style_border_color(halo, lv_color_hex(0xbbeeff), 0);

    lv_obj_t *face = lvgl_card(parent, x + 22, y + 20, 78, 78, 0xffd23f, LV_RADIUS_CIRCLE);
    lv_obj_set_style_shadow_width(face, 10, 0);
    lv_obj_set_style_shadow_opa(face, LV_OPA_20, 0);
    lvgl_card(parent, x + 43, y + 48, 9, 9, 0x10283e, LV_RADIUS_CIRCLE);
    lvgl_card(parent, x + 72, y + 48, 9, 9, 0x10283e, LV_RADIUS_CIRCLE);
    lvgl_card(parent, x + 51, y + 70, 22, 6, 0x10283e, 3);
    lvgl_card(parent, x + 34, y + 62, 12, 7, 0xffb8a8, LV_RADIUS_CIRCLE);
    lvgl_card(parent, x + 78, y + 62, 12, 7, 0xffb8a8, LV_RADIUS_CIRCLE);
}

static void lvgl_init_display(void)
{
    if (s_panel == NULL || s_lvgl_ready) {
        return;
    }

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * 60,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB888,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_spiram = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };
    s_lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    s_lvgl_ready = s_lvgl_disp != NULL;
    if (!s_lvgl_ready) {
        ESP_LOGE(TAG, "LVGL display init failed; using fallback renderer");
    }
}

static bool lvgl_show_launcher(void)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_vertical_gradient(scr, 0xe7f7ff, 0xc7f0ff);
    lvgl_card(scr, 0, 0, LCD_H_RES, 86, 0x19afd8, 0);
    lv_obj_set_style_bg_opa(lvgl_card(scr, 0, 86, LCD_H_RES, 102, 0x8fe2f5, 0), LV_OPA_40, 0);
    lvgl_label(scr, "乐鑫 AI桌宠", 48, 22, &lexin_cn_28, 0xffffff);

    lv_obj_t *pet_card = lvgl_glass_card(scr, 66, 132, 348, 344, 28);
    lvgl_label(pet_card, "小伙伴在线", 32, 30, &lexin_cn_28, 0x10283e);
    lvgl_card(pet_card, 266, 42, 10, 10, 0x2ecc71, LV_RADIUS_CIRCLE);
    lvgl_label(pet_card, "在线", 284, 35, &lexin_cn_20, 0x2c8f57);
    lvgl_label(pet_card, "今日陪伴", 34, 88, &lexin_cn_20, 0x577489);
    lv_obj_t *tip_label = lvgl_label(pet_card, s_pet_tip, 34, 118, &lexin_cn_20, 0x10283e);
    lvgl_label_width(tip_label, 276);

    lvgl_draw_pet_avatar(pet_card, 34, 184);
    lv_obj_t *state_pill = lvgl_card(pet_card, 186, 184, 126, 42, 0xe8f8ff, 21);
    lv_obj_set_style_bg_opa(state_pill, LV_OPA_80, 0);
    lv_obj_set_style_border_width(state_pill, 2, 0);
    lv_obj_set_style_border_color(state_pill, lv_color_hex(s_pet_accent), 0);
    lv_obj_t *state_label = lvgl_label(state_pill, s_pet_state, 16, 10, &lexin_cn_20, s_pet_accent);
    lvgl_label_width(state_label, 96);
    char service_text[24];
    snprintf(service_text, sizeof(service_text), "服务%d次", s_pet_service_count);
    lvgl_label(pet_card, service_text, 194, 234, &lexin_cn_20, 0x577489);
    char focus_text[32];
    lexin_interaction_status_text(focus_text, sizeof(focus_text));
    lv_obj_t *focus_label = lvgl_label(pet_card, focus_text, 194, 262, &lexin_cn_20, 0x577489);
    lvgl_label_width(focus_label, 116);
    lv_obj_t *analysis_btn = lvgl_card(pet_card, 180, 282, 142, 38, 0xe8f8ff, 19);
    lv_obj_set_style_bg_opa(analysis_btn, LV_OPA_80, 0);
    lv_obj_set_style_border_width(analysis_btn, 2, 0);
    lv_obj_set_style_border_color(analysis_btn, lv_color_hex(s_pet_accent), 0);
    lvgl_label(analysis_btn, "查看洞察", 24, 8, &lexin_cn_20, s_pet_accent);

    lv_obj_t *panel = lvgl_glass_card(scr, 456, 132, 486, 344, 28);
    lvgl_draw_weather_app_icon(panel, 14, 48);
    lvgl_draw_calendar_app_icon(panel, 130, 48);
    lvgl_draw_emotion_app_icon(panel, 246, 48);
    lvgl_draw_ai_app_icon(panel, 362, 48);
    lvgl_center_label(panel, "天气", 14, 170, 104, &lexin_cn_20, 0x10283e);
    lvgl_center_label(panel, "日历", 130, 170, 104, &lexin_cn_20, 0x10283e);
    lvgl_center_label(panel, "情绪研伴", 246, 170, 104, &lexin_cn_20, 0x10283e);
    lvgl_center_label(panel, "研伴", 362, 170, 104, &lexin_cn_20, 0x10283e);
    lvgl_center_label(panel, "天气  日历  情绪识别  双模型研伴", 0, 270, 486, &lexin_cn_20, 0x577489);

    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_suggestion_page(void)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_vertical_gradient(scr, 0xe7f7ff, 0xc7f0ff);
    lvgl_card(scr, 0, 0, LCD_H_RES, 86, 0x19afd8, 0);
    lv_obj_set_style_bg_opa(lvgl_card(scr, 0, 86, LCD_H_RES, 88, 0x8fe2f5, 0), LV_OPA_40, 0);
    lvgl_label(scr, "返回", 32, 28, &lexin_cn_20, 0xffffff);
    lvgl_label(scr, "研伴建议", 110, 24, &lexin_cn_28, 0xffffff);

    lv_obj_t *main_card = lvgl_glass_card(scr, 78, 132, 438, 340, 28);
    lvgl_label(main_card, "ESP-DL模型", 40, 34, &lexin_cn_28, 0x10283e);
    lvgl_label(main_card, "本地推理结果", 42, 92, &lexin_cn_20, 0x577489);
    lv_obj_t *tip_label = lvgl_label(main_card, s_pet_combined_tip, 42, 124, &lexin_cn_28, s_pet_accent);
    lvgl_label_width(tip_label, 330);
    lvgl_label(main_card, "分析依据", 42, 190, &lexin_cn_20, 0x577489);
    lv_obj_t *reason_label = lvgl_label(main_card, s_pet_combined_reason, 42, 222, &lexin_cn_20, 0x10283e);
    lvgl_label_width(reason_label, 330);
    lv_obj_t *edge_meta = lvgl_label(main_card, s_pet_edge_meta, 42, 282, &lexin_cn_20, 0x577489);
    lvgl_label_width(edge_meta, 330);

    lv_obj_t *ai_card = lvgl_glass_card(scr, 546, 132, 392, 340, 28);
    lvgl_label(ai_card, "DeepSeek", 44, 34, &lexin_cn_28, 0x10283e);
    lvgl_label(ai_card, "云端建议", 46, 92, &lexin_cn_20, 0x577489);
    lv_obj_t *cloud_label = lvgl_label(ai_card, s_pet_cloud_summary, 46, 124, &lexin_cn_28, 0x9465ff);
    lvgl_label_width(cloud_label, 300);
    lvgl_label(ai_card, "模型说明", 46, 190, &lexin_cn_20, 0x577489);
    lv_obj_t *cloud_meta = lvgl_label(ai_card, s_pet_cloud_meta, 46, 222, &lexin_cn_20, 0x10283e);
    lvgl_label_width(cloud_meta, 300);
    lvgl_label(ai_card, "对比价值", 46, 264, &lexin_cn_20, 0x577489);
    lv_obj_t *compare_label = lvgl_label(ai_card, "同一输入  不同推理风格", 46, 294, &lexin_cn_20, 0x10283e);
    lvgl_label_width(compare_label, 300);

    lvgl_port_unlock();
    return true;
}

static const char *vision_expression_text(const lexin_vision_snapshot_t *snapshot)
{
    switch (snapshot->emotion) {
    case LEXIN_VISION_EMOTION_HAPPY:
        return "HAPPY";
    case LEXIN_VISION_EMOTION_LONELY:
        return "LONELY";
    case LEXIN_VISION_EMOTION_ALERT:
        return "ALERT";
    case LEXIN_VISION_EMOTION_SLEEPY:
        return "SLEEPY";
    case LEXIN_VISION_EMOTION_CALM:
    default:
        return "CALM";
    }

#if 0
    if (!snapshot->face_detected) {
        return "等待识别";
    }
    switch (snapshot->expression) {
    case LEXIN_VISION_EXPRESSION_HAPPY:
        return "开心";
    case LEXIN_VISION_EXPRESSION_SAD:
        return "低落";
    case LEXIN_VISION_EXPRESSION_NEUTRAL:
        return "平静";
    case LEXIN_VISION_EXPRESSION_UNKNOWN:
    default:
        return "分析中";
    }
#endif
}

static uint32_t vision_expression_accent(const lexin_vision_snapshot_t *snapshot)
{
    switch (snapshot->emotion) {
    case LEXIN_VISION_EMOTION_HAPPY:
        return 0x20a56b;
    case LEXIN_VISION_EMOTION_ALERT:
        return 0x6f6bd9;
    case LEXIN_VISION_EMOTION_CALM:
        return 0x1c98d2;
    default:
        return 0x71889a;
    }
}

static const char *vision_response_text(const lexin_vision_snapshot_t *snapshot)
{
    return snapshot->response[0] ? snapshot->response : "CALM AND LISTENING";
}

static bool refresh_emotion_preview(void)
{
    if (!s_emotion_preview_pixels) {
        s_emotion_preview_pixels = heap_caps_malloc(
            EMOTION_PREVIEW_PIXELS * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_emotion_preview_pixels) {
            s_emotion_preview_pixels = heap_caps_malloc(
                EMOTION_PREVIEW_PIXELS * sizeof(uint16_t), MALLOC_CAP_8BIT);
        }
        if (!s_emotion_preview_pixels) {
            return false;
        }
        s_emotion_preview_back_pixels = heap_caps_malloc(
            EMOTION_PREVIEW_PIXELS * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_emotion_preview_back_pixels) {
            s_emotion_preview_back_pixels = heap_caps_malloc(
                EMOTION_PREVIEW_PIXELS * sizeof(uint16_t), MALLOC_CAP_8BIT);
        }
        if (!s_emotion_preview_back_pixels) {
            heap_caps_free(s_emotion_preview_pixels);
            s_emotion_preview_pixels = NULL;
            return false;
        }
        memset(s_emotion_preview_pixels, 0,
               EMOTION_PREVIEW_PIXELS * sizeof(uint16_t));
        memset(s_emotion_preview_back_pixels, 0,
               EMOTION_PREVIEW_PIXELS * sizeof(uint16_t));

        memset(&s_emotion_preview_dsc, 0, sizeof(s_emotion_preview_dsc));
        s_emotion_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_emotion_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_emotion_preview_dsc.header.w = LEXIN_VISION_PREVIEW_WIDTH;
        s_emotion_preview_dsc.header.h = LEXIN_VISION_PREVIEW_HEIGHT;
        s_emotion_preview_dsc.header.stride = LEXIN_VISION_PREVIEW_WIDTH * sizeof(uint16_t);
        s_emotion_preview_dsc.data_size = EMOTION_PREVIEW_PIXELS * sizeof(uint16_t);
        s_emotion_preview_dsc.data = (const uint8_t *)s_emotion_preview_pixels;
    }

    return lexin_vision_copy_preview(s_emotion_preview_back_pixels,
                                         EMOTION_PREVIEW_PIXELS,
                                         &s_emotion_preview_frame_id) == ESP_OK;
}

static void lvgl_commit_emotion_preview(void)
{
    uint16_t *front = s_emotion_preview_pixels;
    s_emotion_preview_pixels = s_emotion_preview_back_pixels;
    s_emotion_preview_back_pixels = front;
    s_emotion_preview_dsc.data = (const uint8_t *)s_emotion_preview_pixels;
}

static void lvgl_update_face_box(lv_obj_t *box,
                                 const lexin_vision_snapshot_t *snapshot)
{
    if (!box || !snapshot->face_detected || snapshot->input_width == 0 ||
        snapshot->input_height == 0 || snapshot->face_width == 0 ||
        snapshot->face_height == 0) {
        if (box) {
            lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    int x = EMOTION_PREVIEW_X +
        (snapshot->face_x * LEXIN_VISION_PREVIEW_WIDTH) /
        snapshot->input_width;
    int y = EMOTION_PREVIEW_Y +
        (snapshot->face_y * LEXIN_VISION_PREVIEW_HEIGHT) /
        snapshot->input_height;
    int w = (snapshot->face_width * LEXIN_VISION_PREVIEW_WIDTH) /
        snapshot->input_width;
    int h = (snapshot->face_height * LEXIN_VISION_PREVIEW_HEIGHT) /
        snapshot->input_height;

    if (w < 18) w = 18;
    if (h < 18) h = 18;
    if (x < EMOTION_PREVIEW_X) x = EMOTION_PREVIEW_X;
    if (y < EMOTION_PREVIEW_Y) y = EMOTION_PREVIEW_Y;
    if (x + w > EMOTION_PREVIEW_X + LEXIN_VISION_PREVIEW_WIDTH) {
        w = EMOTION_PREVIEW_X + LEXIN_VISION_PREVIEW_WIDTH - x;
    }
    if (y + h > EMOTION_PREVIEW_Y + LEXIN_VISION_PREVIEW_HEIGHT) {
        h = EMOTION_PREVIEW_Y + LEXIN_VISION_PREVIEW_HEIGHT - y;
    }

    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
}

static bool lvgl_refresh_emotion_live(const lexin_vision_snapshot_t *snapshot)
{
    if (!snapshot || lexin_launcher_current_screen() != LEXIN_SCREEN_EMOTION) {
        return false;
    }

    bool preview_ready = refresh_emotion_preview();
    if (!lvgl_port_lock(80)) {
        return false;
    }
    if (lexin_launcher_current_screen() != LEXIN_SCREEN_EMOTION ||
        !s_emotion_preview_image) {
        lvgl_port_unlock();
        return false;
    }

    if (preview_ready) {
        lvgl_commit_emotion_preview();
        lv_image_set_src(s_emotion_preview_image, &s_emotion_preview_dsc);
        lv_obj_invalidate(s_emotion_preview_image);
        if (s_emotion_waiting_label) {
            lv_obj_add_flag(s_emotion_waiting_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_update_face_box(s_emotion_face_box, snapshot);

    char text[160];
    snprintf(text, sizeof(text), "FACE %s\nFPS %u.%u\nFRAME %lu\nRGB565",
             snapshot->face_detected ? "YES" : "NO",
             snapshot->camera_fps_x10 / 10, snapshot->camera_fps_x10 % 10,
             (unsigned long)s_emotion_preview_frame_id);
    lv_label_set_text(s_emotion_camera_meta_label, text);

    lv_label_set_text(s_emotion_expression_label, vision_expression_text(snapshot));
    lv_obj_set_style_text_color(s_emotion_expression_label,
                                lv_color_hex(vision_expression_accent(snapshot)), 0);

    snprintf(text, sizeof(text),
             "FACE: %s\nCONF: %03u\nLOCAL: %lums\nMODEL: ESP-WHO",
             snapshot->face_detected ? "YES" : "NO", snapshot->confidence,
             (unsigned long)snapshot->inference_ms);
    lv_label_set_text(s_emotion_meta_label, text);

    snprintf(text, sizeof(text),
             "LCD  OK\nGT911 OK\nCAM  %s\nAI   %s\nRGB  288x216\nLOCAL ONLY\nNET  %s",
             snapshot->camera_ready ? "OK" : "WAIT",
             snapshot->backend == LEXIN_VISION_BACKEND_ESP_WHO ? "WHO OK" : "WAIT",
             snapshot->service_ready ? "READY" : "WAIT");
    lv_label_set_text(s_emotion_system_meta_label, text);
    lv_label_set_text(s_emotion_online_label,
                      snapshot->service_ready ? "AI ONLINE" : "AI STARTING");

    lv_label_set_text(s_emotion_response_label, vision_response_text(snapshot));

    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_emotion_page(void)
{
    lexin_vision_snapshot_t snapshot;
    lexin_vision_get_snapshot(&snapshot);
    bool preview_ready = refresh_emotion_preview();
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    if (preview_ready) {
        lvgl_commit_emotion_preview();
    }
    s_emotion_preview_image = NULL;
    s_emotion_waiting_label = NULL;
    s_emotion_face_box = NULL;
    s_emotion_camera_meta_label = NULL;
    s_emotion_expression_label = NULL;
    s_emotion_meta_label = NULL;
    s_emotion_system_meta_label = NULL;
    s_emotion_response_label = NULL;
    s_emotion_online_label = NULL;
    lvgl_set_vertical_gradient(scr, 0x06111c, 0x0b2635);
    lv_obj_t *header = lvgl_card(scr, 0, 0, LCD_H_RES, 82, 0x0b3650, 0);
    lvgl_set_vertical_gradient(header, 0x0a2740, 0x086179);
    lvgl_label(header, "返回", 24, 27, &lexin_cn_20, 0xffffff);
    lvgl_label(header, "LEXIN VISION", 112, 20, &lv_font_montserrat_28, 0xffffff);
    lvgl_label(header, "EDGE AI COMPANION", 320, 29, &lv_font_montserrat_20, 0x99f3ff);
    lv_obj_t *online = lvgl_card(header, 824, 18, 166, 46, 0x20d9d2, 6);
    s_emotion_online_label = lvgl_center_label(
        online, snapshot.service_ready ? "AI ONLINE" : "AI STARTING",
        0, 12, 166, &lv_font_montserrat_20, 0x06283a);

    lv_obj_t *camera_card = lvgl_card(scr, 28, 102, 466, 326, 0x0c3047, 8);
    lv_obj_set_style_border_width(camera_card, 2, 0);
    lv_obj_set_style_border_color(camera_card, lv_color_hex(0x1cc9d5), 0);
    lvgl_label(camera_card, "CAMERA", 20, 14, &lv_font_montserrat_20, 0xb8f7ff);
    lv_obj_t *preview_frame = lvgl_card(camera_card,
                                        EMOTION_PREVIEW_X - 4, EMOTION_PREVIEW_Y - 4,
                                        LEXIN_VISION_PREVIEW_WIDTH + 8,
                                        LEXIN_VISION_PREVIEW_HEIGHT + 8,
                                        0x061018, 4);
    lv_obj_set_style_border_width(preview_frame, 2, 0);
    lv_obj_set_style_border_color(preview_frame, lv_color_hex(0x5cf6ff), 0);
    if (s_emotion_preview_pixels) {
        lv_obj_t *image = lv_image_create(camera_card);
        lv_image_set_src(image, &s_emotion_preview_dsc);
        lv_obj_set_pos(image, EMOTION_PREVIEW_X, EMOTION_PREVIEW_Y);
        s_emotion_preview_image = image;
    }
    if (!preview_ready) {
        s_emotion_waiting_label = lvgl_label(camera_card, "CAMERA STARTING",
                                             EMOTION_PREVIEW_X + 38,
                                             EMOTION_PREVIEW_Y + 94,
                                             &lv_font_montserrat_20, 0x6fa2b5);
        lvgl_label_width(s_emotion_waiting_label, 220);
    }
    s_emotion_face_box = lv_obj_create(camera_card);
    lv_obj_remove_style_all(s_emotion_face_box);
    lv_obj_set_style_bg_opa(s_emotion_face_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_emotion_face_box, 3, 0);
    lv_obj_set_style_border_color(s_emotion_face_box, lv_color_hex(0xffe45c), 0);
    lv_obj_set_style_radius(s_emotion_face_box, 4, 0);
    lvgl_update_face_box(s_emotion_face_box, &snapshot);
    char camera_meta[96];
    snprintf(camera_meta, sizeof(camera_meta),
             "FACE %s\nFPS %u.%u\nFRAME %lu\nRGB565",
             snapshot.face_detected ? "YES" : "NO",
             snapshot.camera_fps_x10 / 10, snapshot.camera_fps_x10 % 10,
             (unsigned long)s_emotion_preview_frame_id);
    s_emotion_camera_meta_label = lvgl_label(camera_card, camera_meta, 330, 72,
                                             &lv_font_montserrat_20, 0xa6d9e8);
    lvgl_label_width(s_emotion_camera_meta_label, 116);

    lv_obj_t *emotion_card = lvgl_card(scr, 514, 102, 242, 326, 0x0c3c50, 8);
    lv_obj_set_style_border_width(emotion_card, 2, 0);
    lv_obj_set_style_border_color(emotion_card, lv_color_hex(0x20d9d2), 0);
    lvgl_label(emotion_card, "EMOTION", 20, 18, &lv_font_montserrat_20, 0xb8f7ff);
    uint32_t accent = vision_expression_accent(&snapshot);
    s_emotion_expression_label = lvgl_label(
        emotion_card, vision_expression_text(&snapshot),
        20, 66, &lexin_cn_28, accent);
    lvgl_label_width(s_emotion_expression_label, 200);
    char emotion_meta[128];
    snprintf(emotion_meta, sizeof(emotion_meta),
             "FACE: %s\nCONF: %03u\nLOCAL: %lums\nMODEL: ESP-WHO",
             snapshot.face_detected ? "YES" : "NO", snapshot.confidence,
             (unsigned long)snapshot.inference_ms);
    s_emotion_meta_label = lvgl_label(emotion_card, emotion_meta, 20, 124,
                                      &lv_font_montserrat_20, 0xb8e4ee);
    lvgl_label_width(s_emotion_meta_label, 202);

    lv_obj_t *system_card = lvgl_card(scr, 776, 102, 220, 326, 0x0a2b41, 8);
    lv_obj_set_style_border_width(system_card, 2, 0);
    lv_obj_set_style_border_color(system_card, lv_color_hex(0x187f9e), 0);
    lvgl_label(system_card, "SYSTEM", 18, 18, &lv_font_montserrat_20, 0xb8f7ff);
    char system_meta[160];
    snprintf(system_meta, sizeof(system_meta),
             "LCD  OK\nGT911 OK\nCAM  %s\nAI   %s\nRGB  288x216\nLOCAL ONLY\nNET  %s",
             snapshot.camera_ready ? "OK" : "WAIT",
             snapshot.backend == LEXIN_VISION_BACKEND_ESP_WHO ? "WHO OK" : "WAIT",
             snapshot.service_ready ? "READY" : "WAIT");
    s_emotion_system_meta_label = lvgl_label(system_card, system_meta, 18, 64,
                                             &lv_font_montserrat_20, 0x8deaf3);
    lvgl_label_width(s_emotion_system_meta_label, 184);

    lv_obj_t *response = lvgl_card(scr, 28, 452, 968, 116, 0x0b3048, 8);
    lv_obj_set_style_border_width(response, 2, 0);
    lv_obj_set_style_border_color(response, lv_color_hex(0x147e9d), 0);
    lvgl_label(response, "RESPONSE", 22, 16, &lv_font_montserrat_20, 0x7edce8);
    s_emotion_response_label = lvgl_label(
        response, vision_response_text(&snapshot), 22, 54,
        &lv_font_montserrat_28, 0xffffff);

    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_pet_ai_page(void)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_vertical_gradient(scr, 0xe7f7ff, 0xc7f0ff);
    lvgl_card(scr, 0, 0, LCD_H_RES, 86, 0x19afd8, 0);
    lv_obj_set_style_bg_opa(lvgl_card(scr, 0, 86, LCD_H_RES, 88, 0x8fe2f5, 0), LV_OPA_40, 0);
    lvgl_label(scr, "返回", 32, 28, &lexin_cn_20, 0xffffff);
    lvgl_label(scr, "生活助理", 110, 24, &lexin_cn_28, 0xffffff);

    lv_obj_t *main_card = lvgl_glass_card(scr, 92, 132, 430, 340, 28);
    lvgl_label(main_card, "研伴节奏", 40, 34, &lexin_cn_28, 0x10283e);
    lvgl_label(main_card, "当前建议", 42, 92, &lexin_cn_20, 0x577489);
    lv_obj_t *tip_label = lvgl_label(main_card, s_pet_combined_tip, 42, 124, &lexin_cn_28, s_pet_accent);
    lvgl_label_width(tip_label, 330);
    lvgl_label(main_card, "本地输入", 42, 188, &lexin_cn_20, 0x577489);
    lv_obj_t *feature_chip = lvgl_card(main_card, 42, 222, 102, 38, 0xe8f8ff, 19);
    lv_obj_set_style_bg_opa(feature_chip, LV_OPA_80, 0);
    lvgl_center_label(feature_chip, "21维模型", 0, 8, 102, &lexin_cn_20, 0x1c7ed6);
    lv_obj_t *touch_chip = lvgl_card(main_card, 160, 222, 104, 38, 0xf3f0ff, 19);
    lv_obj_set_style_bg_opa(touch_chip, LV_OPA_80, 0);
    lvgl_center_label(touch_chip, "触摸交互", 0, 8, 104, &lexin_cn_20, 0x7651d9);
    lv_obj_t *focus_chip = lvgl_card(main_card, 280, 222, 104, 38, 0xfff6df, 19);
    lv_obj_set_style_bg_opa(focus_chip, LV_OPA_80, 0);
    lvgl_center_label(focus_chip, "专注计时", 0, 8, 104, &lexin_cn_20, 0xa96b00);
    lv_obj_t *note_label = lvgl_label(main_card, "按时间和互动切换提醒", 42, 292, &lexin_cn_20, 0x577489);
    lvgl_label_width(note_label, 330);

    lv_obj_t *pet_card = lvgl_glass_card(scr, 556, 132, 360, 340, 28);
    lvgl_label(pet_card, "桌宠反馈", 44, 34, &lexin_cn_28, 0x10283e);
    lvgl_draw_pet_avatar(pet_card, 44, 84);
    lvgl_label(pet_card, "提醒类型", 188, 96, &lexin_cn_20, 0x577489);
    lv_obj_t *state_label = lvgl_label(pet_card, s_pet_state, 188, 128, &lexin_cn_28, s_pet_accent);
    lvgl_label_width(state_label, 138);
    lv_obj_t *pill = lvgl_card(pet_card, 54, 220, 252, 46, 0xe8f8ff, 23);
    lv_obj_set_style_bg_opa(pill, LV_OPA_80, 0);
    lv_obj_set_style_border_width(pill, 2, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(s_pet_accent), 0);
    lvgl_label(pill, s_pet_last_action == LEXIN_ACTION_WEATHER ? "天气提醒" :
               s_pet_last_action == LEXIN_ACTION_TIME ? "日程提醒" : "生活提醒",
               58, 10, &lexin_cn_20, s_pet_accent);

    char service_text[32];
    snprintf(service_text, sizeof(service_text), "已服务%d次", s_pet_service_count);
    lvgl_label(pet_card, service_text, 64, 278, &lexin_cn_20, 0x577489);
    lv_obj_t *model_label = lvgl_label(pet_card, "本地判断  云端润色", 64, 306, &lexin_cn_20, 0x10283e);
    lvgl_label_width(model_label, 238);

    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_querying_page(lexin_action_id_t action_id)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    uint32_t bg = action_id == LEXIN_ACTION_WEATHER ? 0xe9f6ff :
                  action_id == LEXIN_ACTION_TIME ? 0xf3fbf7 : 0xf4f1ff;
    uint32_t accent = action_id == LEXIN_ACTION_WEATHER ? 0x1c7ed6 :
                      action_id == LEXIN_ACTION_TIME ? 0x0f8a5f : 0x9465ff;
    lvgl_set_bg(scr, bg);
    lvgl_label(scr, "返回", 32, 28, &lexin_cn_20, 0x16324f);
    lvgl_label(scr, action_id == LEXIN_ACTION_WEATHER ? "天气提醒" :
               action_id == LEXIN_ACTION_TIME ? "日程提醒" : "研伴建议",
               70, 154, &lexin_cn_28, accent);
    lvgl_label(scr, "加载中", 74, 232, &lexin_cn_28, 0x42627d);
    lvgl_card(scr, 0, 510, LCD_H_RES, 90, accent, 0);
    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_weather_result_page(const char *text)
{
    char temp[32];
    char weather[48];
    char rain[32];
    char advice[64];
    char temp_number[16];
    copy_field_value(text, "TEMP:", temp, sizeof(temp));
    copy_field_value(text, "WEATHER:", weather, sizeof(weather));
    copy_field_value(text, "RAIN:", rain, sizeof(rain));
    copy_field_value(text, "ADVICE:", advice, sizeof(advice));
    const char *weather_cn = weather_to_cn(weather);
    const char *suggestion_cn = weather_pet_suggestion(weather, rain, advice);
    const char *feel_cn = weather_feel_text(temp, advice);
    const char *outdoor_cn = weather_outdoor_text(weather, rain, advice);
    const char *study_cn = weather_study_text(weather, rain, advice);
    update_pet_tip_from_weather(weather, rain, advice);
    snprintf(temp_number, sizeof(temp_number), "NA");
    size_t temp_i = 0;
    for (const char *p = temp; *p != '\0' && temp_i + 1 < sizeof(temp_number); p++) {
        if ((*p >= '0' && *p <= '9') || *p == '-') {
            temp_number[temp_i++] = *p;
        } else if (temp_i > 0) {
            break;
        }
    }
    if (temp_i > 0) {
        temp_number[temp_i] = '\0';
    }

    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_vertical_gradient(scr, 0x4aaee8, 0xdcf7ff);
    lvgl_card(scr, 0, 0, LCD_H_RES, 84, 0x259bd6, 0);
    lv_obj_set_style_bg_opa(lvgl_card(scr, 0, 84, LCD_H_RES, 116, 0x82d5f3, 0), LV_OPA_40, 0);
    lv_obj_t *hero = lvgl_glass_card(scr, 54, 106, 916, 224, 28);
    lv_obj_t *rain_card = lvgl_glass_card(scr, 54, 350, 206, 86, 18);
    lv_obj_t *feel_card = lvgl_glass_card(scr, 278, 350, 206, 86, 18);
    lv_obj_t *outdoor_card = lvgl_glass_card(scr, 502, 350, 206, 86, 18);
    lv_obj_t *study_card = lvgl_glass_card(scr, 726, 350, 244, 86, 18);
    lv_obj_t *advice_card = lvgl_glass_card(scr, 54, 456, 916, 104, 22);

    lv_obj_t *back = lvgl_label(scr, "返回", 32, 23, &lexin_cn_20, 0xffffff);
    lv_obj_set_style_text_letter_space(back, 1, 0);
    lvgl_label(hero, "西安天气", 36, 26, &lexin_cn_28, 0x10283e);
    lvgl_label(hero, "当前天气", 40, 74, &lexin_cn_20, 0x577489);
    lv_obj_t *temp_label = lvgl_label(hero, temp_number, 36, 96, &lv_font_montserrat_48, 0x0c253b);
    lv_obj_set_style_text_letter_space(temp_label, 1, 0);
    lv_obj_t *unit_label = lvgl_label(hero, "°C", 0, 0, &lv_font_montserrat_28, 0x395467);
    lv_obj_align_to(unit_label, temp_label, LV_ALIGN_OUT_RIGHT_MID, 12, 2);
    lvgl_label(hero, weather_cn, 40, 168, &lexin_cn_28, 0x194d69);
    lvgl_label(hero, suggestion_cn, 180, 174, &lexin_cn_20, 0x1b78a0);
    lvgl_draw_sun_cloud(scr);

    lvgl_card_border(rain_card, 0xffffff, 1);
    lvgl_label(rain_card, "降雨", 24, 14, &lexin_cn_20, 0x577489);
    lvgl_label(rain_card, rain, 24, 42, &lv_font_montserrat_28, 0x10283e);
    lvgl_label(rain_card, "短时参考", 104, 46, &lexin_cn_20, 0x577489);

    lvgl_card_border(feel_card, 0xffffff, 1);
    lvgl_label(feel_card, "体感", 24, 14, &lexin_cn_20, 0x577489);
    lvgl_label(feel_card, feel_cn, 24, 44, &lexin_cn_20, 0x10283e);

    lvgl_card_border(outdoor_card, 0xffffff, 1);
    lvgl_label(outdoor_card, "出行", 24, 14, &lexin_cn_20, 0x577489);
    lv_obj_t *outdoor_label = lvgl_label(outdoor_card, outdoor_cn, 24, 44, &lexin_cn_20, 0x10283e);
    lvgl_label_width(outdoor_label, 158);

    lvgl_card_border(study_card, 0xffffff, 1);
    lvgl_label(study_card, "学习", 24, 14, &lexin_cn_20, 0x577489);
    lv_obj_t *study_label = lvgl_label(study_card, study_cn, 24, 44, &lexin_cn_20, 0x10283e);
    lvgl_label_width(study_label, 190);

    lvgl_card_border(advice_card, 0xffffff, 1);
    lvgl_label(advice_card, "天气研判", 32, 18, &lexin_cn_28, 0x10283e);
    lvgl_label(advice_card, "进入21维本地模型", 32, 62, &lexin_cn_20, 0x577489);
    lv_obj_t *advice_label = lvgl_label(advice_card, suggestion_cn, 418, 30, &lexin_cn_28, 0x1b78a0);
    lvgl_label_width(advice_label, 440);

    lvgl_port_unlock();
    return true;
}

static void lvgl_calendar_cell(lv_obj_t *parent, int x, int y, const char *day, bool muted, bool active)
{
    lv_obj_t *cell = lvgl_card(parent, x, y, 72, 40, active ? 0x28a7e8 : 0xffffff, 12);
    lv_obj_set_style_bg_opa(cell, active ? LV_OPA_COVER : LV_OPA_0, 0);
    lv_obj_set_style_border_width(cell, active ? 0 : 1, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(0xe2eef5), 0);
    lv_obj_set_style_border_opa(cell, muted ? LV_OPA_0 : LV_OPA_40, 0);
    lv_obj_t *label = lvgl_label(cell, day, 0, 8, &lv_font_montserrat_20,
                                 active ? 0xffffff : muted ? 0xb6c4cf : 0x172a3a);
    lv_obj_set_width(label, 72);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static bool lvgl_show_calendar_result_page(const char *text)
{
    char time_value[32];
    char date[32];
    char lunar[64];
    char holiday[64];
    char day_type[32];
    copy_field_value(text, "TIME:", time_value, sizeof(time_value));
    copy_field_value(text, "DATE:", date, sizeof(date));
    copy_field_value(text, "LUNAR:", lunar, sizeof(lunar));
    copy_field_value(text, "HOLIDAY:", holiday, sizeof(holiday));
    copy_field_value(text, "DAY_TYPE:", day_type, sizeof(day_type));
    const char *lunar_cn = lunar_to_cn(lunar);
    const char *holiday_cn = holiday_to_cn(holiday);
    int year = 2026;
    int month = 5;
    int day = 30;
    if (!parse_date_parts(date, &year, &month, &day) || !valid_date_parts(year, month, day)) {
        snprintf(date, sizeof(date), "2026-05-30");
        year = 2026;
        month = 5;
        day = 30;
    }
    const char *effective_day_type = calendar_effective_day_type(day_type, holiday_cn, year, month, day);
    const char *day_type_cn = calendar_day_type_cn(holiday_cn, effective_day_type);
    const char *suggestion_cn = calendar_pet_suggestion(time_value, holiday_cn, effective_day_type);
    update_pet_tip_from_calendar(time_value, holiday_cn, effective_day_type);
    int first_weekday = weekday_monday0(year, month, 1);
    int month_days = days_in_month(year, month);
    int prev_month = month == 1 ? 12 : month - 1;
    int prev_year = month == 1 ? year - 1 : year;
    int prev_days = days_in_month(prev_year, prev_month);

    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_vertical_gradient(scr, 0x71d6e8, 0xf4fbff);
    lvgl_card(scr, 0, 0, LCD_H_RES, 84, 0x26a9d5, 0);
    lv_obj_set_style_bg_opa(lvgl_card(scr, 0, 84, LCD_H_RES, 110, 0x9be5f2, 0), LV_OPA_40, 0);
    lv_obj_t *main_card = lvgl_glass_card(scr, 54, 106, 916, 344, 28);
    lv_obj_t *date_card = lvgl_glass_card(scr, 66, 500, 214, 78, 18);
    lv_obj_t *lunar_card = lvgl_glass_card(scr, 300, 500, 214, 78, 18);
    lv_obj_t *holiday_card = lvgl_glass_card(scr, 534, 500, 166, 78, 18);
    lv_obj_t *suggestion_card = lvgl_glass_card(scr, 720, 500, 238, 78, 18);

    lv_obj_t *back = lvgl_label(scr, "返回", 32, 23, &lexin_cn_20, 0xffffff);
    lv_obj_set_style_text_letter_space(back, 1, 0);
    lvgl_label(main_card, "提醒服务", 32, 30, &lexin_cn_28, 0x10283e);
    lvgl_label(main_card, month_name_cn(month), 32, 68, &lexin_cn_20, 0x667784);
    lvgl_label(main_card, time_value, 734, 34, &lv_font_montserrat_32, 0x1b8ed2);

    const char *week[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    const int start_x = 34;
    const int start_y = 124;
    const int cell_w = 126;
    const int cell_h = 34;
    for (int i = 0; i < 7; i++) {
        lv_obj_t *week_label = lvgl_label(main_card, week[i], start_x + i * cell_w, 102, &lexin_cn_20, 0x667784);
        lv_obj_set_width(week_label, 72);
        lv_obj_set_style_text_align(week_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    for (int i = 0; i < 42; i++) {
        int shown_day = 0;
        bool muted = false;
        bool active = false;
        if (i < first_weekday) {
            shown_day = prev_days - first_weekday + i + 1;
            muted = true;
        } else if (i < first_weekday + month_days) {
            shown_day = i - first_weekday + 1;
            active = shown_day == day;
        } else {
            shown_day = i - first_weekday - month_days + 1;
            muted = true;
        }
        char day_text[16];
        snprintf(day_text, sizeof(day_text), "%d", shown_day);
        lvgl_calendar_cell(main_card, start_x + (i % 7) * cell_w, start_y + (i / 7) * cell_h,
                           day_text, muted, active);
    }

    lvgl_card_border(date_card, 0xffffff, 1);
    lvgl_label(date_card, "日期", 20, 12, &lexin_cn_20, 0x606a76);
    lv_obj_t *date_label = lvgl_label(date_card, date, 20, 34, &lv_font_montserrat_20, 0x151b24);
    lvgl_label_width(date_label, 172);

    lvgl_card_border(lunar_card, 0xffffff, 1);
    lvgl_label(lunar_card, "农历", 20, 12, &lexin_cn_20, 0x606a76);
    lv_obj_t *lunar_label = lvgl_label(lunar_card, lunar_cn, 20, 34, &lexin_cn_20, 0x2f86ff);
    lvgl_label_width(lunar_label, 172);

    lvgl_card_border(holiday_card, 0xffffff, 1);
    lvgl_label(holiday_card, "日期类型", 20, 12, &lexin_cn_20, 0x606a76);
    lv_obj_t *holiday_label = lvgl_label(holiday_card, day_type_cn, 20, 34, &lexin_cn_20, 0x151b24);
    lvgl_label_width(holiday_label, 124);

    lvgl_card_border(suggestion_card, 0xffffff, 1);
    lvgl_label(suggestion_card, "今日安排", 20, 12, &lexin_cn_20, 0x606a76);
    lv_obj_t *suggestion_label = lvgl_label(suggestion_card, suggestion_cn, 20, 34, &lexin_cn_20, 0x10283e);
    lvgl_label_width(suggestion_label, 196);

    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_error_page(lexin_action_id_t action_id)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_bg(scr, 0xfff5f5);
    lvgl_label(scr, "返回", 32, 26, &lexin_cn_20, 0x5a1f1f);
    lvgl_label(scr, action_id == LEXIN_ACTION_WEATHER ? "天气错误" :
               action_id == LEXIN_ACTION_TIME ? "日程错误" : "AI错误",
               72, 154, &lexin_cn_28, 0xc03434);
    lvgl_label(scr, "检查网络后再试", 76, 238, &lexin_cn_28, 0x7a4444);
    lvgl_port_unlock();
    return true;
}

static void screen_backlight_on(void)
{
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT_GPIO, LCD_BACKLIGHT_ON_LEVEL));
}

static void init_lcd(esp_lcd_panel_handle_t *out_panel)
{
    gpio_config_t rst_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_RESET_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_gpio_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_RESET_GPIO, 0));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(gpio_set_level(LCD_RESET_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_LOGI(TAG, "Power MIPI DSI PHY");
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = LCD_PHY_LDO_CHAN,
        .voltage_mv = LCD_PHY_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &s_mipi_phy_ldo));

    ESP_LOGI(TAG, "Create MIPI DSI bus");
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = LCD_DSI_LANES,
        .lane_bit_rate_mbps = 1000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

    ESP_LOGI(TAG, "Create MIPI DBI command IO");
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));

    ESP_LOGI(TAG, "Create EK79007 panel");
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 48,
        .in_color_format = LCD_COLOR_FMT_RGB888,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_back_porch = 120,
            .hsync_pulse_width = 10,
            .hsync_front_porch = 120,
            .vsync_back_porch = 20,
            .vsync_pulse_width = 1,
            .vsync_front_porch = 20,
        },
    };
    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = LCD_DSI_LANES,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(dbi_io, &panel_config, out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*out_panel));
    screen_backlight_on();
}

static bool init_touch(void)
{
    ESP_LOGI(TAG, "Use shared BSP I2C bus SDA=%d SCL=%d", TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BSP I2C init failed: %s", esp_err_to_name(ret));
        return false;
    }
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (!i2c_bus) {
        ESP_LOGE(TAG, "BSP I2C handle is unavailable");
        return false;
    }

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = TOUCH_I2C_FREQ_HZ;
    ret = esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch I2C panel IO init failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    vTaskDelay(pdMS_TO_TICKS(180));
    for (int attempt = 1; attempt <= 6; ++attempt) {
        ret = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GT911 ready after attempt %d", attempt);
            return true;
        }
        ESP_LOGW(TAG, "GT911 init attempt %d/6 failed: %s",
                 attempt, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(180));
    }
    ESP_LOGE(TAG, "GT911 unavailable; keep display and AI services running");
    return false;
}

static void touch_task(void *arg)
{
    bool pressed = false;
    TickType_t last_trigger_tick = 0;

    while (1) {
        esp_lcd_touch_read_data(s_touch);

        uint16_t x[1];
        uint16_t y[1];
        uint16_t strength[1];
        uint8_t point_count = 0;
        bool touched = esp_lcd_touch_get_coordinates(s_touch, x, y, strength, &point_count, 1);

        if (touched && point_count > 0 && !pressed) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_trigger_tick > pdMS_TO_TICKS(TOUCH_RELEASE_MS)) {
                lexin_action_id_t action_id = x[0] < (LCD_H_RES / 2)
                    ? LEXIN_ACTION_WEATHER
                    : LEXIN_ACTION_TIME;
                lexin_touch_event_t event = lexin_launcher_handle_touch(x[0], y[0]);
                if (event.result == LEXIN_TOUCH_BACK) {
                    ESP_LOGI(TAG, "touch x=%u y=%u -> launcher", x[0], y[0]);
                    lexin_screen_show_idle();
                } else if (event.result == LEXIN_TOUCH_OPEN_APP && event.has_action) {
                    action_id = event.action_id;
                    lexin_interaction_record_action(action_id);
                    ESP_LOGI(TAG, "touch x=%u y=%u -> %s", x[0], y[0],
                             action_id == LEXIN_ACTION_WEATHER ? "weather" :
                             action_id == LEXIN_ACTION_TIME ? "calendar" : "ai_insight");
                    lexin_screen_show_querying(action_id);
                    lexin_enqueue_trigger("touch", action_id, action_id);
                } else if (event.result == LEXIN_TOUCH_OPEN_APP) {
                    lexin_interaction_record_screen(event.screen);
                    if (event.screen == LEXIN_SCREEN_PET) {
                        ESP_LOGI(TAG, "touch x=%u y=%u -> pet ai page", x[0], y[0]);
                        lvgl_show_pet_ai_page();
                    } else if (event.screen == LEXIN_SCREEN_EMOTION) {
                        ESP_LOGI(TAG, "touch x=%u y=%u -> emotion page", x[0], y[0]);
                        lvgl_show_emotion_page();
                    } else {
                        ESP_LOGI(TAG, "touch x=%u y=%u -> suggestion page", x[0], y[0]);
                        lvgl_show_suggestion_page();
                    }
                }
                last_trigger_tick = now;
            }
            pressed = true;
        } else if (!touched || point_count == 0) {
            pressed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

static void screen_ui_task(void *arg)
{
    esp_lcd_panel_handle_t panel = NULL;
    init_lcd(&panel);
    s_panel = panel;
    lvgl_init_display();
    lexin_launcher_init();
    lexin_interaction_init();
    lexin_screen_show_idle();
    bool touch_ready = init_touch();

    ESP_LOGI(TAG, "Touch UI ready: launcher apps (touch=%d)", touch_ready);
    if (touch_ready) {
        xTaskCreate(touch_task, "touch_task", 4096, NULL, 5, NULL);
    }
    vTaskDelete(NULL);
}

void lexin_start_screen_ui(void)
{
    xTaskCreate(screen_ui_task, "screen_ui", 8192, NULL, 4, NULL);
}

void lexin_screen_show_idle(void)
{
    lexin_launcher_show_launcher();
    lvgl_show_launcher();
}

void lexin_screen_show_querying(lexin_action_id_t action_id)
{
    update_pet_tip_querying(action_id);
    if (action_id == LEXIN_ACTION_WEATHER) {
        lexin_launcher_show_screen(LEXIN_SCREEN_WEATHER);
        if (s_cached_weather[0] != '\0') {
            lvgl_show_weather_result_page(s_cached_weather);
        } else {
            lvgl_show_querying_page(action_id);
        }
    } else if (action_id == LEXIN_ACTION_TIME) {
        lexin_launcher_show_screen(LEXIN_SCREEN_CALENDAR);
        if (s_cached_calendar[0] != '\0') {
            lvgl_show_calendar_result_page(s_cached_calendar);
        } else {
            lvgl_show_querying_page(action_id);
        }
    } else {
        lexin_launcher_show_screen(LEXIN_SCREEN_SUGGESTION);
        lvgl_show_querying_page(action_id);
    }
}

void lexin_screen_show_result(lexin_action_id_t action_id)
{
    (void)action_id;
}

void lexin_screen_show_result_text(lexin_action_id_t action_id, const char *text)
{
    if (action_id == LEXIN_ACTION_WEATHER) {
        snprintf(s_cached_weather, sizeof(s_cached_weather), "%s", text ? text : "");
        if (lexin_launcher_current_screen() == LEXIN_SCREEN_WEATHER) {
            lvgl_show_weather_result_page(s_cached_weather);
        }
    } else if (action_id == LEXIN_ACTION_TIME) {
        snprintf(s_cached_calendar, sizeof(s_cached_calendar), "%s", text ? text : "");
        if (lexin_launcher_current_screen() == LEXIN_SCREEN_CALENDAR) {
            lvgl_show_calendar_result_page(s_cached_calendar);
        }
    } else {
        update_pet_tip_from_insight(text);
        if (lexin_launcher_current_screen() == LEXIN_SCREEN_SUGGESTION) {
            lvgl_show_suggestion_page();
        }
    }
}

void lexin_screen_show_error(lexin_action_id_t action_id)
{
    lexin_screen_id_t expected = action_id == LEXIN_ACTION_WEATHER ?
        LEXIN_SCREEN_WEATHER : action_id == LEXIN_ACTION_TIME ?
        LEXIN_SCREEN_CALENDAR : LEXIN_SCREEN_SUGGESTION;
    if (lexin_launcher_current_screen() == expected) {
        lvgl_show_error_page(action_id);
    }
}

void lexin_screen_update_ai_context(lexin_face_state_t face, lexin_emotion_state_t emotion)
{
    (void)face;
    s_emotion_state = emotion;
    update_pet_from_ai_context();
}

void lexin_screen_update_vision_context(const lexin_vision_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    lexin_face_state_t face = snapshot->face_detected ?
        LEXIN_FACE_DETECTED : LEXIN_FACE_NOT_DETECTED;
    lexin_emotion_state_t emotion = LEXIN_EMOTION_UNKNOWN;
    switch (snapshot->expression) {
    case LEXIN_VISION_EXPRESSION_HAPPY:
        emotion = LEXIN_EMOTION_HAPPY;
        break;
    case LEXIN_VISION_EXPRESSION_SAD:
        emotion = LEXIN_EMOTION_TIRED;
        break;
    case LEXIN_VISION_EXPRESSION_NEUTRAL:
        emotion = LEXIN_EMOTION_NEUTRAL;
        break;
    case LEXIN_VISION_EXPRESSION_UNKNOWN:
    default:
        break;
    }
    lexin_screen_update_ai_context(face, emotion);

    if (lexin_launcher_current_screen() == LEXIN_SCREEN_EMOTION &&
        snapshot->updated_at_ms - s_last_vision_page_refresh_ms >= 120) {
        s_last_vision_page_refresh_ms = snapshot->updated_at_ms;
        lvgl_refresh_emotion_live(snapshot);
    }
}
