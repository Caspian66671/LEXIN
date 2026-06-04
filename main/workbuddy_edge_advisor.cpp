#include "workbuddy_edge_advisor.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

#if WORKBUDDY_HAS_ESPDL_MODEL
#include <map>
#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
extern const uint8_t workbuddy_advisor_espdl[] asm("_binary_workbuddy_advisor_espdl_start");
#endif

static const char *TAG = "edge_advisor";

#define ADVISOR_FEATURE_COUNT 21

enum advisor_class_t {
    ADVISOR_BREAKFAST = 0,
    ADVISOR_LUNCH,
    ADVISOR_DINNER,
    ADVISOR_RESEARCH_FOCUS,
    ADVISOR_PAPER_READING,
    ADVISOR_WRITE_THESIS,
    ADVISOR_EXERCISE,
    ADVISOR_REST,
    ADVISOR_SLEEP,
    ADVISOR_UMBRELLA,
    ADVISOR_HYDRATE,
    ADVISOR_PLAN,
    ADVISOR_TASK_SPLIT,
    ADVISOR_COMMUTE_CHECK,
    ADVISOR_CLASS_COUNT,
};

struct advisor_context_t {
    char weather[24];
    char advice[24];
    char holiday[32];
    char day_type[16];
    char cloud_model[24];
    char cloud_insight[32];
    char cloud_risk[24];
    char cloud_basis[96];
    char study_state[24];
    int temp_c;
    int rain_percent;
    int hour;
    int touch_weather;
    int touch_calendar;
    int touch_ai;
    int idle_min;
    int focus_min;
    int break_min;
    int focus_rounds;
};

struct advisor_result_t {
    advisor_class_t cls;
    const char *risk;
    int confidence;
    int latency_ms;
    bool espdl;
};

