#pragma once

#include <stdint.h>

#include "workbuddy_actions.h"

void workbuddy_enqueue_trigger(const char *source, uint32_t source_id,
                               workbuddy_action_id_t action_id);
