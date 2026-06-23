#include "lexin_interaction.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#define LEXIN_FOCUS_ROUND_MIN 25
#define LEXIN_BREAK_READY_MIN 5
#define LEXIN_IDLE_LONG_MIN 30

static bool s_initialized;
static int64_t s_last_touch_us;
static int64_t s_focus_start_us;
static int s_idle_before_last_touch_min;
static uint32_t s_weather_taps;
static uint32_t s_calendar_taps;
static uint32_t s_ai_taps;
static uint32_t s_pet_taps;
static uint32_t s_total_taps;

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
