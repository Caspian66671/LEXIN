#pragma once

#include <stdint.h>

#include "lexin_actions.h"

void lexin_enqueue_trigger(const char *source, uint32_t source_id,
                               lexin_action_id_t action_id);

/* Daily plan networking (implemented in lexin_main.c). Each call spawns a
 * short-lived worker so the UI/touch task never blocks on HTTP. Results
 * are delivered back through the lexin_screen_update_plan* callbacks. */
void lexin_plan_fetch_today(void);
void lexin_plan_fetch_month(void);
void lexin_plan_toggle(int index);
void lexin_plan_delete(int index);
void lexin_plan_toggle_day(int index, int year, int month, int day);
void lexin_plan_delete_day(int index, int year, int month, int day);
void lexin_plan_fetch_day(int year, int month, int day);
