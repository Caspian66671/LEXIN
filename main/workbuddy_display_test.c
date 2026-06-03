#include "workbuddy_display_test.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
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
#include "workbuddy_actions.h"
#include "workbuddy_launcher.h"
#include "workbuddy_triggers.h"

static const char *TAG = "screen_ui";

LV_FONT_DECLARE(workbuddy_cn_20);
LV_FONT_DECLARE(workbuddy_cn_28);

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

static esp_ldo_channel_handle_t s_mipi_phy_ldo;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_lvgl_disp;
static bool s_lvgl_ready;
static const char *s_pet_state = "待机中";
static const char *s_pet_tip = "科研前先喝水";
static const char *s_pet_reason = "天气和日程待更新";
static const char *s_pet_model_tip = "已准备好";
static uint32_t s_pet_accent = 0x1c98d2;
static int s_pet_service_count;
static workbuddy_action_id_t s_pet_last_action = WORKBUDDY_ACTION_TIME;
static char s_pet_weather_summary[96] = "天气待更新";
static char s_pet_calendar_summary[96] = "日程待更新";
static char s_pet_combined_reason[256] = "天气和日程待更新";
static char s_pet_combined_tip[256] = "科研前先喝水";
static char s_pet_edge_summary[128] = "等待推理";
static char s_pet_edge_meta[96] = "ESP-DL  置信度--";
static char s_pet_cloud_summary[128] = "未接入";
static char s_pet_cloud_meta[96] = "等待云端模型";
static const char *s_pet_weather_scene = "天气待更新";
static const char *s_pet_time_scene = "日程待更新";
static const char *s_pet_emotion_scene = "状态待更新";
static workbuddy_emotion_state_t s_emotion_state = WORKBUDDY_EMOTION_UNKNOWN;

static void copy_field_value(const char *text, const char *key, char *out, size_t out_size);
static int parse_percent_value(const char *text);
static int parse_hour_value(const char *time_value);
static void refresh_pet_combined_tip(void);
static bool lvgl_show_pet_ai_page(void);
static bool lvgl_show_suggestion_page(void);
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
    case WORKBUDDY_EMOTION_HAPPY:
        s_pet_emotion_scene = "陪伴状态积极";
        s_pet_state = "运行良好";
        s_pet_tip = "先推进一项科研任务";
        s_pet_accent = 0x1c98d2;
        break;
    case WORKBUDDY_EMOTION_TIRED:
        s_pet_emotion_scene = "陪伴状态疲惫";
        s_pet_state = "风险提醒";
        s_pet_tip = "休息五分钟再继续";
        s_pet_accent = 0xff9f22;
        break;
    case WORKBUDDY_EMOTION_FOCUSED:
        s_pet_emotion_scene = "专注推进中";
        s_pet_state = "重点推进";
        s_pet_tip = "先推进一项科研任务";
        s_pet_accent = 0x2f86ff;
        break;
    case WORKBUDDY_EMOTION_NEUTRAL:
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

static const char *calendar_pet_suggestion(const char *time_value, const char *holiday_cn)
{
    int hour = parse_hour_value(time_value);
    if (holiday_cn != NULL && strcmp(holiday_cn, "无") != 0) {
        return "节日到了安排休息";
    }
    if (hour >= 0 && hour < 6) {
        return "夜深了早点休息";
    }
    if (hour >= 6 && hour < 11) {
        return "上午适合专注学习";
    }
    if (hour >= 11 && hour < 14) {
        return "午间记得休息";
    }
    if (hour >= 18) {
        return "晚上适合复盘进度";
    }
    return "今天适合继续开发";
}

static void update_pet_tip_from_weather(const char *weather, const char *rain, const char *advice)
{
    s_pet_service_count++;
    s_pet_last_action = WORKBUDDY_ACTION_WEATHER;
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
        s_pet_tip = "晴天适合去实验室";
        s_pet_reason = "天气和降雨概率";
        s_pet_weather_scene = strcmp(weather_cn, "未知") == 0 ? "天气待更新" : "天气稳定";
        s_pet_accent = 0x1c98d2;
    }
    s_pet_model_tip = suggestion_cn;
    refresh_pet_combined_tip();
}

