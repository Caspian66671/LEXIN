#include "lexin_interaction.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#define LEXIN_FOCUS_ROUND_MIN 25
#define LEXIN_BREAK_READY_MIN 5
#define LEXIN_IDLE_LONG_MIN 30
#define LEXIN_INT_NVS_NS        "lexin_int"
#define LEXIN_INT_NVS_MAX_KEY   64

static bool s_initialized;
static char s_user_id[32];
static int64_t s_last_touch_us;
static int64_t s_focus_start_us;
static int s_idle_before_last_touch_min;
static uint32_t s_weather_taps;
static uint32_t s_calendar_taps;
static uint32_t s_ai_taps;
static uint32_t s_pet_taps;
static uint32_t s_total_taps;
static int64_t s_last_save_ms;

static void int_nvs_save(void);
static void int_nvs_load(void);

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static int elapsed_min(int64_t start, int64_t end)
{
    if (start <= 0 || end <= start) {
        return 0;
    }
    return (int)((end - start) / (60LL * 1000LL * 1000LL));
}

void lexin_interaction_init(void)
{
    int64_t now = now_us();
    s_initialized = true;
    s_last_touch_us = now;
    s_focus_start_us = now;
    s_idle_before_last_touch_min = 0;
    s_weather_taps = 0;
    s_calendar_taps = 0;
    s_ai_taps = 0;
    s_pet_taps = 0;
    s_total_taps = 0;
}

static void ensure_initialized(void)
{
    if (!s_initialized) {
        lexin_interaction_init();
    }
}

static void record_touch_time(void)
{
    ensure_initialized();
    int64_t now = now_us();
    s_idle_before_last_touch_min = elapsed_min(s_last_touch_us, now);
    s_last_touch_us = now;
    s_total_taps++;
    /* Auto-save every 30 s when user is active */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_save_ms > 30000) {
        int_nvs_save();
        s_last_save_ms = now_ms;
    }
}

void lexin_interaction_record_action(lexin_action_id_t action_id)
{
    record_touch_time();
    switch (action_id) {
    case LEXIN_ACTION_WEATHER:
        s_weather_taps++;
        break;
    case LEXIN_ACTION_TIME:
        s_calendar_taps++;
        break;
    case LEXIN_ACTION_AI_INSIGHT:
        s_ai_taps++;
        break;
    default:
        break;
    }
}

void lexin_interaction_record_screen(lexin_screen_id_t screen)
{
    record_touch_time();
    if (screen == LEXIN_SCREEN_PET) {
        s_pet_taps++;
    }
}

void lexin_interaction_get_snapshot(lexin_interaction_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    ensure_initialized();

    int64_t now = now_us();
    int live_idle_min = elapsed_min(s_last_touch_us, now);
    int idle_min = live_idle_min > 0 ? live_idle_min : s_idle_before_last_touch_min;
    int focus_min = elapsed_min(s_focus_start_us, now);
    int break_min = idle_min >= LEXIN_BREAK_READY_MIN ? idle_min : 0;
    int focus_rounds = focus_min / LEXIN_FOCUS_ROUND_MIN;
    const char *study_state = "FOCUS";
    if (idle_min >= LEXIN_IDLE_LONG_MIN) {
        study_state = "IDLE_LONG";
    } else if (focus_min >= LEXIN_FOCUS_ROUND_MIN) {
        study_state = "BREAK_DUE";
    } else if (break_min >= LEXIN_BREAK_READY_MIN) {
        study_state = "READY_FOCUS";
    }

    snapshot->weather_taps = s_weather_taps;
    snapshot->calendar_taps = s_calendar_taps;
    snapshot->ai_taps = s_ai_taps;
    snapshot->pet_taps = s_pet_taps;
    snapshot->total_taps = s_total_taps;
    snapshot->idle_min = idle_min;
    snapshot->focus_min = focus_min;
    snapshot->break_min = break_min;
    snapshot->focus_rounds = focus_rounds;
    snapshot->study_state = study_state;
}

