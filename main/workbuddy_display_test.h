#pragma once

#include "workbuddy_actions.h"
#include "workbuddy_vision.h"

typedef enum {
    WORKBUDDY_FACE_UNKNOWN = 0,
    WORKBUDDY_FACE_NOT_DETECTED,
    WORKBUDDY_FACE_DETECTED,
} workbuddy_face_state_t;

typedef enum {
    WORKBUDDY_EMOTION_UNKNOWN = 0,
    WORKBUDDY_EMOTION_NEUTRAL,
    WORKBUDDY_EMOTION_HAPPY,
    WORKBUDDY_EMOTION_TIRED,
    WORKBUDDY_EMOTION_FOCUSED,
} workbuddy_emotion_state_t;

void workbuddy_start_screen_ui(void);
void workbuddy_screen_show_idle(void);
void workbuddy_screen_show_querying(workbuddy_action_id_t action_id);
void workbuddy_screen_show_result(workbuddy_action_id_t action_id);
void workbuddy_screen_show_result_text(workbuddy_action_id_t action_id, const char *text);
void workbuddy_screen_show_error(workbuddy_action_id_t action_id);
void workbuddy_screen_update_ai_context(workbuddy_face_state_t face, workbuddy_emotion_state_t emotion);
void workbuddy_screen_update_vision_context(const workbuddy_vision_snapshot_t *snapshot);
