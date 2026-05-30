#include "workbuddy_actions.h"

static const workbuddy_action_t actions[] = {
    [WORKBUDDY_ACTION_WEATHER] = {
        .id = WORKBUDDY_ACTION_WEATHER,
        .name = "weather",
        .title_json = "\\u67e5\\u8be2\\u5929\\u6c14",
        .path = "/weather",
    },
    [WORKBUDDY_ACTION_TIME] = {
        .id = WORKBUDDY_ACTION_TIME,
        .name = "time",
        .title_json = "\\u67e5\\u8be2\\u65f6\\u95f4",
        .path = "/time",
    },
};

const workbuddy_action_t *workbuddy_get_action(workbuddy_action_id_t id)
{
    if (id < 0 || id >= WORKBUDDY_ACTION_COUNT) {
        return 0;
    }
    return &actions[id];
}
