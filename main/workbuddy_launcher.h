#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "workbuddy_actions.h"

typedef enum {
    WORKBUDDY_SCREEN_LAUNCHER = 0,
    WORKBUDDY_SCREEN_PET,
    WORKBUDDY_SCREEN_WEATHER,
    WORKBUDDY_SCREEN_CALENDAR,
    WORKBUDDY_SCREEN_EMOTION,
    WORKBUDDY_SCREEN_SUGGESTION,
} workbuddy_screen_id_t;

typedef enum {
    WORKBUDDY_TOUCH_NONE = 0,
    WORKBUDDY_TOUCH_OPEN_APP,
    WORKBUDDY_TOUCH_BACK,
} workbuddy_touch_result_t;

typedef struct {
    workbuddy_touch_result_t result;
    workbuddy_screen_id_t screen;
    bool has_action;
    workbuddy_action_id_t action_id;
} workbuddy_touch_event_t;

void workbuddy_launcher_init(void);
workbuddy_screen_id_t workbuddy_launcher_current_screen(void);
void workbuddy_launcher_show_launcher(void);
void workbuddy_launcher_show_screen(workbuddy_screen_id_t screen);
workbuddy_touch_event_t workbuddy_launcher_handle_touch(uint16_t x, uint16_t y);
