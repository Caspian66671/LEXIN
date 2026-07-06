#include "lexin_display_test.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "bsp/touch.h"
#include "lexin_actions.h"
#include "lexin_interaction.h"
#include "lexin_launcher.h"
#include "lexin_triggers.h"
#include "lexin_wifi.h"

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
static lv_obj_t *s_emotion_capture_status_label;
static char s_capture_status[64] = "CAPTURE IDLE";
typedef enum {
    EMOTION_VIEW_LIVE = 0,
    EMOTION_VIEW_DAILY,
    EMOTION_VIEW_MONTHLY,
} emotion_view_t;
static emotion_view_t s_emotion_view = EMOTION_VIEW_LIVE;
static char s_cached_weather[RESULT_CACHE_SIZE];
static char s_cached_calendar[RESULT_CACHE_SIZE];

#define LEXIN_PLAN_MAX_ITEMS 12
#define LEXIN_PLAN_ITEM_LEN 48
static char s_plan_items[LEXIN_PLAN_MAX_ITEMS][LEXIN_PLAN_ITEM_LEN];
static bool s_plan_done[LEXIN_PLAN_MAX_ITEMS];
static int s_plan_count;
static int s_plan_percent;
static bool s_plan_recording;      /* waiting for a spoken plan */
static bool s_plan_day_view;       /* plan page is showing a specific day */
/* Date shown in day view; parsed from the proxy's DATE field so that
 * toggle/delete on a past day are persisted to that day, not today. */
static int s_plan_view_year, s_plan_view_month, s_plan_view_day;
static char s_plan_day_title[24];  /* header text for the day-record view */
/* Completion percent per day-of-month (1..31). 255 = no plan that day. */
static uint8_t s_plan_month_percent[32];
/* Voice conversation state mirrored from the voice component. */
static lexin_voice_snapshot_t s_voice_snapshot;
static bool s_voice_snapshot_valid;
static lv_obj_t *s_voice_status_label;
static lv_obj_t *s_voice_reply_label;
static lv_obj_t *s_voice_transcript_label;
static lv_obj_t *s_voice_state_pill;
static lv_obj_t *s_voice_meter_label;
static lv_obj_t *s_voice_backend_label;
static const char *s_voice_banner_state = "等待唤醒";
static uint32_t s_voice_last_reply_ms;
static lexin_wifi_ap_t s_wifi_aps[LEXIN_WIFI_MAX_APS];
static uint16_t s_wifi_ap_count;
static int s_wifi_selected = -1;
static char s_wifi_password[LEXIN_WIFI_PASSWORD_MAX_LEN + 1];
static char s_wifi_page_status[96] = "Tap Scan to search networks";
static bool s_wifi_shift;
static bool s_wifi_symbols;
static bool s_wifi_show_password;

static void copy_field_value(const char *text, const char *key, char *out, size_t out_size);
static void copy_field_raw_value(const char *text, const char *key, char *out, size_t out_size);
static int parse_percent_value(const char *text);
static int parse_hour_value(const char *time_value);
static void refresh_pet_combined_tip(void);
static bool lvgl_show_pet_ai_page(void);
static bool lvgl_show_suggestion_page(void);
static bool lvgl_show_emotion_page(void);
static bool lvgl_show_wifi_page(void);
static bool lvgl_show_voice_page(void);
static bool lvgl_show_emotion_report_page(const char *text, bool monthly);
static void lvgl_refresh_voice_page(void);
static void lvgl_draw_voice_banner(lv_obj_t *parent, int x, int y, int w, int h);
static const char *voice_state_cn(lexin_voice_state_t state);
static uint32_t voice_state_color(lexin_voice_state_t state);
static const char *voice_backend_text(uint8_t backend);
static bool handle_wifi_touch(uint16_t x, uint16_t y);
static bool handle_emotion_touch(uint16_t x, uint16_t y);
static bool handle_plan_touch(uint16_t x, uint16_t y);
static bool handle_calendar_touch(uint16_t x, uint16_t y);
static bool lvgl_show_plan_page(void);
static void lvgl_draw_plant(lv_obj_t *parent, int x, int y, int w, int h, int percent);
static void parse_plan_text(const char *text);
static bool launcher_lock_touch_hit(uint16_t raw_x, uint16_t raw_y);
static bool refresh_emotion_preview(void);
static bool lvgl_refresh_emotion_live(const lexin_vision_snapshot_t *snapshot);
static void update_pet_from_ai_context(void);

static void clear_emotion_live_widget_refs(void)
{
    s_emotion_preview_image = NULL;
    s_emotion_waiting_label = NULL;
    s_emotion_face_box = NULL;
    s_emotion_camera_meta_label = NULL;
    s_emotion_expression_label = NULL;
    s_emotion_meta_label = NULL;
    s_emotion_system_meta_label = NULL;
    s_emotion_response_label = NULL;
    s_emotion_online_label = NULL;
    s_emotion_capture_status_label = NULL;
}

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

static void copy_field_raw_value(const char *text, const char *key, char *out, size_t out_size)
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
        out[i++] = *p++;
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

/* ----------------------------------------------------------------- */
/* Voice conversation UI helpers                                      */
/* ----------------------------------------------------------------- */

static const char *voice_state_cn(lexin_voice_state_t state)
{
    switch (state) {
    case LEXIN_VOICE_STATE_LISTENING:  return "待命中";
    case LEXIN_VOICE_STATE_HEARD:      return "正在听";
    case LEXIN_VOICE_STATE_UPLOADING:  return "上传中";
    case LEXIN_VOICE_STATE_THINKING:   return "思考中";
    case LEXIN_VOICE_STATE_REPLY:      return "已回复";
    case LEXIN_VOICE_STATE_ERROR:      return "异常";
    case LEXIN_VOICE_STATE_IDLE:
    default:                           return "等待唤醒";
    }
}

static uint32_t voice_state_color(lexin_voice_state_t state)
{
    switch (state) {
    case LEXIN_VOICE_STATE_HEARD:     return 0x42c2a3;
    case LEXIN_VOICE_STATE_UPLOADING: return 0xff9f22;
    case LEXIN_VOICE_STATE_THINKING:  return 0x9465ff;
    case LEXIN_VOICE_STATE_REPLY:     return 0x1c98d2;
    case LEXIN_VOICE_STATE_ERROR:     return 0xd9534f;
    case LEXIN_VOICE_STATE_LISTENING:
    case LEXIN_VOICE_STATE_IDLE:
    default:                          return 0x71889a;
    }
}

static const char *voice_backend_text(uint8_t backend)
{
    switch (backend) {
    case 1: return "本地规则";
    case 2: return "DeepSeek";
    default: return "未连接";
    }
}

/* ----------------------------------------------------------------- */
/*  Lock screen (face auth) UI                                        */
/* ----------------------------------------------------------------- */

static lexin_face_auth_snapshot_t s_lock_snapshot;
static lv_obj_t *s_lock_preview_image;
static lv_obj_t *s_lock_status_label;
static lv_obj_t *s_lock_sub_label;
static lv_obj_t *s_lock_face_box;
static lv_obj_t *s_lock_action_btn;
static lv_obj_t *s_lock_preview_frame;
static lv_obj_t *s_lock_name_input_label;
static char s_lock_name_buf[32];
static int s_lock_name_len;
static bool s_lock_register_active;
/* True while the lock screen is the content actually painted on the
 * display. Used to repaint the launcher exactly once after unlock
 * instead of on every recognized-face snapshot. */
static bool s_lock_screen_visible;

static void lock_clear_widget_refs(void)
{
    s_lock_preview_image = NULL;
    s_lock_status_label = NULL;
    s_lock_sub_label = NULL;
    s_lock_face_box = NULL;
    s_lock_action_btn = NULL;
    s_lock_preview_frame = NULL;
    s_lock_name_input_label = NULL;
}

static const char *lock_state_text(lexin_face_auth_state_t state)
{
    switch (state) {
    case LEXIN_FACE_AUTH_SCANNING:    return "Scanning face";
    case LEXIN_FACE_AUTH_DETECTED:    return "Checking face";
    case LEXIN_FACE_AUTH_RECOGNIZED:  return "Unlocked";
    case LEXIN_FACE_AUTH_UNKNOWN:     return "Unknown user";
    case LEXIN_FACE_AUTH_REGISTERING: return "Registering";
    case LEXIN_FACE_AUTH_REGISTERED:  return "Registered";
    case LEXIN_FACE_AUTH_ERROR:       return "Face auth error";
    case LEXIN_FACE_AUTH_IDLE:
    default:                          return "Camera starting";
    }
}

static const char *lock_state_sub_text(lexin_face_auth_state_t state)
{
    switch (state) {
    case LEXIN_FACE_AUTH_RECOGNIZED:  return "Entering home";
    case LEXIN_FACE_AUTH_UNKNOWN:     return "Tap Create User to register";
    case LEXIN_FACE_AUTH_REGISTERING: return "Keep your face in frame";
    case LEXIN_FACE_AUTH_REGISTERED:  return "Entering home";
    case LEXIN_FACE_AUTH_ERROR:       return "Check WiFi and proxy";
    default:                          return "Center your face in the frame";
    }
}

static bool lock_can_create_user(const lexin_face_auth_snapshot_t *s)
{
    if (!s || s->recognized) return false;
    if (s->state == LEXIN_FACE_AUTH_RECOGNIZED ||
        s->state == LEXIN_FACE_AUTH_REGISTERING ||
        s->state == LEXIN_FACE_AUTH_REGISTERED) {
        return false;
    }
    if (s->state == LEXIN_FACE_AUTH_UNKNOWN) return true;
    return s->face_detected &&
           s->face_w >= 48 &&
           s->face_h >= 48;
}

static bool lock_has_preview_pixels(void)
{
    return s_emotion_preview_pixels != NULL;
}

static void lock_commit_preview(void)
{
    uint16_t *front = s_emotion_preview_pixels;
    s_emotion_preview_pixels = s_emotion_preview_back_pixels;
    s_emotion_preview_back_pixels = front;
    s_emotion_preview_dsc.data = (const uint8_t *)s_emotion_preview_pixels;
}

static bool lvgl_show_lock_screen(void)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) return false;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lock_clear_widget_refs();
    s_lock_register_active = false;
    s_lock_screen_visible = true;

    lvgl_set_vertical_gradient(scr, 0x06111c, 0x0b2635);
    /* Header */
    lvgl_card(scr, 0, 0, LCD_H_RES, 76, 0x0b3650, 0);
    lvgl_set_vertical_gradient(lv_obj_get_child(scr, 0), 0x0a2740, 0x086179);
    lvgl_label(scr, "LEXIN LOCK", 32, 22, &lv_font_montserrat_28, 0xffffff);
    lvgl_label(scr, "请正对摄像头进行人脸识别", 280, 26, &lexin_cn_20, 0x99f3ff);

    lv_obj_t *header_mask = lvgl_card(scr, 270, 18, 700, 42, 0x0b3650, 0);
    lv_obj_set_style_bg_opa(header_mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header_mask, 0, 0);
    lvgl_label(header_mask, "Look at the camera to unlock", 10, 10,
               &lv_font_montserrat_20, 0x99f3ff);

    lv_obj_t *wifi_btn = lvgl_card(scr, 820, 18, 160, 46, 0xffffff, 23);
    lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_90, 0);
    lvgl_card_border(wifi_btn, 0xffffff, 1);
    lvgl_center_label(wifi_btn, "WiFi", 0, 10, 160, &lv_font_montserrat_20, 0x19afd8);

    /* Camera preview */
    s_lock_preview_frame = lvgl_card(scr, 310, 104, 404, 330, 0x0c3047, 8);
    lv_obj_set_style_border_width(s_lock_preview_frame, 2, 0);
    lv_obj_set_style_border_color(s_lock_preview_frame, lv_color_hex(0x1cc9d5), 0);
    lvgl_label(s_lock_preview_frame, "FACE ID", 16, 12, &lv_font_montserrat_20, 0xb8f7ff);

    lv_obj_t *inner = lvgl_card(s_lock_preview_frame, 50, 40,
                                 LEXIN_VISION_PREVIEW_WIDTH + 8,
                                 LEXIN_VISION_PREVIEW_HEIGHT + 8, 0x061018, 4);
    lv_obj_set_style_border_width(inner, 2, 0);
    lv_obj_set_style_border_color(inner, lv_color_hex(0x5cf6ff), 0);

    if (lock_has_preview_pixels()) {
        lv_obj_t *img = lv_image_create(s_lock_preview_frame);
        lv_image_set_src(img, &s_emotion_preview_dsc);
        lv_obj_set_pos(img, 54, 44);
        s_lock_preview_image = img;
    }
    s_lock_face_box = lv_obj_create(s_lock_preview_frame);
    lv_obj_remove_style_all(s_lock_face_box);
    lv_obj_set_style_bg_opa(s_lock_face_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_lock_face_box, 3, 0);
    lv_obj_set_style_border_color(s_lock_face_box, lv_color_hex(0xffe45c), 0);
    lv_obj_set_style_radius(s_lock_face_box, 4, 0);
    lv_obj_add_flag(s_lock_face_box, LV_OBJ_FLAG_HIDDEN);

    /* Status area */
    s_lock_status_label = lvgl_label(scr, "正在初始化摄像头",
                                      64, 156, &lexin_cn_28, 0xffffff);
    lvgl_label_width(s_lock_status_label, 220);
    s_lock_sub_label = lvgl_label(scr, "请耐心等待",
                                    64, 210, &lexin_cn_20, 0x6fa2b5);
    lvgl_label_width(s_lock_sub_label, 220);
    lv_label_set_text(s_lock_status_label, lock_state_text(s_lock_snapshot.state));
    lv_label_set_text(s_lock_sub_label, lock_state_sub_text(s_lock_snapshot.state));

    /* Action button (hidden until needed) */
    s_lock_action_btn = lvgl_card(scr, 64, 280, 200, 56, 0x20d9d2, 14);
    lv_obj_set_style_bg_opa(s_lock_action_btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_lock_action_btn, LV_OBJ_FLAG_HIDDEN);
    lvgl_label(s_lock_action_btn, "创建新用户", 40, 16, &lexin_cn_28, 0x06283a);

    lv_obj_t *action_mask = lvgl_card(s_lock_action_btn, 8, 8, 184, 40, 0x20d9d2, 0);
    lv_obj_set_style_bg_opa(action_mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(action_mask, 0, 0);
    lvgl_label(action_mask, "Create User", 24, 10, &lv_font_montserrat_20, 0x06283a);

    s_lock_name_len = 0;
    s_lock_name_buf[0] = 0;

    lvgl_port_unlock();
    return true;
}

