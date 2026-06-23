#include "lexin_launcher.h"

#include <stddef.h>

#define BACK_HIT_W 1024
#define BACK_HIT_H 180

typedef struct {
    lexin_screen_id_t screen;
    lexin_action_id_t action_id;
    bool has_action;
    int x;
    int y;
    int w;
    int h;
} launcher_app_t;

static lexin_screen_id_t s_current_screen = LEXIN_SCREEN_LAUNCHER;

static const launcher_app_t s_apps[] = {
    {.screen = LEXIN_SCREEN_PET, .action_id = LEXIN_ACTION_TIME, .has_action = false, .x = 66, .y = 132, .w = 348, .h = 344},
    {.screen = LEXIN_SCREEN_WEATHER, .action_id = LEXIN_ACTION_WEATHER, .has_action = true, .x = 470, .y = 160, .w = 112, .h = 250},
    {.screen = LEXIN_SCREEN_CALENDAR, .action_id = LEXIN_ACTION_TIME, .has_action = true, .x = 586, .y = 160, .w = 112, .h = 250},
    {.screen = LEXIN_SCREEN_EMOTION, .action_id = LEXIN_ACTION_TIME, .has_action = false, .x = 702, .y = 160, .w = 112, .h = 250},
    {.screen = LEXIN_SCREEN_SUGGESTION, .action_id = LEXIN_ACTION_AI_INSIGHT, .has_action = true, .x = 818, .y = 160, .w = 112, .h = 250},
};

static bool point_in_rect(uint16_t x, uint16_t y, const launcher_app_t *app)
{
    return x >= app->x && x < app->x + app->w && y >= app->y && y < app->y + app->h;
}

void lexin_launcher_init(void)
{
    s_current_screen = LEXIN_SCREEN_LAUNCHER;
}

lexin_screen_id_t lexin_launcher_current_screen(void)
{
    return s_current_screen;
}

void lexin_launcher_show_launcher(void)
{
    s_current_screen = LEXIN_SCREEN_LAUNCHER;
}

void lexin_launcher_show_screen(lexin_screen_id_t screen)
{
    s_current_screen = screen;
}

lexin_touch_event_t lexin_launcher_handle_touch(uint16_t x, uint16_t y)
{
    x = 1023 - x;

    lexin_touch_event_t event = {
        .result = LEXIN_TOUCH_NONE,
        .screen = s_current_screen,
        .has_action = false,
    };

    if (s_current_screen != LEXIN_SCREEN_LAUNCHER) {
        (void)y;
        s_current_screen = LEXIN_SCREEN_LAUNCHER;
        event.result = LEXIN_TOUCH_BACK;
        event.screen = s_current_screen;
        return event;
    }

    for (size_t i = 0; i < sizeof(s_apps) / sizeof(s_apps[0]); i++) {
        const launcher_app_t *app = &s_apps[i];
        if (point_in_rect(x, y, app)) {
            s_current_screen = app->screen;
            event.result = LEXIN_TOUCH_OPEN_APP;
            event.screen = app->screen;
            event.has_action = app->has_action;
            event.action_id = app->action_id;
            return event;
        }
    }

    return event;
}
