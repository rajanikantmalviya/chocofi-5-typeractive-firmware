#pragma once

#include <stdbool.h>
#include <stdint.h>

void runtime_safety_event_lock(uint32_t position, bool pressed);
void runtime_safety_event_unlock(void);
void runtime_safety_read_lock(void);
bool runtime_safety_edit_allowed(void);
void runtime_safety_configuration_begin(void);
void runtime_safety_configuration_end(void);
