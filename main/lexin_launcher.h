#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lexin_actions.h"

typedef enum {
    LEXIN_SCREEN_LAUNCHER = 0,
    LEXIN_SCREEN_PET,
    LEXIN_SCREEN_WEATHER,
    LEXIN_SCREEN_CALENDAR,
    LEXIN_SCREEN_EMOTION,
    LEXIN_SCREEN_SUGGESTION,
} lexin_screen_id_t;

typedef enum {
    LEXIN_TOUCH_NONE = 0,
    LEXIN_TOUCH_OPEN_APP,
    LEXIN_TOUCH_BACK,
} lexin_touch_result_t;

typedef struct {
    lexin_touch_result_t result;
    lexin_screen_id_t screen;
    bool has_action;
    lexin_action_id_t action_id;
} lexin_touch_event_t;

void lexin_launcher_init(void);
lexin_screen_id_t lexin_launcher_current_screen(void);
void lexin_launcher_show_launcher(void);
void lexin_launcher_show_screen(lexin_screen_id_t screen);
lexin_touch_event_t lexin_launcher_handle_touch(uint16_t x, uint16_t y);
