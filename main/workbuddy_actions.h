#pragma once

typedef enum {
    WORKBUDDY_ACTION_WEATHER = 0,
    WORKBUDDY_ACTION_TIME,
    WORKBUDDY_ACTION_COUNT,
} workbuddy_action_id_t;

typedef struct {
    workbuddy_action_id_t id;
    const char *name;
    const char *title_json;
    const char *path;
} workbuddy_action_t;

const workbuddy_action_t *workbuddy_get_action(workbuddy_action_id_t id);