static void lock_update_face_box(const lexin_face_auth_snapshot_t *s)
{
    if (!s_lock_face_box) return;
    if (!s->face_detected || s->face_w < 8 || s->face_h < 8) {
        lv_obj_add_flag(s_lock_face_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_lock_face_box, LV_OBJ_FLAG_HIDDEN);
    int bx = 54 + s->face_x;
    int by = 44 + s->face_y;
    int bw = s->face_w;
    int bh = s->face_h;
    /* Clamp */
    int max_x = 54 + LEXIN_VISION_PREVIEW_WIDTH;
    int max_y = 44 + LEXIN_VISION_PREVIEW_HEIGHT;
    if (bx < 54) { bw -= (54 - bx); bx = 54; }
    if (by < 44) { bh -= (44 - by); by = 44; }
    if (bx + bw > max_x) bw = max_x - bx;
    if (by + bh > max_y) bh = max_y - by;
    if (bw < 12) bw = 12;
    if (bh < 12) bh = 12;
    lv_obj_set_pos(s_lock_face_box, bx, by);
    lv_obj_set_size(s_lock_face_box, bw, bh);
}

static void lock_show_action_button(const char *label)
{
    (void)label;
    if (!s_lock_action_btn) return;
    lv_obj_remove_flag(s_lock_action_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *btn_label = lv_obj_get_child(s_lock_action_btn, 0);
    if (btn_label) lv_label_set_text(btn_label, "Create User");
}

static void lock_hide_action_button(void)
{
    if (s_lock_action_btn) lv_obj_add_flag(s_lock_action_btn, LV_OBJ_FLAG_HIDDEN);
}

static const char lock_kb_rows[4][10] = {
    {'q','w','e','r','t','y','u','i','o','p'},
    {'a','s','d','f','g','h','j','k','l', 0 },
    {'z','x','c','v','b','n','m','-','_', 0 },
    {0},
};

static bool lvgl_show_register_screen(void)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) return false;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lock_clear_widget_refs();
    s_lock_register_active = true;

    lvgl_set_vertical_gradient(scr, 0xfff5fa, 0xffd9ea);
    lvgl_card(scr, 0, 0, LCD_H_RES, 76, 0xff4f8a, 0);
    lvgl_label(scr, "新用户注册", 32, 22, &lexin_cn_28, 0xffffff);
    lvgl_label(scr, "请输入你的名字", 280, 26, &lexin_cn_20, 0xffd9ea);

    /* Name display */
    lv_obj_t *name_card = lvgl_glass_card(scr, 64, 106, 896, 60, 16);
    s_lock_name_input_label = lvgl_label(name_card, s_lock_name_buf[0] ? s_lock_name_buf : "点击键盘输入",
                                           24, 16, &lexin_cn_28, 0x10283e);
    lvgl_label_width(s_lock_name_input_label, 840);

    /* Keyboard */
    lv_obj_t *kb = lvgl_glass_card(scr, 64, 186, 896, 280, 16);
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 10; col++) {
            char ch = lock_kb_rows[row][col];
            if (!ch) break;
            int kx = 30 + col * 85;
            int ky = 20 + row * 80;
            lv_obj_t *key = lvgl_card(kb, kx, ky, 74, 60, 0xffffff, 12);
            lv_obj_set_style_border_width(key, 1, 0);
            lv_obj_set_style_border_color(key, lv_color_hex(0xdddddd), 0);
            char txt[2] = {ch, 0};
            lvgl_label(key, txt, 26, 16, &lv_font_montserrat_28, 0x10283e);
        }
    }
    /* Space bar */
    lv_obj_t *space = lvgl_card(kb, 170, 260, 340, 50, 0xffffff, 12);
    lvgl_label(space, "空格", 136, 14, &lexin_cn_20, 0x577489);
    /* Backspace */
    lv_obj_t *bs = lvgl_card(kb, 530, 260, 150, 50, 0xffffff, 12);
    lvgl_label(bs, "删除", 46, 14, &lexin_cn_20, 0xd9534f);

    /* Confirm button */
    lv_obj_t *confirm = lvgl_card(scr, 350, 488, 300, 64, 0xff4f8a, 18);
    lv_obj_t *cfm_label = lvgl_label(confirm, "确认创建", 90, 18, &lexin_cn_28, 0xffffff);

    lvgl_port_unlock();
    return true;
}

static void lock_handle_register_touch(uint16_t x, uint16_t y)
{
    /* Keyboard region: card at x=64, y=186, w=896, h=280 */
    if (x >= 64 && x < 64 + 896 && y >= 186 && y < 186 + 280) {
        int col = ((int)x - 64 - 30) / 85;
        int row = ((int)y - 186 - 20) / 80;
        if (row >= 0 && row < 3 && col >= 0 && col < 10) {
            char ch = lock_kb_rows[row][col];
            if (ch && s_lock_name_len < (int)(sizeof(s_lock_name_buf) - 1)) {
                s_lock_name_buf[s_lock_name_len++] = ch;
                s_lock_name_buf[s_lock_name_len] = 0;
            }
        }
        /* Space bar: 170,260,w=340,h=50 in kb coords => abs x=234, y=446 */
        if ((int)x >= 234 && (int)x < 574 && (int)y >= 446 && (int)y < 496) {
            if (s_lock_name_len < (int)(sizeof(s_lock_name_buf) - 1)) {
                s_lock_name_buf[s_lock_name_len++] = ' ';
                s_lock_name_buf[s_lock_name_len] = 0;
            }
        }
        /* Backspace: 530,260,w=150,h=50 => abs x=594, y=446 */
        if ((int)x >= 594 && (int)x < 744 && (int)y >= 446 && (int)y < 496) {
            if (s_lock_name_len > 0) s_lock_name_buf[--s_lock_name_len] = 0;
        }
        if (s_lock_name_input_label) {
            if (lvgl_port_lock(80)) {
                lv_label_set_text(s_lock_name_input_label,
                                  s_lock_name_buf[0] ? s_lock_name_buf : "Input user name");
                lvgl_port_unlock();
            }
        }
    }
    /* Confirm button: 350,488,w=300,h=64 */
    if ((int)x >= 350 && (int)x < 650 && (int)y >= 488 && (int)y < 552 && s_lock_name_len > 0) {
        esp_err_t ret = lexin_face_auth_register(s_lock_name_buf);
        s_lock_register_active = false;
        lvgl_show_lock_screen();
        s_lock_name_len = 0;
        s_lock_name_buf[0] = 0;
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "face register request failed: %s", esp_err_to_name(ret));
        }
    }
}

void lexin_screen_update_face_auth(const lexin_face_auth_snapshot_t *s)
{
    if (!s) return;
    s_lock_snapshot = *s;
    bool should_unlock =
        s->recognized ||
        s->state == LEXIN_FACE_AUTH_RECOGNIZED ||
        s->state == LEXIN_FACE_AUTH_REGISTERED;

    if (s_lock_register_active) {
        if (should_unlock) {
            s_lock_register_active = false;
            lexin_screen_show_idle();
        }
        return;
    }

    if (!s_lvgl_ready || !lvgl_port_lock(200)) {
        /* LVGL is busy (e.g. painting a result page). Never repaint
         * blindly here: this callback fires for every recognized-face
         * snapshot, and an unconditional show_idle() would stomp
         * whatever page is currently being drawn. The next snapshot
         * (or the unlock path in the main task) will catch up. */
        return;
    }

    /* If the launcher is already shown, just keep the active screen.
     * Keep the LVGL lock held while deciding and redrawing so a touch
     * that opens another page cannot slip in between. */
    lexin_screen_id_t cur = lexin_launcher_current_screen();
    if (lexin_face_auth_is_logged_in()) {
        /* Repaint the launcher only for the actual unlock transition
         * (lock screen still painted). Recognized-face snapshots keep
         * arriving while the user sits in front of the camera; without
         * this guard every one of them would redraw the launcher. */
        if (should_unlock && cur == LEXIN_SCREEN_LAUNCHER &&
            s_lock_screen_visible) {
            lexin_screen_show_idle();
        }
        lvgl_port_unlock();
        return;
    }
    if (cur == LEXIN_SCREEN_WIFI) {
        if (should_unlock) {
            lexin_screen_show_idle();
        }
        lvgl_port_unlock();
        return;
    }
    if (cur != LEXIN_SCREEN_LAUNCHER && cur != LEXIN_SCREEN_VOICE &&
        cur != LEXIN_SCREEN_EMOTION) {
        lvgl_port_unlock();
        return;
    }

    /* Lock screen: if still in launcher (not yet unlocked). */
    if (cur == LEXIN_SCREEN_LAUNCHER && !lexin_face_auth_is_logged_in()) {
        switch (s->state) {
        case LEXIN_FACE_AUTH_SCANNING:
            break;
        case LEXIN_FACE_AUTH_DETECTED:
        case LEXIN_FACE_AUTH_RECOGNIZED:
        case LEXIN_FACE_AUTH_UNKNOWN:
        case LEXIN_FACE_AUTH_REGISTERED:
        case LEXIN_FACE_AUTH_ERROR:
            break;
        default: break;
        }
    }

    /* Refresh preview. The first call allocates the shared preview buffer. */
    bool preview_ready = refresh_emotion_preview();
    if (preview_ready && lock_has_preview_pixels()) {
        lock_commit_preview();
        if (!s_lock_preview_image && s_lock_preview_frame) {
            s_lock_preview_image = lv_image_create(s_lock_preview_frame);
            lv_obj_set_pos(s_lock_preview_image, 54, 44);
        }
        if (s_lock_preview_image) {
            lv_image_set_src(s_lock_preview_image, &s_emotion_preview_dsc);
            lv_obj_invalidate(s_lock_preview_image);
        }
    }

    lock_update_face_box(s);

    /* Status text */
    const char *status = s->status_text[0] ? s->status_text :
        (s->state == LEXIN_FACE_AUTH_SCANNING ? "请正对摄像头" :
         s->state == LEXIN_FACE_AUTH_DETECTED ? "识别中" :
         s->state == LEXIN_FACE_AUTH_RECOGNIZED ? s->status_text :
         s->state == LEXIN_FACE_AUTH_UNKNOWN ? "未识别到用户" :
         s->state == LEXIN_FACE_AUTH_REGISTERING ? "正在注册" :
         s->state == LEXIN_FACE_AUTH_REGISTERED ? "注册成功" :
         s->state == LEXIN_FACE_AUTH_ERROR ? "识别失败" : "等待摄像头");
    if (s_lock_status_label) lv_label_set_text(s_lock_status_label, status);
    if (s_lock_status_label) lv_label_set_text(s_lock_status_label, lock_state_text(s->state));

    const char *sub = "";
    switch (s->state) {
    case LEXIN_FACE_AUTH_RECOGNIZED:
        sub = s->user_name[0] ? s->user_name : ""; break;
    case LEXIN_FACE_AUTH_UNKNOWN:
        sub = "点击下方按钮创建新账户"; break;
    case LEXIN_FACE_AUTH_REGISTERING:
        sub = "请稍候"; break;
    case LEXIN_FACE_AUTH_REGISTERED:
        sub = "即将进入主界面"; break;
    case LEXIN_FACE_AUTH_ERROR:
        sub = "请检查代理连接"; break;
    default: sub = "将人脸置于框内"; break;
    }
    if (s_lock_sub_label) {
        if (s->state == LEXIN_FACE_AUTH_RECOGNIZED) {
            char wb[96]; snprintf(wb, sizeof(wb), "欢迎回来，%s", sub);
            lv_label_set_text(s_lock_sub_label, wb);
        } else {
            lv_label_set_text(s_lock_sub_label, sub);
        }
    }

    if (s_lock_sub_label) {
        if (s->state == LEXIN_FACE_AUTH_RECOGNIZED) {
            char wb[96];
            snprintf(wb, sizeof(wb), "Welcome %s",
                     s->user_name[0] ? s->user_name : "back");
            lv_label_set_text(s_lock_sub_label, wb);
        } else {
            lv_label_set_text(s_lock_sub_label, lock_state_sub_text(s->state));
        }
    }

    /* Action button */
    if (lock_can_create_user(s)) {
        lock_show_action_button("创建新用户");
    } else if (s->state == LEXIN_FACE_AUTH_REGISTERED ||
               s->state == LEXIN_FACE_AUTH_RECOGNIZED) {
        lock_hide_action_button();
    } else {
        lock_hide_action_button();
    }

    if (should_unlock) {
        lexin_screen_show_idle();
    }
    lvgl_port_unlock();
}

