#include "lexin_actions.h"

static const lexin_action_t actions[] = {
    [LEXIN_ACTION_WEATHER] = {
        .id = LEXIN_ACTION_WEATHER,
        .name = "weather",
        .title_json = "\\u67e5\\u8be2\\u5929\\u6c14",
        .path = "/weather",
    },
    [LEXIN_ACTION_TIME] = {
        .id = LEXIN_ACTION_TIME,
        .name = "time",
        .title_json = "\\u67e5\\u8be2\\u65f6\\u95f4",
        .path = "/time",
    },
    [LEXIN_ACTION_AI_INSIGHT] = {
        .id = LEXIN_ACTION_AI_INSIGHT,
        .name = "ai_insight",
        .title_json = "AI\\u6d1e\\u5bdf",
        .path = "/edge-context",
    },
};

const lexin_action_t *lexin_get_action(lexin_action_id_t id)
{
    if (id < 0 || id >= LEXIN_ACTION_COUNT) {
        return 0;
    }
    return &actions[id];
}