static void update_pet_tip_from_calendar(const char *time_value, const char *holiday_cn)
{
    s_pet_service_count++;
    s_pet_last_action = WORKBUDDY_ACTION_TIME;
    int hour = parse_hour_value(time_value);
    const char *suggestion_cn = calendar_pet_suggestion(time_value, holiday_cn);
    snprintf(s_pet_calendar_summary, sizeof(s_pet_calendar_summary), "时间%s，节假日%s",
             time_value != NULL && time_value[0] != '\0' ? time_value : "未知",
             holiday_cn != NULL ? holiday_cn : "未知");
    if (holiday_cn != NULL && strcmp(holiday_cn, "无") != 0) {
        s_pet_state = "提醒";
        s_pet_tip = "节假日安心休息";
        s_pet_reason = "日期和节假日";
        s_pet_time_scene = "节日提醒";
        s_pet_accent = 0xff9f22;
    } else if (hour >= 0 && hour < 6) {
        s_pet_state = "关心";
        s_pet_tip = "夜深了早点休息";
        s_pet_reason = "北京时间";
        s_pet_time_scene = "夜间";
        s_pet_accent = 0x5f75ff;
    } else if (hour >= 6 && hour < 11) {
        s_pet_state = "开心";
        s_pet_tip = "早安先吃早餐";
        s_pet_reason = "北京时间";
        s_pet_time_scene = "上午";
        s_pet_accent = 0x1c98d2;
    } else if (hour >= 11 && hour < 14) {
        s_pet_state = "关心";
        s_pet_tip = "午饭别拖太久";
        s_pet_reason = "北京时间";
        s_pet_time_scene = "午间";
        s_pet_accent = 0xff9f22;
    } else if (hour >= 18) {
        s_pet_state = "提醒";
        s_pet_tip = "晚饭后整理进度";
        s_pet_reason = "北京时间";
        s_pet_time_scene = "夜间";
        s_pet_accent = 0x9465ff;
    } else {
        s_pet_state = "提醒";
        s_pet_tip = "下午适合读论文";
        s_pet_reason = "时间和节假日";
        s_pet_time_scene = "下午";
        s_pet_accent = 0x1c98d2;
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
        return "先吃晚饭再科研";
    }
    if (ascii_contains_ci(insight, "RESEARCH_FOCUS")) {
        return "上午推进实验主线";
    }
    if (ascii_contains_ci(insight, "PAPER_READING")) {
        return "读一篇核心论文";
    }
    if (ascii_contains_ci(insight, "EXPERIMENT")) {
        return "整理实验和数据";
    }
    if (ascii_contains_ci(insight, "WRITE_THESIS")) {
        return "写一点论文初稿";
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
        return "先推进一项科研任务";
    }
    if (ascii_contains_ci(insight, "PLAN")) {
        return "整理明天科研计划";
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
    return "按节奏完成科研";
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
        return "先吃午饭，下午再开题";
    }
    if (ascii_contains_ci(insight, "DINNER")) {
        return "先补晚饭，再做复盘";
    }
    if (ascii_contains_ci(insight, "RESEARCH_FOCUS")) {
        return "把实验主线推进一小步";
    }
    if (ascii_contains_ci(insight, "PAPER_READING")) {
        return "读论文时先抓方法和结论";
    }
    if (ascii_contains_ci(insight, "EXPERIMENT")) {
        return "先校准实验，再记录数据";
    }
    if (ascii_contains_ci(insight, "WRITE_THESIS")) {
        return "写一段论文，别等状态完美";
    }
    if (ascii_contains_ci(insight, "EXERCISE")) {
        return "今天安排运动，给大脑换气";
    }
    if (ascii_contains_ci(insight, "HYDRATE") || ascii_contains_ci(insight, "CARE")) {
        return "喝水伸展，状态会回来";
    }
    if (ascii_contains_ci(insight, "BREAK") || ascii_contains_ci(insight, "RHYTHM")) {
        return "暂停几分钟，再继续科研";
    }
    if (ascii_contains_ci(insight, "FOCUS")) {
        return "先完成最重要的一件事";
    }
    if (ascii_contains_ci(insight, "PLAN")) {
        return "今晚把明天任务排清楚";
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
    return "按当前节奏稳步推进";
}

static const char *edge_basis_text(const char *basis)
{
    if (ascii_contains_ci(basis, "HOLIDAY") || ascii_contains_ci(basis, "WEEKEND")) {
        return "ESP-DL量化模型  节假日  休息运动";
    }
    if (ascii_contains_ci(basis, "WEATHER_RAIN")) {
        return "ESP-DL量化模型  天气风险  日历";
    }
    if (ascii_contains_ci(basis, "WORKDAY")) {
        return "ESP-DL量化模型  工作日  科研节奏";
    }
    return "ESP-DL量化模型  天气日历  科研节奏";
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
    s_pet_last_action = WORKBUDDY_ACTION_AI_INSIGHT;
    const char *edge_value = field_has_value(edge_insight) ? edge_insight : insight;
    const bool cloud_ready = ascii_contains_ci(cloud_model, "DEEPSEEK") && field_has_value(cloud_insight);
    s_pet_tip = enterprise_insight_tip(edge_value);
    s_pet_reason = "硕士研伴建议";
    s_pet_weather_scene = ascii_contains_ci(basis, "WEATHER_RAIN") ? "天气风险" : "天气稳定";
    s_pet_time_scene = (ascii_contains_ci(basis, "HOLIDAY") || ascii_contains_ci(basis, "WEEKEND")) ? "休息节奏" : "科研节奏";
    s_pet_emotion_scene = "研伴建议";
    s_pet_model_tip = (ascii_contains_ci(model, "DEEPSEEK") && ascii_contains_ci(model, "ESP-DL")) ? "ESP-DL + DeepSeek 双模型" :
                      ascii_contains_ci(model, "ESP-DL") ? "ESP-DL 本地推理" :
                      ascii_contains_ci(model, "EDGE-INT8") ? "本地量化推理" :
                      ascii_contains_ci(model, "DEEPSEEK") ? "DeepSeek 已接入" : "离线建议";
    snprintf(s_pet_edge_summary, sizeof(s_pet_edge_summary), "%s", enterprise_insight_tip(edge_value));
    snprintf(s_pet_edge_meta, sizeof(s_pet_edge_meta), "ESP-DL  置信度%.8s  延迟%.12s",
             field_has_value(edge_conf) ? edge_conf : "--",
             field_has_value(edge_lat) ? edge_lat : "--");
    snprintf(s_pet_cloud_summary, sizeof(s_pet_cloud_summary), "%s",
             cloud_ready ? deepseek_insight_tip(cloud_insight) : "未接入");
    snprintf(s_pet_cloud_meta, sizeof(s_pet_cloud_meta), "%s",
             cloud_ready ? "DeepSeek云端研判" : "请先连接DeepSeek");

    if (ascii_contains_ci(risk, "HIGH")) {
        s_pet_state = "贴心提醒";
        s_pet_accent = 0xff9f22;
    } else if (ascii_contains_ci(risk, "MEDIUM")) {
        s_pet_state = "节奏提醒";
        s_pet_accent = 0x9465ff;
    } else {
        s_pet_state = "科研状态";
        s_pet_accent = 0x1c98d2;
    }

    snprintf(s_pet_combined_reason, sizeof(s_pet_combined_reason), "%s", edge_basis_text(basis));
    snprintf(s_pet_combined_tip, sizeof(s_pet_combined_tip), "%s", s_pet_edge_summary);
}

static void update_pet_tip_querying(workbuddy_action_id_t action_id)
{
    s_pet_last_action = action_id;
    if (action_id == WORKBUDDY_ACTION_WEATHER) {
        s_pet_state = "查询天气";
        s_pet_tip = "正在看天气";
        s_pet_reason = "查询请求";
        s_pet_accent = 0x1c98d2;
    } else if (action_id == WORKBUDDY_ACTION_TIME) {
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
    s_pet_model_tip = action_id == WORKBUDDY_ACTION_AI_INSIGHT ? "DeepSeek 思考中" : "已准备好";
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

static void lvgl_draw_sun_cloud(lv_obj_t *parent)
{
    lv_obj_t *sun = lv_obj_create(parent);
    lv_obj_remove_style_all(sun);
    lv_obj_set_pos(sun, 752, 176);
    lv_obj_set_size(sun, 118, 118);
    lvgl_set_bg(sun, 0xffd23f);
    lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *cloud = lv_obj_create(parent);
    lv_obj_remove_style_all(cloud);
    lv_obj_set_pos(cloud, 650, 274);
    lv_obj_set_size(cloud, 284, 82);
    lvgl_set_bg(cloud, 0xffffff);
    lv_obj_set_style_radius(cloud, 38, 0);

    lv_obj_t *c1 = lv_obj_create(parent);
    lv_obj_remove_style_all(c1);
    lv_obj_set_pos(c1, 634, 232);
    lv_obj_set_size(c1, 108, 108);
    lvgl_set_bg(c1, 0xffffff);
    lv_obj_set_style_radius(c1, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *c2 = lv_obj_create(parent);
    lv_obj_remove_style_all(c2);
    lv_obj_set_pos(c2, 734, 214);
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
    lvgl_center_label(icon, "晴", 0, 78, 104, &workbuddy_cn_20, 0xffffff);
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
    lvgl_center_label(icon, "日程", 0, 78, 104, &workbuddy_cn_20, 0x9a6500);
}

static void lvgl_draw_ai_app_icon(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *icon = lvgl_card(parent, x, y, 104, 104, 0x9465ff, 24);
    lvgl_set_vertical_gradient(icon, 0xa886ff, 0x7052f5);
    lv_obj_set_style_border_width(icon, 1, 0);
    lv_obj_set_style_border_color(icon, lv_color_hex(0xc9b8ff), 0);

    lvgl_card(icon, 22, 22, 10, 10, 0xffffff, LV_RADIUS_CIRCLE);
    lvgl_card(icon, 74, 24, 8, 8, 0xffffff, LV_RADIUS_CIRCLE);
    lvgl_card(icon, 26, 74, 8, 8, 0xffffff, LV_RADIUS_CIRCLE);
    lvgl_center_label(icon, "AI", 0, 34, 104, &lv_font_montserrat_32, 0xffffff);
    lvgl_center_label(icon, "研伴", 0, 72, 104, &workbuddy_cn_20, 0xf3efff);
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
    lvgl_label(scr, "AI桌宠助手", 48, 22, &workbuddy_cn_28, 0xffffff);

    lv_obj_t *pet_card = lvgl_glass_card(scr, 66, 132, 348, 344, 28);
    lvgl_label(pet_card, "小伙伴在线", 34, 34, &workbuddy_cn_28, 0x10283e);
    lvgl_label(pet_card, "今日陪伴", 36, 92, &workbuddy_cn_20, 0x577489);
    lv_obj_t *tip_label = lvgl_label(pet_card, s_pet_tip, 36, 126, &workbuddy_cn_20, 0x10283e);
    lvgl_label_width(tip_label, 276);
    lvgl_card(pet_card, 42, 212, 100, 100, 0xffd23f, 50);
    lvgl_card(pet_card, 110, 250, 108, 52, 0xffffff, 26);
    lvgl_card(pet_card, 90, 226, 68, 68, 0xffffff, LV_RADIUS_CIRCLE);
    lv_obj_t *state_pill = lvgl_card(pet_card, 188, 204, 120, 40, 0xe8f8ff, 20);
    lv_obj_set_style_bg_opa(state_pill, LV_OPA_80, 0);
    lv_obj_set_style_border_width(state_pill, 2, 0);
    lv_obj_set_style_border_color(state_pill, lv_color_hex(s_pet_accent), 0);
    lv_obj_t *state_label = lvgl_label(state_pill, s_pet_state, 12, 9, &workbuddy_cn_20, s_pet_accent);
    lvgl_label_width(state_label, 96);
    char service_text[24];
    snprintf(service_text, sizeof(service_text), "服务%d次", s_pet_service_count);
    lvgl_label(pet_card, service_text, 196, 258, &workbuddy_cn_20, 0x577489);
    lv_obj_t *analysis_btn = lvgl_card(pet_card, 174, 292, 142, 38, 0xe8f8ff, 19);
    lv_obj_set_style_bg_opa(analysis_btn, LV_OPA_80, 0);
    lv_obj_set_style_border_width(analysis_btn, 2, 0);
    lv_obj_set_style_border_color(analysis_btn, lv_color_hex(s_pet_accent), 0);
    lvgl_label(analysis_btn, "查看洞察", 24, 8, &workbuddy_cn_20, s_pet_accent);

    lv_obj_t *panel = lvgl_glass_card(scr, 456, 132, 486, 344, 28);
    lvgl_draw_weather_app_icon(panel, 44, 48);
    lvgl_draw_calendar_app_icon(panel, 190, 48);
    lvgl_draw_ai_app_icon(panel, 336, 48);
    lvgl_center_label(panel, "天气提醒", 44, 170, 104, &workbuddy_cn_20, 0x10283e);
    lvgl_center_label(panel, "日程提醒", 190, 170, 104, &workbuddy_cn_20, 0x10283e);
    lvgl_center_label(panel, "研伴建议", 336, 170, 104, &workbuddy_cn_20, 0x10283e);
    lvgl_center_label(panel, "天气  日历  双模型研伴", 0, 270, 486, &workbuddy_cn_20, 0x577489);

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
    lvgl_label(scr, "返回", 32, 28, &workbuddy_cn_20, 0xffffff);
    lvgl_label(scr, "研伴建议", 110, 24, &workbuddy_cn_28, 0xffffff);

    lv_obj_t *main_card = lvgl_glass_card(scr, 78, 132, 438, 340, 28);
    lvgl_label(main_card, "ESP-DL模型", 40, 34, &workbuddy_cn_28, 0x10283e);
    lvgl_label(main_card, "本地推理结果", 42, 92, &workbuddy_cn_20, 0x577489);
    lv_obj_t *tip_label = lvgl_label(main_card, s_pet_combined_tip, 42, 124, &workbuddy_cn_28, s_pet_accent);
    lvgl_label_width(tip_label, 330);
    lvgl_label(main_card, "分析依据", 42, 190, &workbuddy_cn_20, 0x577489);
    lv_obj_t *reason_label = lvgl_label(main_card, s_pet_combined_reason, 42, 222, &workbuddy_cn_20, 0x10283e);
    lvgl_label_width(reason_label, 330);
    lv_obj_t *edge_meta = lvgl_label(main_card, s_pet_edge_meta, 42, 282, &workbuddy_cn_20, 0x577489);
    lvgl_label_width(edge_meta, 330);

    lv_obj_t *ai_card = lvgl_glass_card(scr, 546, 132, 392, 340, 28);
    lvgl_label(ai_card, "DeepSeek", 44, 34, &workbuddy_cn_28, 0x10283e);
    lvgl_label(ai_card, "云端建议", 46, 92, &workbuddy_cn_20, 0x577489);
    lv_obj_t *cloud_label = lvgl_label(ai_card, s_pet_cloud_summary, 46, 124, &workbuddy_cn_28, 0x9465ff);
    lvgl_label_width(cloud_label, 300);
    lvgl_label(ai_card, "模型说明", 46, 190, &workbuddy_cn_20, 0x577489);
    lv_obj_t *cloud_meta = lvgl_label(ai_card, s_pet_cloud_meta, 46, 222, &workbuddy_cn_20, 0x10283e);
    lvgl_label_width(cloud_meta, 300);
    lvgl_label(ai_card, "对比价值", 46, 264, &workbuddy_cn_20, 0x577489);
    lv_obj_t *compare_label = lvgl_label(ai_card, "同一输入  不同推理风格", 46, 294, &workbuddy_cn_20, 0x10283e);
    lvgl_label_width(compare_label, 300);

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
    lvgl_label(scr, "返回", 32, 28, &workbuddy_cn_20, 0xffffff);
    lvgl_label(scr, "生活助理", 110, 24, &workbuddy_cn_28, 0xffffff);

    lv_obj_t *main_card = lvgl_glass_card(scr, 92, 132, 430, 340, 28);
    lvgl_label(main_card, "今日状态", 40, 34, &workbuddy_cn_28, 0x10283e);
    lvgl_label(main_card, "当前状态", 42, 92, &workbuddy_cn_20, 0x577489);
    lv_obj_t *state_label = lvgl_label(main_card, s_pet_state, 42, 122, &workbuddy_cn_28, s_pet_accent);
    lvgl_label_width(state_label, 330);
    lvgl_label(main_card, "参考信息", 42, 180, &workbuddy_cn_20, 0x577489);
    lv_obj_t *reason_label = lvgl_label(main_card, s_pet_combined_reason, 42, 212, &workbuddy_cn_20, 0x10283e);
    lvgl_label_width(reason_label, 330);
    lvgl_label(main_card, "小建议", 42, 258, &workbuddy_cn_20, 0x577489);
    lv_obj_t *tip_label = lvgl_label(main_card, s_pet_combined_tip, 42, 288, &workbuddy_cn_20, 0x10283e);
    lvgl_label_width(tip_label, 330);

    lv_obj_t *pet_card = lvgl_glass_card(scr, 556, 132, 360, 340, 28);
    lvgl_card(pet_card, 68, 50, 118, 118, 0xffd23f, 59);
    lvgl_card(pet_card, 152, 98, 134, 64, 0xffffff, 32);
    lvgl_card(pet_card, 126, 74, 78, 78, 0xffffff, LV_RADIUS_CIRCLE);
    lv_obj_t *pill = lvgl_card(pet_card, 54, 202, 252, 48, 0xe8f8ff, 24);
    lv_obj_set_style_bg_opa(pill, LV_OPA_80, 0);
    lv_obj_set_style_border_width(pill, 2, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(s_pet_accent), 0);
    lvgl_label(pill, s_pet_last_action == WORKBUDDY_ACTION_WEATHER ? "天气提醒" :
               s_pet_last_action == WORKBUDDY_ACTION_TIME ? "日程提醒" : "生活提醒",
               58, 11, &workbuddy_cn_20, s_pet_accent);

    char service_text[32];
    snprintf(service_text, sizeof(service_text), "已服务%d次", s_pet_service_count);
    lvgl_label(pet_card, "模型亮点", 64, 270, &workbuddy_cn_20, 0x577489);
    lv_obj_t *model_label = lvgl_label(pet_card, s_pet_model_tip, 64, 300, &workbuddy_cn_20, 0x10283e);
    lvgl_label_width(model_label, 238);
    lvgl_label(scr, service_text, 616, 488, &workbuddy_cn_20, 0x577489);
    lvgl_label(scr, "硕士研伴桌宠", 740, 488, &workbuddy_cn_20, 0x577489);

    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_querying_page(workbuddy_action_id_t action_id)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    uint32_t bg = action_id == WORKBUDDY_ACTION_WEATHER ? 0xe9f6ff :
                  action_id == WORKBUDDY_ACTION_TIME ? 0xf3fbf7 : 0xf4f1ff;
    uint32_t accent = action_id == WORKBUDDY_ACTION_WEATHER ? 0x1c7ed6 :
                      action_id == WORKBUDDY_ACTION_TIME ? 0x0f8a5f : 0x9465ff;
    lvgl_set_bg(scr, bg);
    lvgl_label(scr, "返回", 32, 28, &workbuddy_cn_20, 0x16324f);
    lvgl_label(scr, action_id == WORKBUDDY_ACTION_WEATHER ? "天气提醒" :
               action_id == WORKBUDDY_ACTION_TIME ? "日程提醒" : "研伴建议",
               70, 154, &workbuddy_cn_28, accent);
    lvgl_label(scr, "加载中", 74, 232, &workbuddy_cn_28, 0x42627d);
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
    lvgl_set_vertical_gradient(scr, 0x54b6e9, 0xd9f4ff);
    lvgl_card(scr, 0, 0, LCD_H_RES, 84, 0x2f9ad6, 0);
    lv_obj_set_style_bg_opa(lvgl_card(scr, 0, 84, LCD_H_RES, 132, 0x7bc9ef, 0), LV_OPA_40, 0);
    lv_obj_t *hero = lvgl_glass_card(scr, 66, 112, 892, 318, 28);
    lv_obj_t *rain_card = lvgl_glass_card(scr, 86, 454, 372, 106, 18);
    lv_obj_t *advice_card = lvgl_glass_card(scr, 490, 454, 448, 106, 18);

    lv_obj_t *back = lvgl_label(scr, "返回", 32, 23, &workbuddy_cn_20, 0xffffff);
    lv_obj_set_style_text_letter_space(back, 1, 0);
    lvgl_label(hero, "西安", 36, 30, &workbuddy_cn_20, 0x10283e);
    lvgl_label(hero, "天气提醒", 40, 84, &workbuddy_cn_20, 0x577489);
    lv_obj_t *temp_label = lvgl_label(hero, temp_number, 36, 110, &lv_font_montserrat_48, 0x0c253b);
    lv_obj_set_style_text_letter_space(temp_label, 1, 0);
    lv_obj_t *unit_label = lvgl_label(hero, "°C", 0, 0, &lv_font_montserrat_28, 0x395467);
    lv_obj_align_to(unit_label, temp_label, LV_ALIGN_OUT_RIGHT_MID, 12, 2);
    lvgl_label(hero, weather_cn, 40, 248, &workbuddy_cn_28, 0x194d69);
    lvgl_draw_sun_cloud(scr);

    lvgl_card_border(rain_card, 0xffffff, 1);
    lvgl_label(rain_card, "降雨概率", 30, 18, &workbuddy_cn_20, 0x577489);
    lvgl_label(rain_card, rain, 30, 48, &lv_font_montserrat_28, 0x10283e);

    lvgl_card_border(advice_card, 0xffffff, 1);
    lvgl_label(advice_card, "桌宠建议", 30, 18, &workbuddy_cn_20, 0x577489);
    lv_obj_t *advice_label = lvgl_label(advice_card, suggestion_cn, 30, 48, &workbuddy_cn_20, 0x10283e);
    lvgl_label_width(advice_label, 388);

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
    copy_field_value(text, "TIME:", time_value, sizeof(time_value));
    copy_field_value(text, "DATE:", date, sizeof(date));
    copy_field_value(text, "LUNAR:", lunar, sizeof(lunar));
    copy_field_value(text, "HOLIDAY:", holiday, sizeof(holiday));
    const char *lunar_cn = lunar_to_cn(lunar);
    const char *holiday_cn = holiday_to_cn(holiday);
    const char *suggestion_cn = calendar_pet_suggestion(time_value, holiday_cn);
    update_pet_tip_from_calendar(time_value, holiday_cn);
    int year = 2026;
    int month = 5;
    int day = 30;
    if (!parse_date_parts(date, &year, &month, &day) || !valid_date_parts(year, month, day)) {
        snprintf(date, sizeof(date), "2026-05-30");
        year = 2026;
        month = 5;
        day = 30;
    }
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

    lv_obj_t *back = lvgl_label(scr, "返回", 32, 23, &workbuddy_cn_20, 0xffffff);
    lv_obj_set_style_text_letter_space(back, 1, 0);
    lvgl_label(main_card, "提醒服务", 32, 30, &workbuddy_cn_28, 0x10283e);
    lvgl_label(main_card, month_name_cn(month), 32, 68, &workbuddy_cn_20, 0x667784);
    lvgl_label(main_card, time_value, 734, 34, &lv_font_montserrat_32, 0x1b8ed2);

    const char *week[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    const int start_x = 34;
    const int start_y = 124;
    const int cell_w = 126;
    const int cell_h = 34;
    for (int i = 0; i < 7; i++) {
        lv_obj_t *week_label = lvgl_label(main_card, week[i], start_x + i * cell_w, 102, &workbuddy_cn_20, 0x667784);
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
    lvgl_label(date_card, "日期", 20, 12, &workbuddy_cn_20, 0x606a76);
    lv_obj_t *date_label = lvgl_label(date_card, date, 20, 34, &lv_font_montserrat_20, 0x151b24);
    lvgl_label_width(date_label, 172);

    lvgl_card_border(lunar_card, 0xffffff, 1);
    lvgl_label(lunar_card, "农历", 20, 12, &workbuddy_cn_20, 0x606a76);
    lv_obj_t *lunar_label = lvgl_label(lunar_card, lunar_cn, 20, 34, &workbuddy_cn_20, 0x2f86ff);
    lvgl_label_width(lunar_label, 172);

    lvgl_card_border(holiday_card, 0xffffff, 1);
    lvgl_label(holiday_card, "节假日", 20, 12, &workbuddy_cn_20, 0x606a76);
    lv_obj_t *holiday_label = lvgl_label(holiday_card, holiday_cn, 20, 34, &workbuddy_cn_20, 0x151b24);
    lvgl_label_width(holiday_label, 124);

    lvgl_card_border(suggestion_card, 0xffffff, 1);
    lvgl_label(suggestion_card, "今日安排", 20, 12, &workbuddy_cn_20, 0x606a76);
    lv_obj_t *suggestion_label = lvgl_label(suggestion_card, suggestion_cn, 20, 34, &workbuddy_cn_20, 0x10283e);
    lvgl_label_width(suggestion_label, 196);

    lvgl_port_unlock();
    return true;
}

static bool lvgl_show_error_page(workbuddy_action_id_t action_id)
{
    if (!s_lvgl_ready || !lvgl_port_lock(1000)) {
        return false;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lvgl_set_bg(scr, 0xfff5f5);
    lvgl_label(scr, "返回", 32, 26, &workbuddy_cn_20, 0x5a1f1f);
    lvgl_label(scr, action_id == WORKBUDDY_ACTION_WEATHER ? "天气错误" :
               action_id == WORKBUDDY_ACTION_TIME ? "日程错误" : "AI错误",
               72, 154, &workbuddy_cn_28, 0xc03434);
    lvgl_label(scr, "检查网络后再试", 76, 238, &workbuddy_cn_28, 0x7a4444);
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

static void init_touch(void)
{
    ESP_LOGI(TAG, "Create touch I2C bus SDA=%d SCL=%d", TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = TOUCH_I2C_FREQ_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io));

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
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch));
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
                workbuddy_action_id_t action_id = x[0] < (LCD_H_RES / 2)
                    ? WORKBUDDY_ACTION_WEATHER
                    : WORKBUDDY_ACTION_TIME;
                workbuddy_touch_event_t event = workbuddy_launcher_handle_touch(x[0], y[0]);
                if (event.result == WORKBUDDY_TOUCH_BACK) {
                    ESP_LOGI(TAG, "touch x=%u y=%u -> launcher", x[0], y[0]);
                    workbuddy_screen_show_idle();
                } else if (event.result == WORKBUDDY_TOUCH_OPEN_APP && event.has_action) {
                    action_id = event.action_id;
                    ESP_LOGI(TAG, "touch x=%u y=%u -> %s", x[0], y[0],
                             action_id == WORKBUDDY_ACTION_WEATHER ? "weather" :
                             action_id == WORKBUDDY_ACTION_TIME ? "calendar" : "ai_insight");
                    workbuddy_screen_show_querying(action_id);
                    workbuddy_enqueue_trigger("touch", action_id, action_id);
                } else if (event.result == WORKBUDDY_TOUCH_OPEN_APP) {
                    if (event.screen == WORKBUDDY_SCREEN_PET) {
                        ESP_LOGI(TAG, "touch x=%u y=%u -> pet ai page", x[0], y[0]);
                        lvgl_show_pet_ai_page();
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
    workbuddy_launcher_init();
    workbuddy_screen_show_idle();
    init_touch();

    ESP_LOGI(TAG, "Touch UI ready: launcher apps");
    xTaskCreate(touch_task, "touch_task", 4096, NULL, 5, NULL);
    vTaskDelete(NULL);
}

void workbuddy_start_screen_ui(void)
{
    xTaskCreate(screen_ui_task, "screen_ui", 8192, NULL, 4, NULL);
}

void workbuddy_screen_show_idle(void)
{
    workbuddy_launcher_show_launcher();
    lvgl_show_launcher();
}

void workbuddy_screen_show_querying(workbuddy_action_id_t action_id)
{
    update_pet_tip_querying(action_id);
    if (action_id == WORKBUDDY_ACTION_WEATHER) {
        workbuddy_launcher_show_screen(WORKBUDDY_SCREEN_WEATHER);
        lvgl_show_querying_page(action_id);
    } else if (action_id == WORKBUDDY_ACTION_TIME) {
        workbuddy_launcher_show_screen(WORKBUDDY_SCREEN_CALENDAR);
        lvgl_show_querying_page(action_id);
    } else {
        workbuddy_launcher_show_screen(WORKBUDDY_SCREEN_SUGGESTION);
        lvgl_show_querying_page(action_id);
    }
}

void workbuddy_screen_show_result(workbuddy_action_id_t action_id)
{
    (void)action_id;
}

void workbuddy_screen_show_result_text(workbuddy_action_id_t action_id, const char *text)
{
    if (action_id == WORKBUDDY_ACTION_WEATHER) {
        workbuddy_launcher_show_screen(WORKBUDDY_SCREEN_WEATHER);
        lvgl_show_weather_result_page(text);
    } else if (action_id == WORKBUDDY_ACTION_TIME) {
        workbuddy_launcher_show_screen(WORKBUDDY_SCREEN_CALENDAR);
        lvgl_show_calendar_result_page(text);
    } else {
        update_pet_tip_from_insight(text);
        workbuddy_launcher_show_screen(WORKBUDDY_SCREEN_SUGGESTION);
        lvgl_show_suggestion_page();
    }
}

void workbuddy_screen_show_error(workbuddy_action_id_t action_id)
{
    lvgl_show_error_page(action_id);
}

void workbuddy_screen_update_ai_context(workbuddy_face_state_t face, workbuddy_emotion_state_t emotion)
{
    (void)face;
    s_emotion_state = emotion;
    update_pet_from_ai_context();
}
