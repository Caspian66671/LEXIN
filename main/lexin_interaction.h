#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lexin_actions.h"
#include "lexin_launcher.h"

typedef struct {
    uint32_t weather_taps;
    uint32_t calendar_taps;
    uint32_t ai_taps;
    uint32_t pet_taps;
    uint32_t total_taps;
    int idle_min;
    int focus_min;
    int break_min;
    int focus_rounds;
    const char *study_state;
} lexin_interaction_snapshot_t;

void lexin_interaction_init(void);
void lexin_interaction_set_user(const char *user_id);
void lexin_interaction_record_action(lexin_action_id_t action_id);
void lexin_interaction_record_screen(lexin_screen_id_t screen);
void lexin_interaction_get_snapshot(lexin_interaction_snapshot_t *snapshot);
void lexin_interaction_build_context(char *out, size_t out_size);
void lexin_interaction_status_text(char *out, size_t out_size);
