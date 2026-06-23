#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void lexin_edge_advisor_init(void);
bool lexin_edge_advisor_infer_text(const char *context_text, char *out_text, size_t out_size);

#ifdef __cplusplus
}
#endif