static bool ascii_contains_ci(const char *text, const char *needle)
{
    if (text == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < needle_len && p[i] != '\0' &&
               (char)std::toupper((unsigned char)p[i]) == (char)std::toupper((unsigned char)needle[i])) {
            ++i;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static void copy_field_value(const char *text, const char *key, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (text == nullptr || key == nullptr) {
        return;
    }
    const char *start = strstr(text, key);
    if (start == nullptr) {
        return;
    }
    start += strlen(key);
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    size_t i = 0;
    while (start[i] != '\0' && start[i] != '\r' && start[i] != '\n' && i + 1 < out_size) {
        out[i] = start[i];
        i++;
    }
    out[i] = '\0';
}

static int int_field(const char *text, const char *key, int fallback)
{
    char value[24];
    copy_field_value(text, key, value, sizeof(value));
    if (value[0] == '\0') {
        return fallback;
    }
    return atoi(value);
}

static advisor_context_t parse_context(const char *text)
{
    advisor_context_t ctx = {};
    strcpy(ctx.weather, "UNKNOWN");
    strcpy(ctx.advice, "UNKNOWN");
    strcpy(ctx.holiday, "NONE");
    strcpy(ctx.day_type, "WORKDAY");
    strcpy(ctx.cloud_model, "NONE");
    strcpy(ctx.cloud_insight, "");
    strcpy(ctx.cloud_risk, "LOW");
    strcpy(ctx.cloud_basis, "");
    strcpy(ctx.study_state, "FOCUS");
    ctx.temp_c = int_field(text, "TEMP:", 24);
    ctx.rain_percent = int_field(text, "RAIN:", 0);
    ctx.hour = int_field(text, "HOUR:", 9);
    ctx.touch_weather = int_field(text, "TOUCH_WEATHER:", 0);
    ctx.touch_calendar = int_field(text, "TOUCH_CALENDAR:", 0);
    ctx.touch_ai = int_field(text, "TOUCH_AI:", 0);
    ctx.idle_min = int_field(text, "IDLE_MIN:", 0);
    ctx.focus_min = int_field(text, "FOCUS_MIN:", 0);
    ctx.break_min = int_field(text, "BREAK_MIN:", 0);
    ctx.focus_rounds = int_field(text, "FOCUS_ROUNDS:", 0);
    copy_field_value(text, "WEATHER:", ctx.weather, sizeof(ctx.weather));
    copy_field_value(text, "ADVICE:", ctx.advice, sizeof(ctx.advice));
    copy_field_value(text, "HOLIDAY:", ctx.holiday, sizeof(ctx.holiday));
    copy_field_value(text, "DAY_TYPE:", ctx.day_type, sizeof(ctx.day_type));
    copy_field_value(text, "CLOUD_MODEL:", ctx.cloud_model, sizeof(ctx.cloud_model));
    copy_field_value(text, "CLOUD_INSIGHT:", ctx.cloud_insight, sizeof(ctx.cloud_insight));
    copy_field_value(text, "CLOUD_RISK:", ctx.cloud_risk, sizeof(ctx.cloud_risk));
    copy_field_value(text, "CLOUD_BASIS:", ctx.cloud_basis, sizeof(ctx.cloud_basis));
    copy_field_value(text, "STUDY_STATE:", ctx.study_state, sizeof(ctx.study_state));
    return ctx;
}

static bool is_rest_day(const advisor_context_t &ctx)
{
    return ascii_contains_ci(ctx.day_type, "HOLIDAY") ||
           ascii_contains_ci(ctx.day_type, "WEEKEND") ||
           !ascii_contains_ci(ctx.holiday, "NONE");
}

static bool is_rain_risk(const advisor_context_t &ctx)
{
    return ascii_contains_ci(ctx.weather, "RAIN") || ctx.rain_percent >= 60;
}

static float clamp01(float value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

static void make_features(const advisor_context_t &ctx, float features[ADVISOR_FEATURE_COUNT])
{
    const bool rain = ascii_contains_ci(ctx.weather, "RAIN") || ctx.rain_percent >= 50;
    const bool hot = ascii_contains_ci(ctx.advice, "HOT") || ctx.temp_c >= 30;
    const bool cold = ascii_contains_ci(ctx.advice, "COLD") || ctx.temp_c <= 8;
    const bool rest = is_rest_day(ctx);

    features[0] = std::max(-1.0f, std::min(1.0f, (ctx.hour - 12) / 12.0f));
    features[1] = rest ? 1.0f : -1.0f;
    features[2] = rain ? 1.0f : std::max(0.0f, std::min(1.0f, ctx.rain_percent / 100.0f));
    features[3] = hot ? 1.0f : -0.25f;
    features[4] = cold ? 1.0f : -0.25f;
    features[5] = ascii_contains_ci(ctx.weather, "SUNNY") ? 0.75f : -0.25f;
    features[6] = ascii_contains_ci(ctx.weather, "CLOUDY") ? 0.75f : -0.25f;
    features[7] = ascii_contains_ci(ctx.holiday, "NONE") ? -0.5f : 1.0f;
    features[8] = clamp01(ctx.touch_ai / 5.0f);
    features[9] = clamp01(ctx.touch_weather / 5.0f);
    features[10] = clamp01(ctx.touch_calendar / 5.0f);
    features[11] = clamp01(ctx.idle_min / 45.0f);
    features[12] = ctx.focus_min >= 25 ? 1.0f : clamp01(ctx.focus_min / 25.0f);
    features[13] = ctx.break_min >= 8 ? 1.0f : clamp01(ctx.break_min / 8.0f);
    features[14] = (ctx.hour >= 6 && ctx.hour < 9) ? 1.0f : 0.0f;
    features[15] = (ctx.hour >= 9 && ctx.hour < 11) ? 1.0f : 0.0f;
    features[16] = (ctx.hour >= 11 && ctx.hour < 14) ? 1.0f : 0.0f;
    features[17] = (ctx.hour >= 14 && ctx.hour < 17) ? 1.0f : 0.0f;
    features[18] = (ctx.hour >= 17 && ctx.hour < 19) ? 1.0f : 0.0f;
    features[19] = (ctx.hour >= 19 && ctx.hour < 22) ? 1.0f : 0.0f;
    features[20] = (ctx.hour >= 22 || ctx.hour < 6) ? 1.0f : 0.0f;
}

static advisor_class_t rule_class(const advisor_context_t &ctx)
{
    const bool hot = ascii_contains_ci(ctx.advice, "HOT") || ctx.temp_c >= 30;
    const bool rest = is_rest_day(ctx);

    if (is_rain_risk(ctx)) {
        return ADVISOR_UMBRELLA;
    }
    if (rest) {
        if (ctx.hour >= 7 && ctx.hour < 11) {
            return ADVISOR_EXERCISE;
        }
        if (ctx.hour >= 11 && ctx.hour < 14) {
            return ADVISOR_LUNCH;
        }
        if (ctx.hour >= 14 && ctx.hour < 18) {
            return ADVISOR_REST;
        }
        if (ctx.hour >= 18 && ctx.hour < 22) {
            return ADVISOR_EXERCISE;
        }
        return ADVISOR_SLEEP;
    }
    if (ctx.hour >= 6 && ctx.hour < 9) {
        return ADVISOR_BREAKFAST;
    }
    if (ctx.hour >= 9 && ctx.hour < 11) {
        return ADVISOR_RESEARCH_FOCUS;
    }
    if (ctx.hour >= 11 && ctx.hour < 14) {
        return ADVISOR_LUNCH;
    }
    if (ctx.hour >= 14 && ctx.hour < 17) {
        return hot ? ADVISOR_HYDRATE : ADVISOR_PAPER_READING;
    }
    if (ctx.hour >= 17 && ctx.hour < 19) {
        return ADVISOR_DINNER;
    }
    if (ctx.hour >= 19 && ctx.hour < 22) {
        return ADVISOR_WRITE_THESIS;
    }
    if (ctx.hour >= 22 || ctx.hour < 6) {
        return ADVISOR_SLEEP;
    }
    return ADVISOR_PLAN;
}

static const char *class_name(advisor_class_t cls)
{
    switch (cls) {
    case ADVISOR_BREAKFAST:
        return "BREAKFAST";
    case ADVISOR_LUNCH:
        return "LUNCH";
    case ADVISOR_DINNER:
        return "DINNER";
    case ADVISOR_RESEARCH_FOCUS:
        return "RESEARCH_FOCUS";
    case ADVISOR_PAPER_READING:
        return "PAPER_READING";
    case ADVISOR_WRITE_THESIS:
        return "WRITE_THESIS";
    case ADVISOR_EXERCISE:
        return "EXERCISE";
    case ADVISOR_REST:
        return "REST";
    case ADVISOR_SLEEP:
        return "SLEEP";
    case ADVISOR_UMBRELLA:
        return "UMBRELLA";
    case ADVISOR_HYDRATE:
        return "HYDRATE";
    case ADVISOR_PLAN:
        return "PLAN";
    case ADVISOR_TASK_SPLIT:
        return "TASK_SPLIT";
    case ADVISOR_COMMUTE_CHECK:
        return "COMMUTE_CHECK";
    default:
        return "PLAN";
    }
}

static bool class_from_name(const char *name, advisor_class_t *cls)
{
    if (name == nullptr || cls == nullptr) {
        return false;
    }
    for (int i = 0; i < ADVISOR_CLASS_COUNT; ++i) {
        if (ascii_contains_ci(name, class_name((advisor_class_t)i))) {
            *cls = (advisor_class_t)i;
            return true;
        }
    }
    return false;
}

static advisor_class_t constrain_class(const advisor_context_t &ctx, advisor_class_t candidate)
{
    if (candidate == ADVISOR_UMBRELLA && !is_rain_risk(ctx)) {
        return rule_class(ctx);
    }
    if ((candidate == ADVISOR_HYDRATE || candidate == ADVISOR_REST) &&
        is_rain_risk(ctx)) {
        return ADVISOR_UMBRELLA;
    }
    return candidate;
}

static bool meal_sleep_or_weather_guard(advisor_class_t cls)
{
    return cls == ADVISOR_BREAKFAST ||
           cls == ADVISOR_LUNCH ||
           cls == ADVISOR_DINNER ||
           cls == ADVISOR_SLEEP ||
           cls == ADVISOR_UMBRELLA;
}

static advisor_class_t interaction_adjust_class(const advisor_context_t &ctx, advisor_class_t candidate)
{
    if (meal_sleep_or_weather_guard(candidate)) {
        return candidate;
    }
    if (ctx.focus_min >= 25 || ascii_contains_ci(ctx.study_state, "BREAK_DUE")) {
        return ADVISOR_REST;
    }
    if (ctx.idle_min >= 45) {
        return ADVISOR_HYDRATE;
    }
    if (ctx.break_min >= 8 || ascii_contains_ci(ctx.study_state, "READY_FOCUS")) {
        return ADVISOR_RESEARCH_FOCUS;
    }
    if (ctx.touch_ai >= 3) {
        return ADVISOR_TASK_SPLIT;
    }
    if (ctx.touch_weather >= 3) {
        return is_rain_risk(ctx) ? ADVISOR_UMBRELLA : ADVISOR_COMMUTE_CHECK;
    }
    if (ctx.touch_calendar >= 3) {
        return ADVISOR_PLAN;
    }
    return candidate;
}

static const char *risk_name(const advisor_context_t &ctx, advisor_class_t cls)
{
    if (cls == ADVISOR_UMBRELLA || is_rain_risk(ctx)) {
        return "HIGH";
    }
    if (cls == ADVISOR_SLEEP || cls == ADVISOR_REST || cls == ADVISOR_TASK_SPLIT ||
        ctx.temp_c >= 32 || ctx.temp_c <= 5) {
        return "MEDIUM";
    }
    return "LOW";
}

#if WORKBUDDY_HAS_ESPDL_MODEL
static dl::Model *s_model;

static int8_t quantize_feature(float value, int exponent)
{
    float scale = std::ldexp(1.0f, exponent);
    int quantized = (int)std::lround(value / scale);
    return (int8_t)std::max(-128, std::min(127, quantized));
}

static bool run_espdl_model(const float features[ADVISOR_FEATURE_COUNT], advisor_result_t *result)
{
    if (s_model == nullptr) {
        s_model = new dl::Model((const char *)workbuddy_advisor_espdl,
                                fbs::MODEL_LOCATION_IN_FLASH_RODATA,
                                0,
                                dl::MEMORY_MANAGER_GREEDY,
                                nullptr,
                                false);
        ESP_LOGI(TAG, "ESP-DL advisor model loaded from rodata");
    }

    std::map<std::string, dl::TensorBase *> inputs = s_model->get_inputs();
    std::map<std::string, dl::TensorBase *> outputs = s_model->get_outputs();
    if (inputs.empty() || outputs.empty()) {
        ESP_LOGE(TAG, "ESP-DL advisor model has no input or output tensors");
        return false;
    }

    dl::TensorBase *input = inputs.begin()->second;
    dl::TensorBase *output = outputs.begin()->second;
    int input_exp = input->get_exponent();
    int8_t quantized_features[ADVISOR_FEATURE_COUNT] = {};
    for (int i = 0; i < ADVISOR_FEATURE_COUNT; ++i) {
        quantized_features[i] = quantize_feature(features[i], input_exp);
    }
    if (!input->assign({1, ADVISOR_FEATURE_COUNT}, quantized_features, input_exp, dl::DATA_TYPE_INT8)) {
        ESP_LOGE(TAG, "Failed to assign advisor input tensor");
        return false;
    }

    int64_t start_us = esp_timer_get_time();
    s_model->run();
    int64_t end_us = esp_timer_get_time();

    int best = 0;
    int best_score = -128;
    int8_t *scores = output->get_element_ptr<int8_t>();
    int output_size = std::min(output->get_size(), (int)ADVISOR_CLASS_COUNT);
    for (int i = 0; i < output_size; ++i) {
        if ((int)scores[i] > best_score) {
            best_score = scores[i];
            best = i;
        }
    }

    result->cls = (advisor_class_t)best;
    result->confidence = std::max(50, std::min(99, 55 + best_score / 2));
    result->latency_ms = (int)((end_us - start_us + 999) / 1000);
    result->espdl = true;
    return true;
}
#endif

static advisor_result_t infer_reference_int8(const advisor_context_t &ctx)
{
    int64_t start_us = esp_timer_get_time();
    advisor_result_t result = {};
    result.cls = rule_class(ctx);
    result.risk = risk_name(ctx, result.cls);
    result.confidence = result.risk[0] == 'H' ? 91 : result.risk[0] == 'M' ? 84 : 88;
    result.latency_ms = (int)((esp_timer_get_time() - start_us + 999) / 1000);
    result.espdl = false;
    return result;
}

void workbuddy_edge_advisor_init(void)
{
#if WORKBUDDY_HAS_ESPDL_MODEL
    advisor_result_t unused = {};
    float zero_features[ADVISOR_FEATURE_COUNT] = {};
    if (run_espdl_model(zero_features, &unused)) {
        ESP_LOGI(TAG, "ESP-DL advisor warmup ok");
    } else {
        ESP_LOGW(TAG, "ESP-DL advisor warmup failed, reference INT8 fallback will be used");
    }
#else
    ESP_LOGW(TAG, "ESP-DL advisor model file not embedded; using reference INT8 edge advisor");
#endif
}

bool workbuddy_edge_advisor_infer_text(const char *context_text, char *out_text, size_t out_size)
{
    if (out_text == nullptr || out_size == 0) {
        return false;
    }

    advisor_context_t ctx = parse_context(context_text);
    float features[ADVISOR_FEATURE_COUNT] = {};
    make_features(ctx, features);

    advisor_result_t result = {};
    bool used_espdl = false;
#if WORKBUDDY_HAS_ESPDL_MODEL
    used_espdl = run_espdl_model(features, &result);
#endif
    if (!used_espdl) {
        result = infer_reference_int8(ctx);
    }
    result.cls = interaction_adjust_class(ctx, constrain_class(ctx, result.cls));
    result.risk = risk_name(ctx, result.cls);

    advisor_class_t cloud_cls;
    bool has_cloud = ascii_contains_ci(ctx.cloud_model, "DEEPSEEK") &&
                     class_from_name(ctx.cloud_insight, &cloud_cls);
    advisor_class_t final_cls = result.cls;
    const char *final_risk = risk_name(ctx, final_cls);

    snprintf(out_text, out_size,
             "MODEL: %s\nINSIGHT: %s\nRISK: %s\n"
             "BASIS: %s FEATURES_%d WEATHER_%s HOUR_%d RAIN_%d TOUCH_AI_%d TOUCH_WEATHER_%d FOCUS_%d IDLE_%d\n"
             "EDGE_MODEL: %s\nEDGE_FEATURES: %d\nEDGE_INSIGHT: %s\nEDGE_RISK: %s\nEDGE_CONF: %d\nEDGE_LAT: %dMS\n"
             "CLOUD_MODEL: %s\nCLOUD_INSIGHT: %s\nCLOUD_RISK: %s\n"
             "INTERACTION: W%d C%d A%d IDLE%d FOCUS%d ROUND%d %s",
              has_cloud ? "ESP-DL+DEEPSEEK_REF" : (result.espdl ? "ESP-DL" : "EDGE-INT8"),
             class_name(final_cls),
             final_risk,
              has_cloud ? "FINAL_EDGE_PRIMARY_DEEPSEEK_REF" :
                          (is_rest_day(ctx) ? "FINAL_EDGE_REST_DAY" : "FINAL_EDGE_WORKDAY"),
             ADVISOR_FEATURE_COUNT,
             ctx.weather,
             ctx.hour,
             ctx.rain_percent,
             ctx.touch_ai,
             ctx.touch_weather,
             ctx.focus_min,
             ctx.idle_min,
             result.espdl ? "ESP-DL" : "EDGE-INT8",
             ADVISOR_FEATURE_COUNT,
             class_name(result.cls),
             result.risk,
             result.confidence,
             result.latency_ms,
             ctx.cloud_model,
             ctx.cloud_insight[0] != '\0' ? ctx.cloud_insight : "NONE",
             ctx.cloud_risk,
             ctx.touch_weather,
             ctx.touch_calendar,
             ctx.touch_ai,
             ctx.idle_min,
             ctx.focus_min,
             ctx.focus_rounds,
             ctx.study_state);
    return true;
}
