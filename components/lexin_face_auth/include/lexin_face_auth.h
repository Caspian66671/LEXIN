#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LEXIN_FACE_AUTH_IDLE = 0,      /* waiting for camera ready */
    LEXIN_FACE_AUTH_SCANNING,      /* no face or face unstable */
    LEXIN_FACE_AUTH_DETECTED,      /* face detected, uploading */
    LEXIN_FACE_AUTH_RECOGNIZED,    /* user matched */
    LEXIN_FACE_AUTH_UNKNOWN,       /* face seen but unknown */
    LEXIN_FACE_AUTH_REGISTERING,   /* user is typing name */
    LEXIN_FACE_AUTH_REGISTERED,    /* new user created */
    LEXIN_FACE_AUTH_ERROR,
} lexin_face_auth_state_t;

typedef struct {
    lexin_face_auth_state_t state;
    bool face_detected;
    bool recognized;
    char user_id[32];
    char user_name[64];
    char status_text[96];
    uint16_t face_x, face_y, face_w, face_h;
    uint8_t confidence;
    uint32_t updated_at_ms;
} lexin_face_auth_snapshot_t;

typedef void (*lexin_face_auth_callback_t)(
    const lexin_face_auth_snapshot_t *snapshot, void *user_data);

/** Start the face auth service. Requires lexin_vision to be running. */
esp_err_t lexin_face_auth_start(lexin_face_auth_callback_t cb, void *user_data);

void lexin_face_auth_get_snapshot(lexin_face_auth_snapshot_t *snapshot);

/** Submit a user name for registration of the currently-seen face.
 *  Returns ESP_OK if the request was queued, ESP_FAIL on error. */
esp_err_t lexin_face_auth_register(const char *user_name);

/** Switch the active user (user picked from a list, e.g. after reboot
 *  when the camera is unavailable). */
esp_err_t lexin_face_auth_login_by_id(const char *user_id, const char *user_name);

/** Return true if someone is logged in. */
bool lexin_face_auth_is_logged_in(void);

/** Lock the current session without deleting saved users. After a manual
 *  lock, recognition resumes only after the current face leaves the frame. */
void lexin_face_auth_lock(void);

/** Get current user info. */
const char *lexin_face_auth_current_user_id(void);
const char *lexin_face_auth_current_user_name(void);

#ifdef __cplusplus
}
#endif
