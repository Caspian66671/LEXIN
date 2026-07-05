#pragma once

#include "lexin_actions.h"
#include "lexin_face_auth.h"
#include "lexin_voice.h"
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
void lexin_screen_update_voice_context(const lexin_voice_snapshot_t *snapshot);
void lexin_screen_update_face_auth(const lexin_face_auth_snapshot_t *snapshot);
void lexin_screen_set_capture_status(const char *status);
bool lexin_screen_is_emotion_live(void);
void lexin_screen_show_emotion_report(const char *text, bool monthly);
void lexin_request_board_capture(void);
void lexin_request_emotion_report(bool monthly);

/* Daily plan module. The plan page is opened from the calendar. Plan data
 * is fetched from / posted to the proxy by lexin_main.c (see lexin_plan_*
 * in lexin_triggers.h); these functions push the results into the UI. */
void lexin_screen_show_plan(void);                     /* open today's plan */
void lexin_screen_update_plan(const char *plan_text);   /* today plan -> page */
void lexin_screen_update_plan_month(const char *month_text); /* calendar fill */
void lexin_screen_update_plan_day(const char *plan_text);    /* read-only day */
void lexin_screen_set_plan_recording(bool recording);   /* voice capture state */