void lexin_interaction_build_context(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    lexin_interaction_snapshot_t snapshot;
    lexin_interaction_get_snapshot(&snapshot);
    snprintf(out, out_size,
             "TOUCH_WEATHER: %lu\nTOUCH_CALENDAR: %lu\nTOUCH_AI: %lu\n"
             "TOUCH_PET: %lu\nTOUCH_TOTAL: %lu\nIDLE_MIN: %d\n"
             "FOCUS_MIN: %d\nBREAK_MIN: %d\nFOCUS_ROUNDS: %d\nSTUDY_STATE: %s",
             (unsigned long)snapshot.weather_taps,
             (unsigned long)snapshot.calendar_taps,
             (unsigned long)snapshot.ai_taps,
             (unsigned long)snapshot.pet_taps,
             (unsigned long)snapshot.total_taps,
             snapshot.idle_min,
             snapshot.focus_min,
             snapshot.break_min,
             snapshot.focus_rounds,
             snapshot.study_state);
}

void lexin_interaction_status_text(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    lexin_interaction_snapshot_t snapshot;
    lexin_interaction_get_snapshot(&snapshot);
    snprintf(out, out_size, "专注%d分", snapshot.focus_min);
}

/* ------------------------------------------------------------------ */
/*  Per-user NVS persistence                                           */
/* ------------------------------------------------------------------ */

static void int_nvs_key(const char *suffix, char *key, size_t key_size)
{
    if (s_user_id[0]) {
        snprintf(key, key_size, "%s_%s", s_user_id, suffix);
    } else {
        snprintf(key, key_size, "__%s", suffix);
    }
}

static void int_nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(LEXIN_INT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[LEXIN_INT_NVS_MAX_KEY];
    int_nvs_key("wt", key, sizeof(key)); nvs_set_u32(h, key, s_weather_taps);
    int_nvs_key("ct", key, sizeof(key)); nvs_set_u32(h, key, s_calendar_taps);
    int_nvs_key("at", key, sizeof(key)); nvs_set_u32(h, key, s_ai_taps);
    int_nvs_key("pt", key, sizeof(key)); nvs_set_u32(h, key, s_pet_taps);
    int_nvs_key("tt", key, sizeof(key)); nvs_set_u32(h, key, s_total_taps);
    nvs_commit(h);
    nvs_close(h);
}

static void int_nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(LEXIN_INT_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char key[LEXIN_INT_NVS_MAX_KEY];
    uint32_t v = 0;
    int_nvs_key("wt", key, sizeof(key)); if (nvs_get_u32(h, key, &v) == ESP_OK) s_weather_taps = v;
    int_nvs_key("ct", key, sizeof(key)); if (nvs_get_u32(h, key, &v) == ESP_OK) s_calendar_taps = v;
    int_nvs_key("at", key, sizeof(key)); if (nvs_get_u32(h, key, &v) == ESP_OK) s_ai_taps = v;
    int_nvs_key("pt", key, sizeof(key)); if (nvs_get_u32(h, key, &v) == ESP_OK) s_pet_taps = v;
    int_nvs_key("tt", key, sizeof(key)); if (nvs_get_u32(h, key, &v) == ESP_OK) s_total_taps = v;
    nvs_close(h);
}

void lexin_interaction_set_user(const char *user_id)
{
    /* Save current user's data before switching */
    if (s_initialized && s_user_id[0]) int_nvs_save();
    snprintf(s_user_id, sizeof(s_user_id), "%s", user_id ? user_id : "");
    int64_t now = now_us();
    s_last_touch_us = now;
    s_focus_start_us = now;
    s_idle_before_last_touch_min = 0;
    s_weather_taps = 0;
    s_calendar_taps = 0;
    s_ai_taps = 0;
    s_pet_taps = 0;
    s_total_taps = 0;
    s_last_save_ms = 0;
    int_nvs_load();
    s_initialized = true;
}
