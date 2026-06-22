#include "esp_timer.h"

#include "echomate_app.h"
#include "echomate_config.h"

static int64_t s_last_interaction_us;

const char *echomate_emotion_to_string(echomate_emotion_t emotion)
{
    switch (emotion) {
    case ECHOMATE_EMOTION_HAPPY:
        return "HAPPY";
    case ECHOMATE_EMOTION_CALM:
        return "CALM";
    case ECHOMATE_EMOTION_LONELY:
        return "LONELY";
    case ECHOMATE_EMOTION_ALERT:
        return "ALERT";
    case ECHOMATE_EMOTION_SLEEPY:
        return "SLEEPY";
    default:
        return "UNKNOWN";
    }
}

esp_err_t emotion_engine_init(void)
{
    s_last_interaction_us = esp_timer_get_time();
    return ESP_OK;
}

esp_err_t emotion_engine_update(const vision_result_msg_t *vision,
                                const audio_feature_msg_t *audio,
                                emotion_state_msg_t *state)
{
    if (!vision || !audio || !state) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t now = esp_timer_get_time();
    const bool face = vision->face_detected;
    const bool smile = face && vision->expression == ECHOMATE_EXPRESSION_HAPPY;
    const bool loud = audio->amplitude >= CONFIG_ECHOMATE_LOUD_AMPLITUDE;
    const bool wake = audio->wake_word_detected || audio->command != ECHOMATE_COMMAND_NONE;
    const bool interacted = face || wake || audio->amplitude > 3000;

    if (interacted) {
        s_last_interaction_us = now;
    }

    const int64_t idle_ms = (now - s_last_interaction_us) / 1000;

    state->timestamp_us = now;
    state->reason_flags = ECHOMATE_REASON_NONE;

    if (loud) {
        state->emotion = ECHOMATE_EMOTION_ALERT;
        state->confidence = 92;
        state->reason_flags = ECHOMATE_REASON_LOUD;
    } else if (smile) {
        state->emotion = ECHOMATE_EMOTION_HAPPY;
        state->confidence = vision->confidence;
        state->reason_flags = ECHOMATE_REASON_FACE | ECHOMATE_REASON_SMILE;
    } else if (idle_ms >= CONFIG_ECHOMATE_SLEEPY_TIMEOUT_MS) {
        state->emotion = ECHOMATE_EMOTION_SLEEPY;
        state->confidence = 80;
        state->reason_flags = ECHOMATE_REASON_IDLE;
    } else if (idle_ms >= CONFIG_ECHOMATE_LONELY_TIMEOUT_MS) {
        state->emotion = ECHOMATE_EMOTION_LONELY;
        state->confidence = 76;
        state->reason_flags = ECHOMATE_REASON_IDLE;
    } else {
        state->emotion = ECHOMATE_EMOTION_CALM;
        state->confidence = 68;
        if (face) {
            state->reason_flags |= ECHOMATE_REASON_FACE;
        }
        if (wake) {
            state->reason_flags |= ECHOMATE_REASON_WAKE;
        }
    }

    return ESP_OK;
}