/* Compact banner shown on the launcher. The banner is a tappable
 * card that opens the voice conversation screen. */
static void lvgl_draw_voice_banner(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *banner = lvgl_glass_card(parent, x, y, w, h, 20);
    lv_obj_set_style_bg_color(banner, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_90, 0);
    lvgl_card_border(banner, 0xffa8d8, 2);

    /* Mic icon: rounded square with a vertical stem and a base. */
    lv_obj_t *mic = lvgl_card(banner, 18, 14, 30, 30, 0xff4f8a, 8);
    lvgl_card(mic, 11, 4, 8, 14, 0xffffff, 4);
    lvgl_card(mic, 5, 14, 20, 4, 0xffffff, 2);
    lvgl_card(mic, 13, 22, 4, 6, 0xffffff, 1);

    lvgl_label(banner, "语音对话  喊 乐鑫乐鑫 即可唤醒", 64, 12, &lexin_cn_20, 0x10283e);
    lvgl_label(banner, "本地麦克风采集   上传电脑 ASR + DeepSeek", 64, 36, &lexin_cn_20, 0x577489);

    lv_obj_t *text_mask = lvgl_card(banner, 58, 8, w - 232, 48, 0xffffff, 0);
    lv_obj_set_style_bg_opa(text_mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(text_mask, 0, 0);
    lvgl_label(text_mask, "语音对话  喊 乐鑫乐鑫 即可唤醒", 6, 2,
               &lexin_cn_20, 0x10283e);
    lvgl_label(text_mask, "本地麦克风采集  上传电脑 ASR + DeepSeek", 6, 26,
               &lexin_cn_20, 0x577489);

    /* State pill on the right edge. */
    lv_obj_t *pill = lvgl_card(banner, w - 158, 16, 142, 28, 0xfff0f6, 14);
    lv_obj_set_style_bg_opa(pill, LV_OPA_80, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(0xff4f8a), 0);
    lv_obj_t *state = lvgl_label(pill, s_voice_banner_state, 12, 6,
                                 &lexin_cn_20, 0xff4f8a);
    lvgl_label_width(state, 118);
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
            .mirror_x = true,
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

static bool wifi_hit(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* Hit boxes are aligned to the drawn buttons (Lock card x=650..790,
 * WiFi card x=820..980) and must not overlap, otherwise a tap that lands
 * near the shared edge — or drifts a few pixels due to touch calibration —
 * can trigger the wrong action (e.g. Lock opening the WiFi page). The two
 * regions meet at x=800, which sits in the gap between the buttons. */
static bool launcher_wifi_touch_hit(uint16_t raw_x, uint16_t raw_y)
{
    return wifi_hit(raw_x, raw_y, 800, 0, 224, 96);
}

static bool launcher_lock_touch_hit(uint16_t raw_x, uint16_t raw_y)
{
    return wifi_hit(raw_x, raw_y, 620, 0, 180, 96);
}

static bool lock_wifi_touch_hit(uint16_t raw_x, uint16_t raw_y)
{
    return wifi_hit(raw_x, raw_y, 700, 0, 324, 96);
}

static bool handle_emotion_touch(uint16_t x, uint16_t y)
{
    if (s_emotion_view != EMOTION_VIEW_LIVE) {
        if (wifi_hit(x, y, 0, 0, 180, 96) || wifi_hit(x, y, 844, 0, 180, 96)) {
            ESP_LOGI(TAG, "emotion report -> live page");
            lvgl_show_emotion_page();
            return true;
        }
        return true;
    }

    if (wifi_hit(x, y, 800, 352, 172, 34)) {
        ESP_LOGI(TAG, "touch x=%u y=%u -> capture board camera frame", x, y);
        lexin_screen_set_capture_status("CAPTURE QUEUED");
        lexin_request_board_capture();
        return true;
    }
    if (wifi_hit(x, y, 800, 390, 80, 34)) {
        ESP_LOGI(TAG, "touch x=%u y=%u -> daily emotion report", x, y);
        lexin_request_emotion_report(false);
        return true;
    }
    if (wifi_hit(x, y, 892, 390, 80, 34)) {
        ESP_LOGI(TAG, "touch x=%u y=%u -> monthly emotion report", x, y);
        lexin_request_emotion_report(true);
        return true;
    }
    return false;
}

static void wifi_password_append(char c)
{
    size_t len = strlen(s_wifi_password);
    if (len + 1 >= sizeof(s_wifi_password)) {
        snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "Password is full");
        return;
    }
    s_wifi_password[len] = c;
    s_wifi_password[len + 1] = '\0';
}

static void wifi_password_backspace(void)
{
    size_t len = strlen(s_wifi_password);
    if (len > 0) {
        s_wifi_password[len - 1] = '\0';
    }
}

static bool wifi_load_saved_password_for_selected(void)
{
    if (s_wifi_selected < 0 || s_wifi_selected >= s_wifi_ap_count) {
        s_wifi_password[0] = '\0';
        return false;
    }

    bool loaded = lexin_wifi_get_saved_password(s_wifi_aps[s_wifi_selected].ssid,
                                                s_wifi_password,
                                                sizeof(s_wifi_password));
    if (loaded) {
        s_wifi_show_password = true;
        return true;
    }

    s_wifi_password[0] = '\0';
    s_wifi_show_password = false;
    return false;
}

static bool wifi_select_first_saved_ap(void)
{
    for (uint16_t i = 0; i < s_wifi_ap_count; i++) {
        s_wifi_selected = i;
        if (wifi_load_saved_password_for_selected()) {
            return true;
        }
    }
    return false;
}

static void wifi_draw_key_row(lv_obj_t *parent, const char *keys, int x, int y, bool letters)
{
    for (size_t i = 0; keys[i] != '\0'; i++) {
        char label[2] = {keys[i], '\0'};
        if (letters && s_wifi_shift) {
            label[0] = (char)toupper((unsigned char)label[0]);
        }
        lv_obj_t *key = lvgl_card(parent, x + (int)i * 46, y, 40, 36, 0xffffff, 10);
        lvgl_card_border(key, 0xd7e8f2, 1);
        lvgl_center_label(key, label, 0, 8, 40, &lv_font_montserrat_20, 0x17364a);
    }
}

static bool wifi_try_key_row(uint16_t x, uint16_t y, const char *keys, int row_x, int row_y, bool letters)
{
    for (size_t i = 0; keys[i] != '\0'; i++) {
        int key_x = row_x + (int)i * 46;
        if (wifi_hit(x, y, key_x, row_y, 40, 36)) {
            char value = keys[i];
            if (letters && s_wifi_shift) {
                value = (char)toupper((unsigned char)value);
            }
            wifi_password_append(value);
            snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "Enter password");
            lvgl_show_wifi_page();
            return true;
        }
    }
    return false;
}

static void wifi_start_scan(void)
{
    snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "Scanning...");
    lvgl_show_wifi_page();

    uint16_t count = LEXIN_WIFI_MAX_APS;
    esp_err_t err = lexin_wifi_scan(s_wifi_aps, &count);
    s_wifi_ap_count = err == ESP_OK ? count : 0;
    if (err == ESP_OK) {
        s_wifi_selected = -1;
        bool saved_loaded = wifi_select_first_saved_ap();
        if (!saved_loaded && count > 0) {
            s_wifi_selected = 0;
            s_wifi_password[0] = '\0';
            s_wifi_show_password = false;
        }
        snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "%s",
                 saved_loaded ? "Saved password loaded" :
                 (count > 0 ? "Select network and enter password" : "No networks found"));
    } else {
        s_wifi_selected = -1;
        s_wifi_password[0] = '\0';
        s_wifi_show_password = false;
        snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "Scan failed: %s", esp_err_to_name(err));
    }
    lvgl_show_wifi_page();
}

static void wifi_start_connect(void)
{
    if (s_wifi_selected < 0 || s_wifi_selected >= s_wifi_ap_count) {
        snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "Select a network first");
        lvgl_show_wifi_page();
        return;
    }

    snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "Connecting to %s",
             s_wifi_aps[s_wifi_selected].ssid);
    lvgl_show_wifi_page();
    esp_err_t err = lexin_wifi_connect_ap(&s_wifi_aps[s_wifi_selected], s_wifi_password);
    if (err != ESP_OK) {
        snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "Connect failed: %s", esp_err_to_name(err));
        lvgl_show_wifi_page();
        return;
    }

    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const char *status = lexin_wifi_status_text();
        snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "%s", status);
        lvgl_show_wifi_page();
        if (lexin_wifi_is_connected()) {
            esp_err_t save_err = lexin_wifi_save_password(s_wifi_aps[s_wifi_selected].ssid,
                                                          s_wifi_password);
            snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "%s",
                     save_err == ESP_OK ? "Connected, password saved" : "Connected, save failed");
            lvgl_show_wifi_page();
            break;
        }
        if (strstr(status, "failed") != NULL ||
            strstr(status, "mismatch") != NULL ||
            strstr(status, "weak") != NULL ||
            strstr(status, "No AP") != NULL ||
            strstr(status, "timeout") != NULL) {
            break;
        }
    }
    const char *final_status = lexin_wifi_status_text();
    if (!lexin_wifi_is_connected() &&
        (strcmp(final_status, "Connecting...") == 0 ||
         strcmp(final_status, "Reconnecting...") == 0 ||
         strcmp(final_status, "Waiting IP...") == 0)) {
        snprintf(s_wifi_page_status, sizeof(s_wifi_page_status),
                 "Connect timeout, check password");
        lvgl_show_wifi_page();
    }
}

