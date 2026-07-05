#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of the voice subsystem state. Surfaced to the UI for the
 * "voice conversation" page so the screen can show "listening / wake /
 * thinking / reply" without the LVGL task having to know the audio
 * pipeline internals. */
typedef enum {
    LEXIN_VOICE_STATE_IDLE = 0,
    LEXIN_VOICE_STATE_LISTENING,   /* mic open, RMS below threshold */
    LEXIN_VOICE_STATE_HEARD,       /* user speech detected */
    LEXIN_VOICE_STATE_UPLOADING,   /* audio sent to proxy, awaiting ASR */
    LEXIN_VOICE_STATE_THINKING,    /* proxy is composing the reply */
    LEXIN_VOICE_STATE_REPLY,       /* text reply delivered */
    LEXIN_VOICE_STATE_ERROR,
} lexin_voice_state_t;

typedef struct {
    lexin_voice_state_t state;
    uint32_t updated_at_ms;     /* esp_timer_get_time() / 1000 */
    char transcript[256];       /* latest ASR text from proxy */
    char reply[512];            /* latest reply text from proxy */
    char status[64];            /* human readable status, e.g. "WAKE" */
    uint16_t rms;               /* current frame RMS, useful for UI meter */
    uint8_t backend;            /* 0 = none, 1 = local-rule, 2 = deepseek */
    uint8_t last_error;
} lexin_voice_snapshot_t;

/* Callback signature used by the voice module to push a fresh snapshot
 * to the rest of the firmware. The same shape as lexin_vision's
 * callback so the UI can be wired in the same way. */
typedef void (*lexin_voice_callback_t)(const lexin_voice_snapshot_t *snapshot,
                                        void *user_data);

/* Start the voice service. Wi-Fi must already be initialised so the
 * module can hand audio chunks to the proxy. The callback is invoked
 * from the voice task every time the snapshot changes. */
esp_err_t lexin_voice_start(lexin_voice_callback_t callback, void *user_data);

/* Copy the most recent snapshot for the UI. */
void lexin_voice_get_snapshot(lexin_voice_snapshot_t *snapshot);

/* Set the logged-in user id. Sent to the proxy as x-lexin-user-id so the
 * conversation session window and history are kept per user. Safe to call
 * from another task; pass NULL or "" to clear. */
void lexin_voice_set_user_id(const char *user_id);

/* Set the capture mode for the *next* uploaded utterances. "chat" (the
 * default) routes to the conversation engine; "plan" tells the proxy to
 * parse the speech into a to-do list for the daily plan module. Sent to
 * the proxy as x-lexin-voice-mode. */
void lexin_voice_set_mode(const char *mode);

/* Manually push a snapshot update (used by the proxy response task
 * to reflect ASR / reply text in the UI). */
void lexin_voice_post_reply(const char *transcript, const char *reply,
                              uint8_t backend, bool is_error);

#ifdef __cplusplus
}
#endif
