#pragma once

#include "lexin_actions.h"
#include "lexin_vision.h"

typedef enum {
    LEXIN_FACE_UNKNOWN = 0,
    LEXIN_FACE_NOT_DETECTED,
    LEXIN_FACE_DETECTED,
} lexin_face_state_t;

typedef enum {
    LEXIN_EMOTION_UNKNOWN = 0,
    LEXIN_EMOTION_NEUTRAL,
    LEXIN_EMOTION_HAPPY,
    LEXIN_EMOTION_TIRED,
    LEXIN_EMOTION_FOCUSED,
} lexin_emotion_state_t;

void lexin_start_screen_ui(void);
void lexin_screen_show_idle(void);
void lexin_screen_show_querying(lexin_action_id_t action_id);
void lexin_screen_show_result(lexin_action_id_t action_id);
void lexin_screen_show_result_text(lexin_action_id_t action_id, const char *text);
void lexin_screen_show_error(lexin_action_id_t action_id);
void lexin_screen_update_ai_context(lexin_face_state_t face, lexin_emotion_state_t emotion);
void lexin_screen_update_vision_context(const lexin_vision_snapshot_t *snapshot);