static bool lvgl_show_wifi_page(void)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lexin_launcher_show_screen(LEXIN_SCREEN_WIFI);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_vertical_gradient(scr, 0xf3fbff, 0xd7f2ff);
    lvgl_card(scr, 0, 0, LCD_H_RES, 86, 0x167ca7, 0);
    lv_obj_set_style_bg_opa(lvgl_card(scr, 0, 86, LCD_H_RES, 86, 0x80d9ee, 0), LV_OPA_40, 0);
    lvgl_label(scr, "Back", 32, 28, &lv_font_montserrat_20, 0xffffff);
    lvgl_label(scr, "WiFi Setup", 132, 22, &lv_font_montserrat_28, 0xffffff);
    lvgl_label(scr, lexin_wifi_is_connected() ? "Connected" : lexin_wifi_status_text(),
               612, 30, &lv_font_montserrat_20, 0xffffff);
    lv_obj_t *scan_btn = lvgl_card(scr, 830, 22, 138, 42, 0xffffff, 21);
    lv_obj_set_style_bg_opa(scan_btn, LV_OPA_90, 0);
    lvgl_center_label(scan_btn, "Scan", 0, 10, 138, &lv_font_montserrat_20, 0x167ca7);

    lv_obj_t *left = lvgl_glass_card(scr, 42, 112, 430, 430, 24);
    lvgl_label(left, "Networks", 24, 22, &lv_font_montserrat_28, 0x10283e);
    lvgl_label(left, s_wifi_page_status, 24, 58, &lv_font_montserrat_20, 0x557387);
    if (s_wifi_ap_count == 0) {
        lvgl_label(left, "Tap Scan to list nearby WiFi.", 28, 150, &lv_font_montserrat_20, 0x557387);
    }
    uint16_t visible = s_wifi_ap_count < 6 ? s_wifi_ap_count : 6;
    for (uint16_t i = 0; i < visible; i++) {
        bool selected = (int)i == s_wifi_selected;
        lv_obj_t *row = lvgl_card(left, 24, 92 + (int)i * 56, 382, 48,
                                  selected ? 0x19afd8 : 0xffffff, 14);
        lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_80, 0);
        lvgl_card_border(row, selected ? 0x19afd8 : 0xd7e8f2, 1);
        lv_obj_t *ssid = lvgl_label(row, s_wifi_aps[i].ssid, 16, 8,
                                    &lv_font_montserrat_20, selected ? 0xffffff : 0x17364a);
        lvgl_label_width(ssid, 260);
        char meta[32];
        snprintf(meta, sizeof(meta), "%ddBm %s", s_wifi_aps[i].rssi,
                 s_wifi_aps[i].authmode == 0 ? "OPEN" : "LOCK");
        lvgl_label(row, meta, 280, 13, &lv_font_montserrat_14,
                   selected ? 0xe9f8ff : 0x557387);
    }

    lv_obj_t *right = lvgl_glass_card(scr, 500, 112, 480, 430, 24);
    lvgl_label(right, "Password", 24, 22, &lv_font_montserrat_28, 0x10283e);
    lvgl_label(right, s_wifi_selected >= 0 ? s_wifi_aps[s_wifi_selected].ssid : "No network selected",
               208, 30, &lv_font_montserrat_20, 0x557387);
    lv_obj_t *field = lvgl_card(right, 24, 66, 432, 46, 0xffffff, 14);
    lvgl_card_border(field, 0xc6e0ed, 1);
    char password_view[LEXIN_WIFI_PASSWORD_MAX_LEN + 1];
    size_t pass_len = strlen(s_wifi_password);
    if (pass_len == 0) {
        snprintf(password_view, sizeof(password_view), "Enter password");
    } else if (s_wifi_show_password) {
        snprintf(password_view, sizeof(password_view), "%s", s_wifi_password);
    } else {
        for (size_t i = 0; i < pass_len && i < sizeof(password_view) - 1; i++) {
            password_view[i] = '*';
        }
        password_view[pass_len < sizeof(password_view) ? pass_len : sizeof(password_view) - 1] = '\0';
    }
    lv_obj_t *pass_label = lvgl_label(field, password_view, 16, 12, &lv_font_montserrat_20, 0x17364a);
    lvgl_label_width(pass_label, 396);

    if (s_wifi_symbols) {
        wifi_draw_key_row(right, "1234567890", 22, 136, false);
        wifi_draw_key_row(right, "!@#$%^&*()", 22, 182, false);
        wifi_draw_key_row(right, "-_=+[]{}", 68, 228, false);
        wifi_draw_key_row(right, ".,?:;/+", 91, 274, false);
    } else {
        wifi_draw_key_row(right, "1234567890", 22, 136, false);
        wifi_draw_key_row(right, "qwertyuiop", 22, 182, true);
        wifi_draw_key_row(right, "asdfghjkl", 45, 228, true);
        wifi_draw_key_row(right, "zxcvbnm", 91, 274, true);
    }

    lv_obj_t *shift = lvgl_card(right, 24, 322, 72, 36, s_wifi_shift ? 0x19afd8 : 0xffffff, 12);
    lvgl_card_border(shift, 0xc6e0ed, 1);
    lvgl_center_label(shift, s_wifi_shift ? "ABC" : "abc", 0, 8, 72, &lv_font_montserrat_20,
                      s_wifi_shift ? 0xffffff : 0x17364a);
    lv_obj_t *sym = lvgl_card(right, 104, 322, 72, 36, s_wifi_symbols ? 0x19afd8 : 0xffffff, 12);
    lvgl_card_border(sym, 0xc6e0ed, 1);
    lvgl_center_label(sym, "SYM", 0, 8, 72, &lv_font_montserrat_20,
                      s_wifi_symbols ? 0xffffff : 0x17364a);
    lv_obj_t *del = lvgl_card(right, 184, 322, 72, 36, 0xffffff, 12);
    lvgl_card_border(del, 0xc6e0ed, 1);
    lvgl_center_label(del, "DEL", 0, 8, 72, &lv_font_montserrat_20, 0x17364a);
    lv_obj_t *clear = lvgl_card(right, 264, 322, 72, 36, 0xffffff, 12);
    lvgl_card_border(clear, 0xc6e0ed, 1);
    lvgl_center_label(clear, "CLR", 0, 8, 72, &lv_font_montserrat_20, 0x17364a);
    lv_obj_t *show = lvgl_card(right, 344, 322, 88, 36, s_wifi_show_password ? 0x19afd8 : 0xffffff, 12);
    lvgl_card_border(show, 0xc6e0ed, 1);
    lvgl_center_label(show, s_wifi_show_password ? "HIDE" : "SHOW", 0, 8, 88, &lv_font_montserrat_20,
                      s_wifi_show_password ? 0xffffff : 0x17364a);

    lv_obj_t *connect = lvgl_card(right, 24, 372, 432, 42, 0x167ca7, 21);
    lvgl_center_label(connect, "Connect", 0, 10, 432, &lv_font_montserrat_20, 0xffffff);

    lvgl_port_unlock();
    return true;
}

static bool handle_wifi_touch(uint16_t x, uint16_t y)
{
    if (wifi_hit(x, y, 0, 0, 180, 86)) {
        lexin_screen_show_idle();
        return true;
    }
    if (wifi_hit(x, y, 830, 22, 138, 42)) {
        wifi_start_scan();
        return true;
    }

    for (uint16_t i = 0; i < s_wifi_ap_count && i < 6; i++) {
        if (wifi_hit(x, y, 66, 204 + (int)i * 56, 382, 48)) {
            s_wifi_selected = i;
            bool saved_loaded = wifi_load_saved_password_for_selected();
            snprintf(s_wifi_page_status, sizeof(s_wifi_page_status), "%s",
                     saved_loaded ? "Saved password loaded" : "Enter password");
            lvgl_show_wifi_page();
            return true;
        }
    }

    const int right_x = 500;
    const int key_base_x = right_x + 22;
    if (s_wifi_symbols) {
        if (wifi_try_key_row(x, y, "1234567890", key_base_x, 248, false) ||
            wifi_try_key_row(x, y, "!@#$%^&*()", key_base_x, 294, false) ||
            wifi_try_key_row(x, y, "-_=+[]{}", right_x + 68, 340, false) ||
            wifi_try_key_row(x, y, ".,?:;/+", right_x + 91, 386, false)) {
            return true;
        }
    } else {
        if (wifi_try_key_row(x, y, "1234567890", key_base_x, 248, false) ||
            wifi_try_key_row(x, y, "qwertyuiop", key_base_x, 294, true) ||
            wifi_try_key_row(x, y, "asdfghjkl", right_x + 45, 340, true) ||
            wifi_try_key_row(x, y, "zxcvbnm", right_x + 91, 386, true)) {
            return true;
        }
    }

    if (wifi_hit(x, y, right_x + 24, 434, 72, 36)) {
        s_wifi_shift = !s_wifi_shift;
        lvgl_show_wifi_page();
        return true;
    } else if (wifi_hit(x, y, right_x + 104, 434, 72, 36)) {
        s_wifi_symbols = !s_wifi_symbols;
        lvgl_show_wifi_page();
        return true;
    } else if (wifi_hit(x, y, right_x + 184, 434, 72, 36)) {
        wifi_password_backspace();
        lvgl_show_wifi_page();
        return true;
    } else if (wifi_hit(x, y, right_x + 264, 434, 72, 36)) {
        s_wifi_password[0] = '\0';
        lvgl_show_wifi_page();
        return true;
    } else if (wifi_hit(x, y, right_x + 344, 434, 88, 36)) {
        s_wifi_show_password = !s_wifi_show_password;
        lvgl_show_wifi_page();
        return true;
    } else if (wifi_hit(x, y, right_x + 24, 484, 432, 42)) {
        wifi_start_connect();
        return true;
    }
    return false;
}

