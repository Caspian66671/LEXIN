#pragma once

typedef enum {
    LEXIN_ACTION_WEATHER = 0,
    LEXIN_ACTION_TIME,
    LEXIN_ACTION_AI_INSIGHT,
    LEXIN_ACTION_COUNT,
} lexin_action_id_t;

typedef struct {
    lexin_action_id_t id;
    const char *name;
    const char *title_json;
    const char *path;
} lexin_action_t;

const lexin_action_t *lexin_get_action(lexin_action_id_t id);
