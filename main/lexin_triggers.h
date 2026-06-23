#pragma once

#include <stdint.h>

#include "lexin_actions.h"

void lexin_enqueue_trigger(const char *source, uint32_t source_id,
                               lexin_action_id_t action_id);