static bool lvgl_show_launcher(void)
{
    /* The lock screen is painted while the launcher screen-state stays
     * LEXIN_SCREEN_LAUNCHER, so background status callbacks (voice banner
     * updates etc.) that repaint the launcher "in place" would stomp the
     * lock screen and make the device look unlocked while it is not.
     * Never render the launcher for a logged-out user. */
    if (!lexin_face_auth_is_logged_in()) {
        ESP_LOGI(TAG, "render launcher suppressed: locked");
        return false;
    }
    ESP_LOGI(TAG, "render launcher");
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lexin_launcher_show_launcher();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lock_clear_widget_refs();
    s_lock_register_active = false;
    s_lock_screen_visible = false;
    lvgl_set_vertical_gradient(scr, 0xe7f7ff, 0xc7f0ff);
    lvgl_card(scr, 0, 0, LCD_H_RES, 86, 0x19afd8, 0);
    lv_obj_set_style_bg_opa(lvgl_card(scr, 0, 86, LCD_H_RES, 102, 0x8fe2f5, 0), LV_OPA_40, 0);
    lvgl_label(scr, "LeXin", 48, 22, &lv_font_montserrat_28, 0xffffff);
    lvgl_label(scr, "AI桌宠", 140, 22, &lexin_cn_28, 0xffffff);

    lv_obj_t *wifi_btn = lvgl_card(scr, 820, 20, 160, 46, 0xffffff, 23);
    lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_90, 0);
    lvgl_card_border(wifi_btn, 0xffffff, 1);
    lvgl_center_label(wifi_btn, "WiFi", 0, 10, 160, &lv_font_montserrat_20, 0x19afd8);

    lv_obj_t *lock_btn = lvgl_card(scr, 650, 20, 140, 46, 0xffffff, 23);
    lv_obj_set_style_bg_opa(lock_btn, LV_OPA_90, 0);
    lvgl_card_border(lock_btn, 0xffffff, 1);
    lvgl_center_label(lock_btn, "Lock", 0, 10, 140, &lv_font_montserrat_20, 0x19afd8);

    lv_obj_t *pet_card = lvgl_glass_card(scr, 66, 132, 348, 344, 28);
    const char *user_label = lexin_face_auth_current_user_name();
    char user_title[48];
    snprintf(user_title, sizeof(user_title), "%s 在线",
             user_label ? user_label : "乐鑫");
    lvgl_label(pet_card, user_title, 32, 30, &lexin_cn_28, 0x10283e);
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

    /* Voice banner sits below the app panel. Coordinates mirror the
     * launcher_app_t entry in lexin_launcher.c so taps map 1:1. */
    lvgl_draw_voice_banner(scr, 66, 494, 892, 60);

    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_suggestion_page(void)
{
    ESP_LOGI(TAG, "render suggestion");
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lexin_launcher_show_screen(LEXIN_SCREEN_SUGGESTION);

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
    /* Neural 5-mood (temporal-voted) from the FER model. */
    switch (snapshot->mood) {
    case LEXIN_VISION_MOOD_HAPPY:
        return "HAPPY";
    case LEXIN_VISION_MOOD_TIRED:
        return "TIRED";
    case LEXIN_VISION_MOOD_STRESSED:
        return "STRESSED";
    case LEXIN_VISION_MOOD_SURPRISED:
        return "SURPRISED";
    case LEXIN_VISION_MOOD_AWAY:
        return "AWAY";
    case LEXIN_VISION_MOOD_FOCUSED:
    default:
        return "FOCUSED";
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
    switch (snapshot->mood) {
    case LEXIN_VISION_MOOD_HAPPY:
        return 0x20a56b;  /* green */
    case LEXIN_VISION_MOOD_STRESSED:
        return 0xd9534f;  /* red */
    case LEXIN_VISION_MOOD_TIRED:
        return 0x8a6fd9;  /* purple */
    case LEXIN_VISION_MOOD_SURPRISED:
        return 0xe0a030;  /* amber */
    case LEXIN_VISION_MOOD_FOCUSED:
        return 0x1c98d2;  /* blue */
    case LEXIN_VISION_MOOD_AWAY:
    default:
        return 0x71889a;  /* grey */
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
    if (!snapshot || lexin_launcher_current_screen() != LEXIN_SCREEN_EMOTION ||
        s_emotion_view != EMOTION_VIEW_LIVE) {
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
    if (s_emotion_capture_status_label) {
        lv_label_set_text(s_emotion_capture_status_label, s_capture_status);
    }

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
    lexin_launcher_show_screen(LEXIN_SCREEN_EMOTION);
    s_emotion_view = EMOTION_VIEW_LIVE;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    if (preview_ready) {
        lvgl_commit_emotion_preview();
    }
    clear_emotion_live_widget_refs();
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
    s_emotion_system_meta_label = lvgl_label(system_card, system_meta, 18, 48,
                                             &lv_font_montserrat_20, 0x8deaf3);
    lvgl_label_width(s_emotion_system_meta_label, 184);
    s_emotion_capture_status_label = lvgl_label(system_card, s_capture_status, 18, 220,
                                                &lv_font_montserrat_20, 0x8deaf3);
    lvgl_label_width(s_emotion_capture_status_label, 184);
    lv_obj_t *capture_btn = lvgl_card(system_card, 24, 250, 172, 34, 0x20d9d2, 8);
    lv_obj_set_style_bg_opa(capture_btn, LV_OPA_COVER, 0);
    lvgl_center_label(capture_btn, "CAPTURE", 0, 7, 172,
                      &lv_font_montserrat_20, 0x06283a);
    lv_obj_t *today_btn = lvgl_card(system_card, 24, 288, 80, 34, 0xffffff, 8);
    lvgl_card_border(today_btn, 0x20d9d2, 2);
    lvgl_center_label(today_btn, "TODAY", 0, 7, 80,
                      &lv_font_montserrat_20, 0x0b3650);
    lv_obj_t *month_btn = lvgl_card(system_card, 116, 288, 80, 34, 0xffffff, 8);
    lvgl_card_border(month_btn, 0x20d9d2, 2);
    lvgl_center_label(month_btn, "MONTH", 0, 7, 80,
                      &lv_font_montserrat_20, 0x0b3650);

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

static int emotion_parse_scores(const char *text, int *scores, int max_scores)
{
    if (!text || !scores || max_scores <= 0) {
        return 0;
    }
    const char *p = strstr(text, "SCORES:");
    if (!p) {
        return 0;
    }
    p += strlen("SCORES:");
    int count = 0;
    while (*p != '\0' && *p != '\n' && *p != '\r' && count < max_scores) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == '\0' || *p == '\n' || *p == '\r') {
            break;
        }
        scores[count++] = atoi(p);
        while (*p != '\0' && *p != ',' && *p != '\n' && *p != '\r') {
            p++;
        }
    }
    return count;
}

static uint32_t emotion_score_color(int score)
{
    if (score >= 2) return 0x1ecf82;
    if (score == 1) return 0xf2be3e;
    if (score == 0) return 0x20d9d2;
    if (score == -1) return 0x8d8ce8;
    return 0xff6577;
}

static void emotion_draw_score_bars(lv_obj_t *parent, const int *scores, int count,
                                    int x, int y, int w, int h)
{
    lv_obj_t *chart = lvgl_card(parent, x, y, w, h, 0x092438, 6);
    lvgl_card_border(chart, 0x147e9d, 1);
    lv_obj_t *mid = lvgl_card(chart, 12, h / 2, w - 24, 2, 0x315669, 1);
    lv_obj_set_style_bg_opa(mid, LV_OPA_60, 0);
    if (!scores || count <= 0) {
        lvgl_center_label(chart, "NO DATA", 0, h / 2 - 12, w,
                          &lv_font_montserrat_20, 0x88b9c7);
        return;
    }

    int gap = 3;
    int usable = w - 24;
    int bar_w = usable / count - gap;
    if (bar_w < 4) {
        bar_w = 4;
        gap = 1;
    }
    if (bar_w > 24) {
        bar_w = 24;
    }
    int zero_y = h / 2;
    for (int i = 0; i < count; i++) {
        int score = scores[i];
        if (score > 2) score = 2;
        if (score < -2) score = -2;
        int mag = score >= 0 ? score : -score;
        int bar_h = 6 + mag * ((h / 2 - 16) / 2);
        int bx = 12 + i * (bar_w + gap);
        int by = score >= 0 ? zero_y - bar_h : zero_y;
        lvgl_card(chart, bx, by, bar_w, bar_h, emotion_score_color(score), 3);
    }
}

static bool lvgl_show_emotion_report_page(const char *text, bool monthly)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lexin_launcher_show_screen(LEXIN_SCREEN_EMOTION);
    s_emotion_view = monthly ? EMOTION_VIEW_MONTHLY : EMOTION_VIEW_DAILY;

    char model[48], date[48], month[48], samples[32], dominant[64];
    char avg[32], stress[32], positive[32], advice[384];
    copy_field_value(text, "MODEL:", model, sizeof(model));
    copy_field_value(text, "DATE:", date, sizeof(date));
    copy_field_value(text, "MONTH:", month, sizeof(month));
    copy_field_value(text, "SAMPLES:", samples, sizeof(samples));
    copy_field_value(text, "DOMINANT:", dominant, sizeof(dominant));
    copy_field_value(text, "AVG_SCORE:", avg, sizeof(avg));
    copy_field_value(text, "STRESS_TIRED:", stress, sizeof(stress));
    copy_field_value(text, "HAPPY_FOCUSED:", positive, sizeof(positive));
    copy_field_raw_value(text, "ADVICE:", advice, sizeof(advice));

    lv_obj_t *scr = lv_screen_active();
    clear_emotion_live_widget_refs();
    lv_obj_clean(scr);
    lvgl_set_vertical_gradient(scr, 0x06111c, 0x0b2635);
    lv_obj_t *header = lvgl_card(scr, 0, 0, LCD_H_RES, 82, 0x0b3650, 0);
    lvgl_set_vertical_gradient(header, 0x0a2740, 0x086179);
    lvgl_label(header, "BACK", 24, 27, &lv_font_montserrat_20, 0xffffff);
    lvgl_label(header, monthly ? "EMOTION MONTH" : "EMOTION TODAY",
               112, 20, &lv_font_montserrat_28, 0xffffff);
    lvgl_label(header, monthly ? month : date, 444, 29,
               &lv_font_montserrat_20, 0x99f3ff);

    lv_obj_t *summary = lvgl_card(scr, 36, 112, 330, 344, 0x0c3047, 8);
    lvgl_card_border(summary, 0x1cc9d5, 2);
    lvgl_label(summary, "SUMMARY", 22, 22, &lv_font_montserrat_20, 0xb8f7ff);
    char line[384];
    snprintf(line, sizeof(line),
             "MODEL: %s\nSAMPLES: %s\n%s: %s\nAVG: %s\n%s: %s\n%s: %s",
             model, samples,
             "DOMINANT", dominant,
             avg,
             "STRESS", stress,
             "POSITIVE", positive);
    lv_obj_t *summary_label = lvgl_label(summary, line, 22, 70,
                                         &lv_font_montserrat_20, 0xd6f4fb);
    lv_obj_set_width(summary_label, 286);
    lv_label_set_long_mode(summary_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *chart_card = lvgl_card(scr, 394, 112, 594, 202, 0x0c3c50, 8);
    lvgl_card_border(chart_card, 0x20d9d2, 2);
    lvgl_label(chart_card, monthly ? "DAILY TREND" : "HOURLY TREND",
               22, 18, &lv_font_montserrat_20, 0xb8f7ff);
    int scores[32];
    int count = emotion_parse_scores(text, scores, 32);
    emotion_draw_score_bars(chart_card, scores, count, 22, 58, 550, 118);

    lv_obj_t *advice_card = lvgl_card(scr, 394, 338, 594, 190, 0x0b3048, 8);
    lvgl_card_border(advice_card, 0x147e9d, 2);
    lvgl_label(advice_card, "ADVICE", 22, 16, &lv_font_montserrat_20, 0x7edce8);
    lv_obj_t *advice_label = lvgl_label(advice_card, advice, 22, 56,
                                        &lexin_cn_20, 0xffffff);
    lv_obj_set_width(advice_label, 548);
    lv_label_set_long_mode(advice_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *hint = lvgl_card(scr, 36, 480, 330, 48, 0x123e5a, 8);
    lvgl_center_label(hint, "BACK returns to emotion page", 0, 12, 330,
                      &lv_font_montserrat_20, 0xb8f7ff);

    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_voice_page(void)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lexin_voice_set_mode("voice");
    lexin_launcher_show_screen(LEXIN_SCREEN_VOICE);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_vertical_gradient(scr, 0xfff5fa, 0xffd9ea);

    /* Header. */
    lvgl_card(scr, 0, 0, LCD_H_RES, 86, 0xff4f8a, 0);
    lv_obj_set_style_bg_opa(lvgl_card(scr, 0, 86, LCD_H_RES, 88, 0xffa8d8, 0), LV_OPA_40, 0);
    lvgl_label(scr, "返回", 32, 28, &lexin_cn_20, 0xffffff);
    lvgl_label(scr, "语音对话", 110, 24, &lexin_cn_28, 0xffffff);

    lvgl_card(scr, 24, 18, 360, 52, 0xff4f8a, 0);
    lvgl_label(scr, "返回", 32, 28, &lexin_cn_20, 0xffffff);
    lvgl_label(scr, "语音对话", 110, 24, &lexin_cn_28, 0xffffff);

    /* State pill (large). */
    lv_obj_t *state_pill = lvgl_card(scr, 32, 116, 380, 64, 0xffffff, 28);
    lv_obj_set_style_bg_opa(state_pill, LV_OPA_90, 0);
    lv_obj_set_style_border_width(state_pill, 2, 0);
    lv_obj_set_style_border_color(state_pill, lv_color_hex(0xff4f8a), 0);
    s_voice_state_pill = lvgl_label(state_pill, voice_state_cn(s_voice_snapshot.state),
                                       22, 18, &lexin_cn_28, 0xff4f8a);
    lvgl_label_width(s_voice_state_pill, 336);
    s_voice_status_label = lvgl_label(scr, "BOOTING", 432, 124,
                                      &lv_font_montserrat_20, 0x10283e);
    lvgl_label_width(s_voice_status_label, 240);
    s_voice_backend_label = lvgl_label(scr, "backend --", 432, 154,
                                       &lv_font_montserrat_20, 0x577489);
    lvgl_label_width(s_voice_backend_label, 240);
    s_voice_meter_label = lvgl_label(scr, "RMS 0", 700, 124,
                                     &lv_font_montserrat_20, 0x9465ff);
    lvgl_label_width(s_voice_meter_label, 200);

    /* Tip card. */
    lv_obj_t *tip_card = lvgl_glass_card(scr, 32, 200, 960, 110, 22);
    lvgl_label(tip_card, "使用方法", 24, 16, &lexin_cn_20, 0x577489);
    lvgl_label(tip_card, "在本页直接说一句话即可；回到主页后，可喊“乐鑫乐鑫”唤醒", 24, 46,
                  &lexin_cn_20, 0x10283e);
    lvgl_label(tip_card, "等喇叭到位后会接 TTS 念出来", 24, 76,
                  &lexin_cn_20, 0x9465ff);

    lv_obj_t *tip_mask = lvgl_card(tip_card, 18, 10, 924, 90, 0xffffff, 0);
    lv_obj_set_style_bg_opa(tip_mask, LV_OPA_90, 0);
    lv_obj_set_style_border_width(tip_mask, 0, 0);
    lvgl_label(tip_mask, "使用方法", 6, 6, &lexin_cn_20, 0x577489);
    lvgl_label(tip_mask, "先说“乐鑫乐鑫”，听到提示后再说一句话", 6, 34,
               &lexin_cn_20, 0x10283e);
    lvgl_label(tip_mask, "观察 RMS 数值，接近 0 表示麦克风没有输入", 6, 62,
               &lexin_cn_20, 0x9465ff);

    /* Reply card. */
    lv_obj_t *reply_card = lvgl_glass_card(scr, 32, 322, 960, 230, 22);
    lvgl_label(reply_card, "你说", 24, 16, &lexin_cn_20, 0x577489);
    s_voice_transcript_label = lvgl_label(reply_card, "（等待识别）", 24, 46, &lexin_cn_20, 0x10283e);
    lvgl_label_width(s_voice_transcript_label, 912);
    lvgl_label(reply_card, "回复", 24, 86, &lexin_cn_20, 0x577489);
    s_voice_reply_label = lvgl_label(reply_card, "（暂无回复）", 24, 116, &lexin_cn_28, 0xff4f8a);
    lvgl_label_width(s_voice_reply_label, 912);

    lv_obj_t *reply_title_mask = lvgl_card(reply_card, 18, 10, 924, 110, 0xffffff, 0);
    lv_obj_set_style_bg_opa(reply_title_mask, LV_OPA_90, 0);
    lv_obj_set_style_border_width(reply_title_mask, 0, 0);
    lvgl_label(reply_title_mask, "你说", 6, 6, &lexin_cn_20, 0x577489);
    lvgl_label(reply_title_mask, "回复", 6, 72, &lexin_cn_20, 0x577489);
    lv_obj_move_foreground(s_voice_transcript_label);
    lv_obj_move_foreground(s_voice_reply_label);

    lvgl_refresh_voice_page();
    lvgl_port_unlock();
    return true;
}

static void lvgl_refresh_voice_page(void)
{
    if (!s_lvgl_ready || !s_voice_state_pill || !s_voice_reply_label) {
        return;
    }
    if (!lvgl_port_lock(80)) {
        return;
    }

    lv_label_set_text(s_voice_state_pill, voice_state_cn(s_voice_snapshot.state));
    lv_obj_set_style_text_color(s_voice_state_pill,
                                  lv_color_hex(voice_state_color(s_voice_snapshot.state)), 0);
    lv_label_set_text(s_voice_status_label,
                         s_voice_snapshot.status[0] ? s_voice_snapshot.status : "待命中");
    lv_label_set_text(s_voice_backend_label,
                         voice_backend_text(s_voice_snapshot.backend));
    char meter[32];
    snprintf(meter, sizeof(meter), "RMS %u", s_voice_snapshot.rms);
    lv_label_set_text(s_voice_meter_label, meter);

    const char *transcript = s_voice_snapshot.transcript[0]
        ? s_voice_snapshot.transcript : "（等待识别）";
    lv_label_set_text(s_voice_transcript_label, transcript);

    const char *reply = s_voice_snapshot.reply[0]
        ? s_voice_snapshot.reply : "（暂无回复）";
    lv_label_set_text(s_voice_reply_label, reply);

    lvgl_port_unlock();
}

void lexin_screen_update_voice_context(const lexin_voice_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    s_voice_snapshot_valid = true;
    s_voice_snapshot = *snapshot;

    /* Daily-plan capture replies (status PLAN_OK / PLAN_EMPTY) never touch
     * the voice page. End the recording state and refresh the plan page. */
    if (strncmp(snapshot->status, "PLAN", 4) == 0) {
        s_plan_recording = false;
        lexin_voice_set_mode("chat");
        if (strcmp(snapshot->status, "PLAN_OK") == 0) {
            lexin_plan_fetch_today();   /* reload + re-render the plan page */
        } else if (s_lvgl_ready && lvgl_port_lock(200)) {
            if (lexin_launcher_current_screen() == LEXIN_SCREEN_PLAN) {
                lvgl_show_plan_page();
            }
            lvgl_port_unlock();
        }
        return;
    }

    /* While locked (or on the register screen) the launcher screen-state
     * is still LEXIN_SCREEN_LAUNCHER, so the banner-refresh and the
     * wake-reply hijack below would repaint over the lock screen. Voice
     * UI must stay dormant until the user is authenticated. */
    if (!lexin_face_auth_is_logged_in()) {
        return;
    }

    const char *state_text = voice_state_cn(snapshot->state);
    bool banner_changed = state_text != s_voice_banner_state;
    s_voice_banner_state = state_text;

    /* A "conversational" reply is one the proxy accepted after a wake
     * word / open session (status OK or WAKE). NO_WAKE / NO_ASR replies
     * must not hijack the screen. */
    bool conversational = snapshot->state == LEXIN_VOICE_STATE_REPLY &&
        (strcmp(snapshot->status, "OK") == 0 ||
         strcmp(snapshot->status, "WAKE") == 0);
    bool fresh_reply = conversational &&
        snapshot->updated_at_ms != s_voice_last_reply_ms;

    if (!s_lvgl_ready) {
        return;
    }
    /* Hold the LVGL lock (recursive) across both the screen check and
     * the redraw. Otherwise this task can pass the "still on launcher"
     * check, lose the CPU to a touch that opens a result page, and then
     * repaint the launcher over it while desyncing s_current_screen. */
    if (!lvgl_port_lock(200)) {
        return;
    }
    lexin_screen_id_t cur = lexin_launcher_current_screen();
    if (cur == LEXIN_SCREEN_VOICE) {
        lvgl_refresh_voice_page();
    } else if (fresh_reply && cur == LEXIN_SCREEN_LAUNCHER) {
        /* Wake succeeded while the user was on the launcher: surface the
         * conversation so the recognized text and reply are visible. */
        lvgl_show_voice_page();
    } else if (banner_changed && cur == LEXIN_SCREEN_LAUNCHER) {
        /* Redraw the launcher banner so the user can see the live
         * state without opening the voice page. */
        lvgl_show_launcher();
    }
    if (conversational) {
        s_voice_last_reply_ms = snapshot->updated_at_ms;
    }
    lvgl_port_unlock();
}

static bool lvgl_show_pet_ai_page(void)
{
    ESP_LOGI(TAG, "render pet");
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lexin_launcher_show_screen(LEXIN_SCREEN_PET);

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
    ESP_LOGI(TAG, "render querying action=%d", (int)action_id);
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lexin_launcher_show_screen(action_id == LEXIN_ACTION_WEATHER ?
        LEXIN_SCREEN_WEATHER : action_id == LEXIN_ACTION_TIME ?
        LEXIN_SCREEN_CALENDAR : LEXIN_SCREEN_SUGGESTION);

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
    ESP_LOGI(TAG, "render weather result");
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
    lexin_launcher_show_screen(LEXIN_SCREEN_WEATHER);

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

static void lvgl_calendar_cell(lv_obj_t *parent, int x, int y, const char *day, bool muted,
                               bool active, int percent)
{
    lv_obj_t *cell = lvgl_card(parent, x, y, 72, 40, active ? 0x28a7e8 : 0xffffff, 12);
    lv_obj_set_style_bg_opa(cell, active ? LV_OPA_COVER : LV_OPA_0, 0);
    lv_obj_set_style_border_width(cell, active ? 0 : 1, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(0xe2eef5), 0);
    lv_obj_set_style_border_opa(cell, muted ? LV_OPA_0 : LV_OPA_40, 0);
    lv_obj_t *label = lvgl_label(cell, day, 0, 4, &lv_font_montserrat_20,
                                 active ? 0xffffff : muted ? 0xb6c4cf : 0x172a3a);
    lv_obj_set_width(label, 72);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    /* Daily plan progress: a small framed bar filled by completion %. */
    if (percent >= 0) {
        lv_obj_t *bar_bg = lvgl_card(cell, 10, 30, 52, 6, active ? 0x1c85c0 : 0xe2f0e9, 3);
        lvgl_card_border(bar_bg, active ? 0xbfe4f7 : 0x9fd8bd, 1);
        int fill = percent > 100 ? 100 : percent;
        int fill_w = (52 * fill) / 100;
        if (fill_w > 0) {
            lvgl_card(bar_bg, 0, 0, fill_w, 6, 0x2fb98a, 3);
        }
    }
}

static bool lvgl_show_calendar_result_page(const char *text)
{
    ESP_LOGI(TAG, "render calendar result");
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
    lexin_launcher_show_screen(LEXIN_SCREEN_CALENDAR);

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
    /* Entry to the daily plan module (handled in handle_calendar_touch). */
    lv_obj_t *plan_btn = lvgl_card(scr, 724, 18, 250, 48, 0x1c8f66, 24);
    lvgl_card_border(plan_btn, 0xbff0dc, 2);
    lvgl_center_label(plan_btn, "今日计划", 0, 12, 250, &lexin_cn_20, 0xffffff);
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
        int cell_percent = -1;
        if (!muted && shown_day >= 1 && shown_day <= 31 &&
            s_plan_month_percent[shown_day] != 255) {
            cell_percent = s_plan_month_percent[shown_day];
        }
        lvgl_calendar_cell(main_card, start_x + (i % 7) * cell_w, start_y + (i / 7) * cell_h,
                           day_text, muted, active, cell_percent);
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

/* ----------------------------------------------------------------- */
/* Daily plan module                                                  */
/* ----------------------------------------------------------------- */

static void parse_plan_text(const char *text)
{
    s_plan_count = 0;
    s_plan_percent = 0;
    if (!text) {
        return;
    }
    char buf[16];
    copy_field_value(text, "PERCENT:", buf, sizeof(buf));
    s_plan_percent = atoi(buf);           /* "UNKNOWN" -> 0 */
    if (s_plan_percent < 0) s_plan_percent = 0;
    if (s_plan_percent > 100) s_plan_percent = 100;

    copy_field_value(text, "COUNT:", buf, sizeof(buf));
    int count = atoi(buf);
    if (count < 0) count = 0;
    if (count > LEXIN_PLAN_MAX_ITEMS) count = LEXIN_PLAN_MAX_ITEMS;

    for (int i = 0; i < count; i++) {
        char key[20];
        snprintf(key, sizeof(key), "ITEM%d:", i);
        char raw[LEXIN_PLAN_ITEM_LEN];
        copy_field_raw_value(text, key, raw, sizeof(raw));
        if (strcmp(raw, "UNKNOWN") == 0) {
            continue;
        }
        bool done = false;
        const char *txt = raw;
        char *bar = strchr(raw, '|');
        if (bar) {
            done = (raw[0] == '1');
            txt = bar + 1;
        }
        s_plan_done[s_plan_count] = done;
        snprintf(s_plan_items[s_plan_count], LEXIN_PLAN_ITEM_LEN, "%s", txt);
        s_plan_count++;
    }
}

static void lvgl_draw_plant(lv_obj_t *parent, int x, int y, int w, int h, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int cx = x + w / 2;
    int base_y = y + h - 48;                 /* top of the pot */

    /* Pot + rim + soil. */
    lvgl_card(parent, cx - 46, base_y, 92, 60, 0xcf7a4a, 10);
    lvgl_card(parent, cx - 52, base_y - 12, 104, 18, 0xe08a57, 6);
    lvgl_card(parent, cx - 44, base_y - 4, 88, 10, 0x6b4a2f, 4);

    /* Stem grows with completion. */
    int stem_h = 16 + (percent * 150) / 100; /* 16..166 px */
    int stem_top = base_y - 4 - stem_h;
    lvgl_card(parent, cx - 5, stem_top, 10, stem_h, 0x3fa76a, 5);

    if (percent >= 25) {
        lvgl_card(parent, cx - 46, stem_top + stem_h / 2, 44, 18, 0x54c07d, 9);
    }
    if (percent >= 45) {
        lvgl_card(parent, cx + 2, stem_top + stem_h / 3, 44, 18, 0x54c07d, 9);
    }

    if (percent >= 100) {
        lvgl_card(parent, cx - 26, stem_top - 26, 52, 52, 0xff9ec4, LV_RADIUS_CIRCLE);
        lvgl_card(parent, cx - 12, stem_top - 12, 24, 24, 0xffd23f, LV_RADIUS_CIRCLE);
    } else if (percent >= 80) {
        lvgl_card(parent, cx - 20, stem_top - 18, 40, 40, 0xffb3d1, LV_RADIUS_CIRCLE);
        lvgl_card(parent, cx - 9, stem_top - 7, 18, 18, 0xffd23f, LV_RADIUS_CIRCLE);
    } else if (percent >= 60) {
        lvgl_card(parent, cx - 14, stem_top - 8, 28, 28, 0xff9ec4, LV_RADIUS_CIRCLE);
    } else if (percent >= 25) {
        lvgl_card(parent, cx - 9, stem_top - 6, 18, 18, 0x9be0b0, LV_RADIUS_CIRCLE);
    } else {
        lvgl_card(parent, cx - 7, stem_top - 4, 14, 14, 0x8fd6a6, LV_RADIUS_CIRCLE);
    }

    const char *stage = percent >= 100 ? "绽放啦" :
                        percent >= 80 ? "快开花了" :
                        percent >= 60 ? "含苞待放" :
                        percent >= 25 ? "茁壮成长" :
                        percent > 0 ? "发芽中" : "等待播种";
    lvgl_center_label(parent, stage, x, y + h + 2, w, &lexin_cn_20, 0x2f7d5a);
}

static bool lvgl_show_plan_page(void)
{
    ESP_LOGI(TAG, "render plan");
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lexin_launcher_show_screen(LEXIN_SCREEN_PLAN);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_vertical_gradient(scr, 0xeafaf0, 0xd2f0e6);

    lvgl_card(scr, 0, 0, LCD_H_RES, 84, 0x2fb98a, 0);
    lvgl_label(scr, "返回", 32, 26, &lexin_cn_20, 0xffffff);
    const char *title = s_plan_day_view ? s_plan_day_title : "今日计划";
    lvgl_label(scr, title, 150, 22, &lexin_cn_28, 0xffffff);
    char pct[24];
    snprintf(pct, sizeof(pct), "完成 %d%%", s_plan_percent);
    lvgl_label(scr, pct, 800, 30, &lexin_cn_28, 0xffffff);

    /* Plain cards (no padding) so touch hit-testing lines up exactly. */
    lv_obj_t *list = lvgl_card(scr, 40, 108, 560, 460, 0xf4fbf8, 24);
    lvgl_card_border(list, 0xc7e9db, 1);
    const int list_start = 24;
    const int list_step = 34;
    if (s_plan_recording) {
        lvgl_label(list, "正在聆听，请说出今天要完成的事…", 32, 60, &lexin_cn_28, 0x14413a);
        lvgl_label(list, "例如：完成数学作业、跑步三十分钟、看书一小时", 32, 118,
                   &lexin_cn_20, 0x4d7a70);
    } else if (s_plan_count == 0) {
        lvgl_label(list, "今天还没有计划", 32, 66, &lexin_cn_28, 0x14413a);
        if (!s_plan_day_view) {
            lvgl_label(list, "点右下角按钮，用语音说出今天的计划", 32, 126,
                       &lexin_cn_20, 0x4d7a70);
        }
    } else {
        for (int i = 0; i < s_plan_count; i++) {
            int row_y = list_start + i * list_step;
            lv_obj_t *box = lvgl_card(list, 30, row_y, 26, 26,
                                      s_plan_done[i] ? 0x2fb98a : 0xffffff, 8);
            lvgl_card_border(box, 0x2fb98a, 2);
            lv_obj_t *lab = lvgl_label(list, s_plan_items[i], 74, row_y + 1, &lexin_cn_20,
                                       s_plan_done[i] ? 0x8fb3aa : 0x14413a);
            lvgl_label_width(lab, 420);
            /* Per-row delete button ("x"), right-aligned in the row. Past
             * days are editable too, so it shows in both views. */
            lv_obj_t *del = lvgl_card(list, 506, row_y, 34, 26, 0xffffff, 8);
            lvgl_card_border(del, 0xd9534f, 2);
            lvgl_center_label(del, "x", 0, 2, 34, &lv_font_montserrat_20, 0xd9534f);
        }
    }

    lv_obj_t *grow = lvgl_card(scr, 620, 108, 364, 460, 0xf4fbf8, 24);
    lvgl_card_border(grow, 0xc7e9db, 1);
    lvgl_label(grow, "成长花园", 28, 20, &lexin_cn_28, 0x14413a);
    lvgl_label(grow, "每完成一项，小花就长大一点", 28, 62, &lexin_cn_20, 0x4d7a70);
    lvgl_draw_plant(grow, 40, 104, 284, 300, s_plan_percent);

    if (!s_plan_day_view) {
        bool plan_full = !s_plan_recording && s_plan_count >= LEXIN_PLAN_MAX_ITEMS;
        uint32_t rec_color = s_plan_recording ? 0xff7043 :
                             plan_full ? 0x9db8ae : 0x2fb98a;
        lv_obj_t *rec = lvgl_card(scr, 620, 512, 364, 52, rec_color, 26);
        lvgl_center_label(rec,
                          s_plan_recording ? "结束录入" :
                          plan_full ? "计划已满 删除后再录" : "语音录入计划",
                          0, 12, 364, &lexin_cn_28, 0xffffff);
    }

    lvgl_port_unlock();
    return true;
}

static bool handle_plan_touch(uint16_t x, uint16_t y)
{
    /* Back to the calendar. */
    if (x < 170 && y < 84) {
        s_plan_recording = false;
        s_plan_day_view = false;
        lexin_voice_set_mode("chat");
        if (s_cached_calendar[0] != '\0') {
            lvgl_show_calendar_result_page(s_cached_calendar);
            /* Re-pull month completion so the day colours reflect any
             * toggles/deletes just made on the plan page. */
            lexin_plan_fetch_month();
        } else {
            lexin_screen_show_idle();
        }
        return true;
    }

    /* Voice record toggle (only on today's editable view). */
    if (!s_plan_day_view && x >= 620 && x < 984 && y >= 512 && y < 564) {
        if (s_plan_recording) {
            s_plan_recording = false;
            lexin_voice_set_mode("chat");
        } else if (s_plan_count >= LEXIN_PLAN_MAX_ITEMS) {
            /* Button reads "计划已满 删除后再录" — nothing to start. */
            return true;
        } else {
            s_plan_recording = true;
            lexin_voice_set_mode("plan");
        }
        lvgl_show_plan_page();
        return true;
    }

    /* Toggle or delete a checklist item. Today's list and past days from
     * the calendar are both editable; edits on a past day carry its date
     * so the proxy updates the right plan file. */
    bool editable = !s_plan_day_view || s_plan_view_year > 0;
    if (editable && !s_plan_recording && s_plan_count > 0) {
        const int list_x = 40, list_y = 108, list_start = 24, list_step = 34;
        for (int i = 0; i < s_plan_count; i++) {
            int ry = list_y + list_start + i * list_step;
            if (y < ry - 4 || y >= ry + 30) {
                continue;
            }
            /* Delete button: drawn at list-relative x=506 w=34 (abs 546..580).
             * Hit zone is slightly padded on both sides. */
            if (x >= list_x + 500 && x < list_x + 548) {
                ESP_LOGI(TAG, "plan delete item %d (day_view=%d)", i, (int)s_plan_day_view);
                for (int k = i; k < s_plan_count - 1; k++) {
                    s_plan_done[k] = s_plan_done[k + 1];
                    memcpy(s_plan_items[k], s_plan_items[k + 1], LEXIN_PLAN_ITEM_LEN);
                }
                s_plan_count--;
                int done = 0;
                for (int k = 0; k < s_plan_count; k++) {
                    if (s_plan_done[k]) done++;
                }
                s_plan_percent = s_plan_count ? (done * 100) / s_plan_count : 0;
                lvgl_show_plan_page();
                /* persist + refresh from proxy */
                if (s_plan_day_view) {
                    lexin_plan_delete_day(i, s_plan_view_year,
                                          s_plan_view_month, s_plan_view_day);
                } else {
                    lexin_plan_delete(i);
                }
                return true;
            }
            if (x >= list_x + 20 && x < list_x + 500) {
                s_plan_done[i] = !s_plan_done[i];
                int done = 0;
                for (int k = 0; k < s_plan_count; k++) {
                    if (s_plan_done[k]) done++;
                }
                s_plan_percent = s_plan_count ? (done * 100) / s_plan_count : 0;
                lvgl_show_plan_page();
                /* persist + refresh from proxy */
                if (s_plan_day_view) {
                    lexin_plan_toggle_day(i, s_plan_view_year,
                                          s_plan_view_month, s_plan_view_day);
                } else {
                    lexin_plan_toggle(i);
                }
                return true;
            }
            break;
        }
    }
    return true;   /* consume all taps while on the plan page */
}

static bool handle_calendar_touch(uint16_t x, uint16_t y)
{
    /* Plan entry button in the header. */
    if (x >= 724 && x < 974 && y >= 18 && y < 66) {
        lexin_screen_show_plan();
        return true;
    }

    /* Day cell -> that day's plan record (editable). main_card sits at
     * (54,106); cells use the same constants as the render loop. Cells
     * are 72 px wide on a 126 px pitch; attribute taps in the 54 px gap
     * to the nearest cell so day taps don't silently miss. */
    int rel_x = (int)x - 54 - 34;
    int rel_y = (int)y - 106 - 124;
    if (rel_x < 0 || rel_y < 0) {
        return false;
    }
    int col = rel_x / 126;
    int row = rel_y / 34;
    if (col < 0 || col > 6 || row < 0 || row > 5) {
        return false;
    }
    if ((rel_x - col * 126) > 99 && col < 6) {
        col++;   /* right half of the gap belongs to the next cell */
    }

    int year = 0, month = 0, cur_day = 0;
    /* s_cached_calendar holds the full proxy text (TIME:/DATE:/…), not a bare
     * date string — extract DATE: first, same as lvgl_show_calendar_result_page. */
    char date_buf[32];
    copy_field_value(s_cached_calendar, "DATE:", date_buf, sizeof(date_buf));
    if (!parse_date_parts(date_buf, &year, &month, &cur_day)) {
        ESP_LOGW(TAG, "calendar day tap: no DATE in cache (tap %u,%u)", x, y);
        return true;
    }
    int first_weekday = weekday_monday0(year, month, 1);
    int month_days = days_in_month(year, month);
    int index = row * 7 + col;
    if (index < first_weekday || index >= first_weekday + month_days) {
        return true;    /* adjacent-month day: consume the tap, do nothing */
    }
    int tapped_day = index - first_weekday + 1;
    ESP_LOGI(TAG, "calendar day tap -> %04d-%02d-%02d", year, month, tapped_day);
    lexin_plan_fetch_day(year, month, tapped_day);
    return true;
}

static bool lvgl_show_error_page(lexin_action_id_t action_id)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lexin_launcher_show_screen(action_id == LEXIN_ACTION_WEATHER ?
        LEXIN_SCREEN_WEATHER : action_id == LEXIN_ACTION_TIME ?
        LEXIN_SCREEN_CALENDAR : LEXIN_SCREEN_SUGGESTION);
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
    ESP_LOGI(TAG, "Initialize BSP GT911 touch on I2C SDA=%d SCL=%d", TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    bsp_touch_config_t tp_cfg = {0};
    vTaskDelay(pdMS_TO_TICKS(300));
    for (int attempt = 1; attempt <= 6; ++attempt) {
        s_touch = NULL;
        esp_err_t ret = bsp_touch_new(&tp_cfg, &s_touch);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GT911 ready after attempt %d", attempt);
            return true;
        }
        ESP_LOGW(TAG, "GT911 init attempt %d/6 failed: %s",
                 attempt, esp_err_to_name(ret));
        bsp_touch_delete();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ESP_LOGE(TAG, "GT911 unavailable; keep display and AI services running");
    return false;
}

/* GT911 occasionally drops a single "no touch" poll in the middle of a
 * press. Treating that as a release re-arms the trigger while the finger
 * is still down, so the same physical tap fires twice — e.g. the launcher
 * Lock button (x 650..790) retriggers on the lock screen, whose WiFi hit
 * region (x >= 700) overlaps the same spot, jumping straight to the WiFi
 * page. Require several consecutive empty polls before declaring release. */
#define TOUCH_RELEASE_POLLS 3
/* Ignore lock-screen taps briefly after a manual lock so a lingering
 * finger or an accidental double-tap cannot activate lock-screen buttons. */
#define MANUAL_LOCK_GUARD_MS 800

static void touch_task(void *arg)
{
    bool pressed = false;
    int release_polls = 0;
    TickType_t last_trigger_tick = 0;
    TickType_t manual_lock_tick = 0;

    while (1) {
        esp_lcd_touch_read_data(s_touch);

        uint16_t x[1] = {0};
        uint16_t y[1] = {0};
        uint16_t strength[1] = {0};
        uint8_t point_count = 0;
        bool touched = esp_lcd_touch_get_coordinates(s_touch, x, y, strength, &point_count, 1);
        uint16_t raw_x = x[0];
        uint16_t raw_y = y[0];

        if (touched && point_count > 0 && !pressed) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_trigger_tick > pdMS_TO_TICKS(TOUCH_RELEASE_MS)) {
                bool logged_in = lexin_face_auth_is_logged_in();
                ESP_LOGI(TAG, "touch screen=%d raw=(%u,%u) ui=(%u,%u)",
                         (int)lexin_launcher_current_screen(), raw_x, raw_y, x[0], y[0]);
                /* Lock / register screen touch handling (before launcher). */
                lexin_face_auth_snapshot_t fas;
                lexin_face_auth_get_snapshot(&fas);
                if (!logged_in) {
                    /* Right after a manual lock the finger that pressed the
                     * launcher Lock button may still be on the glass at a
                     * spot that overlaps lock-screen buttons (the WiFi entry
                     * region). Swallow taps during the guard window. */
                    if (manual_lock_tick != 0 &&
                        now - manual_lock_tick < pdMS_TO_TICKS(MANUAL_LOCK_GUARD_MS)) {
                        /* Mark as pressed so a finger held past the guard
                         * window still needs a real release + new tap. */
                        pressed = true;
                        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                        continue;
                    }
                    if (lexin_launcher_current_screen() == LEXIN_SCREEN_WIFI) {
                        handle_wifi_touch(x[0], y[0]);
                        last_trigger_tick = now;
                        pressed = true;
                        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                        continue;
                    }
                    if (s_lock_name_input_label == NULL && lock_wifi_touch_hit(x[0], y[0])) {
                        ESP_LOGI(TAG, "lock -> wifi page, touch raw=(%u,%u)",
                                 x[0], y[0]);
                        lexin_launcher_show_screen(LEXIN_SCREEN_WIFI);
                        lvgl_show_wifi_page();
                        last_trigger_tick = now;
                        pressed = true;
                        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                        continue;
                    }
                    /* "创建新用户" button on lock screen: x=64,y=280,w=200,h=56 */
                    if (lock_can_create_user(&fas) &&
                        x[0] >= 64 && x[0] < 264 && y[0] >= 280 && y[0] < 336) {
                        ESP_LOGI(TAG, "lock -> register screen");
                        lvgl_show_register_screen();
                        last_trigger_tick = now;
                        pressed = true;
                        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                        continue;
                    }
                    /* Registration screen keyboard handle */
                    if (s_lock_name_input_label != NULL) {
                        lock_handle_register_touch(x[0], y[0]);
                        last_trigger_tick = now;
                        pressed = true;
                        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                        continue;
                    }
                    /* Block all other touches while locked */
                    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                    continue;
                }
                if (lexin_launcher_current_screen() == LEXIN_SCREEN_WIFI) {
                    handle_wifi_touch(x[0], y[0]);
                    last_trigger_tick = now;
                    pressed = true;
                    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                    continue;
                }
                if (lexin_launcher_current_screen() == LEXIN_SCREEN_EMOTION) {
                    bool handled = handle_emotion_touch(x[0], y[0]);
                    if (handled) {
                        last_trigger_tick = now;
                        pressed = true;
                        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                        continue;
                    }
                }
                if (lexin_launcher_current_screen() == LEXIN_SCREEN_PLAN) {
                    handle_plan_touch(x[0], y[0]);
                    last_trigger_tick = now;
                    pressed = true;
                    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                    continue;
                }
                if (lexin_launcher_current_screen() == LEXIN_SCREEN_CALENDAR &&
                    handle_calendar_touch(x[0], y[0])) {
                    last_trigger_tick = now;
                    pressed = true;
                    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                    continue;
                }
                if (lexin_launcher_current_screen() == LEXIN_SCREEN_LAUNCHER &&
                    launcher_lock_touch_hit(x[0], y[0])) {
                    ESP_LOGI(TAG, "touch x=%u y=%u -> manual lock", x[0], y[0]);
                    lexin_face_auth_lock();
                    lexin_launcher_show_launcher();
                    lvgl_show_lock_screen();
                    manual_lock_tick = now;
                    last_trigger_tick = now;
                    pressed = true;
                    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                    continue;
                }
                if (lexin_launcher_current_screen() == LEXIN_SCREEN_LAUNCHER &&
                    launcher_wifi_touch_hit(x[0], y[0])) {
                    ESP_LOGI(TAG, "touch x=%u y=%u -> wifi page", x[0], y[0]);
                    lexin_launcher_show_screen(LEXIN_SCREEN_WIFI);
                    lvgl_show_wifi_page();
                    last_trigger_tick = now;
                    pressed = true;
                    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
                    continue;
                }
                lexin_action_id_t action_id = x[0] < (LCD_H_RES / 2)
                    ? LEXIN_ACTION_WEATHER
                    : LEXIN_ACTION_TIME;
                lexin_touch_event_t event = lexin_launcher_handle_touch(x[0], y[0]);
                if (event.result == LEXIN_TOUCH_BACK) {
                    ESP_LOGI(TAG, "touch x=%u y=%u -> launcher", x[0], y[0]);
                    if (lexin_launcher_current_screen() == LEXIN_SCREEN_VOICE) {
                        lexin_voice_set_mode("chat");
                    }
                    lexin_screen_show_idle();
                } else if (event.result == LEXIN_TOUCH_OPEN_APP && event.has_action) {
                    action_id = event.action_id;
                    lexin_interaction_record_action(action_id);
                    ESP_LOGI(TAG, "touch x=%u y=%u -> %s", x[0], y[0],
                             action_id == LEXIN_ACTION_WEATHER ? "weather" :
                             action_id == LEXIN_ACTION_TIME ? "calendar" : "ai_insight");
                    lexin_screen_show_querying(action_id);
                    lexin_enqueue_trigger("touch", action_id, action_id);
                    if (action_id == LEXIN_ACTION_TIME) {
                        /* Pull this month's plan completion so the calendar
                         * can colour each day by progress. */
                        lexin_plan_fetch_month();
                    }
                } else if (event.result == LEXIN_TOUCH_OPEN_APP) {
                    lexin_interaction_record_screen(event.screen);
                    if (event.screen == LEXIN_SCREEN_PET) {
                        ESP_LOGI(TAG, "touch x=%u y=%u -> pet ai page", x[0], y[0]);
                        lvgl_show_pet_ai_page();
                    } else if (event.screen == LEXIN_SCREEN_EMOTION) {
                        ESP_LOGI(TAG, "touch x=%u y=%u -> emotion page", x[0], y[0]);
                        lvgl_show_emotion_page();
                    } else if (event.screen == LEXIN_SCREEN_WIFI) {
                        ESP_LOGI(TAG, "touch x=%u y=%u -> wifi page", x[0], y[0]);
                        lvgl_show_wifi_page();
                    } else if (event.screen == LEXIN_SCREEN_VOICE) {
                        ESP_LOGI(TAG, "touch x=%u y=%u -> voice page", x[0], y[0]);
                        lvgl_show_voice_page();
                    } else {
                        ESP_LOGI(TAG, "touch x=%u y=%u -> suggestion page", x[0], y[0]);
                        lvgl_show_suggestion_page();
                    }
                }
                last_trigger_tick = now;
            }
            pressed = true;
            release_polls = 0;
        } else if (touched && point_count > 0) {
            /* Finger still down (already handled press) — not a release. */
            release_polls = 0;
        } else if (pressed) {
            /* Only declare release after several consecutive empty polls so
             * a single dropped GT911 report cannot re-arm the trigger while
             * the finger is still down (which double-fires the same tap on
             * whatever screen was just switched to). */
            if (++release_polls >= TOUCH_RELEASE_POLLS) {
                pressed = false;
                release_polls = 0;
            }
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
    for (int i = 0; i < 32; i++) {
        s_plan_month_percent[i] = 255;   /* no plan yet for any day */
    }
    /* Start with lock screen; the main loop will transition to
     * launcher once face auth succeeds. */
    lvgl_show_lock_screen();
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
    if (!lexin_face_auth_is_logged_in()) {
        lexin_launcher_show_launcher();
        lvgl_show_lock_screen();
        return;
    }
    /* lvgl_show_launcher() updates the screen state under the LVGL
     * lock so state and display can never diverge. */
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
        lexin_launcher_show_screen(LEXIN_SCREEN_WEATHER);
        if (lexin_face_auth_is_logged_in()) {
            lvgl_show_weather_result_page(s_cached_weather);
        }
    } else if (action_id == LEXIN_ACTION_TIME) {
        snprintf(s_cached_calendar, sizeof(s_cached_calendar), "%s", text ? text : "");
        lexin_launcher_show_screen(LEXIN_SCREEN_CALENDAR);
        if (lexin_face_auth_is_logged_in()) {
            lvgl_show_calendar_result_page(s_cached_calendar);
        }
    } else {
        update_pet_tip_from_insight(text);
        lexin_launcher_show_screen(LEXIN_SCREEN_SUGGESTION);
        if (lexin_face_auth_is_logged_in()) {
            lvgl_show_suggestion_page();
        }
    }
}

void lexin_screen_show_error(lexin_action_id_t action_id)
{
    lexin_screen_id_t expected = action_id == LEXIN_ACTION_WEATHER ?
        LEXIN_SCREEN_WEATHER : action_id == LEXIN_ACTION_TIME ?
        LEXIN_SCREEN_CALENDAR : LEXIN_SCREEN_SUGGESTION;
    lexin_launcher_show_screen(expected);
    if (lexin_face_auth_is_logged_in()) {
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

void lexin_screen_set_capture_status(const char *status)
{
    snprintf(s_capture_status, sizeof(s_capture_status), "%s",
             status ? status : "CAPTURE IDLE");
    if (lexin_launcher_current_screen() != LEXIN_SCREEN_EMOTION ||
        s_emotion_view != EMOTION_VIEW_LIVE ||
        !s_lvgl_ready || !s_emotion_capture_status_label) {
        return;
    }
    if (lvgl_port_lock(80)) {
        lv_label_set_text(s_emotion_capture_status_label, s_capture_status);
        lvgl_port_unlock();
    }
}

bool lexin_screen_is_emotion_live(void)
{
    return lexin_launcher_current_screen() == LEXIN_SCREEN_EMOTION &&
           s_emotion_view == EMOTION_VIEW_LIVE;
}

void lexin_screen_show_emotion_report(const char *text, bool monthly)
{
    lvgl_show_emotion_report_page(text, monthly);
}

void lexin_screen_show_plan(void)
{
    s_plan_day_view = false;
    s_plan_recording = false;
    lexin_voice_set_mode("chat");
    /* Show immediately with whatever we have, then refresh from proxy. */
    lvgl_show_plan_page();
    lexin_plan_fetch_today();
}

void lexin_screen_update_plan(const char *plan_text)
{
    parse_plan_text(plan_text);
    s_plan_day_view = false;
    if (s_lvgl_ready && lvgl_port_lock(300)) {
        if (lexin_launcher_current_screen() == LEXIN_SCREEN_PLAN) {
            lvgl_show_plan_page();
        }
        lvgl_port_unlock();
    }
}

void lexin_screen_update_plan_day(const char *plan_text)
{
    parse_plan_text(plan_text);
    s_plan_day_view = true;
    char date[24];
    copy_field_value(plan_text, "DATE:", date, sizeof(date));
    /* DATE is YYYY-MM-DD; show MM-DD in the header. */
    if (strlen(date) >= 10) {
        snprintf(s_plan_day_title, sizeof(s_plan_day_title), "%c%c-%c%c 的计划",
                 date[5], date[6], date[8], date[9]);
        s_plan_view_year = atoi(date);
        s_plan_view_month = atoi(date + 5);
        s_plan_view_day = atoi(date + 8);
    } else {
        snprintf(s_plan_day_title, sizeof(s_plan_day_title), "计划记录");
        s_plan_view_year = 0;
        s_plan_view_month = 0;
        s_plan_view_day = 0;
    }
    if (s_lvgl_ready && lvgl_port_lock(300)) {
        lvgl_show_plan_page();
        lvgl_port_unlock();
    }
}

void lexin_screen_update_plan_month(const char *month_text)
{
    for (int i = 0; i < 32; i++) {
        s_plan_month_percent[i] = 255;
    }
    const char *p = month_text ? strstr(month_text, "PERCENTS:") : NULL;
    if (p) {
        p += strlen("PERCENTS:");
        while (*p) {
            while (*p == ' ' || *p == ',') {
                p++;
            }
            if (*p < '0' || *p > '9') {
                break;
            }
            int day = atoi(p);
            while (*p >= '0' && *p <= '9') {
                p++;
            }
            if (*p == ':') {
                p++;
                int pct = atoi(p);
                while (*p >= '0' && *p <= '9') {
                    p++;
                }
                if (day >= 1 && day <= 31) {
                    s_plan_month_percent[day] = (uint8_t)(pct > 100 ? 100 : (pct < 0 ? 0 : pct));
                }
            }
        }
    }
    if (s_lvgl_ready && lvgl_port_lock(300)) {
        if (lexin_launcher_current_screen() == LEXIN_SCREEN_CALENDAR &&
            s_cached_calendar[0] != '\0') {
            lvgl_show_calendar_result_page(s_cached_calendar);
        }
        lvgl_port_unlock();
    }
}

void lexin_screen_set_plan_recording(bool recording)
{
    s_plan_recording = recording;
    if (s_lvgl_ready && lvgl_port_lock(200)) {
        if (lexin_launcher_current_screen() == LEXIN_SCREEN_PLAN) {
            lvgl_show_plan_page();
        }
        lvgl_port_unlock();
    }
}
