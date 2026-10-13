#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

static inline bool mouse_speed_percent_valid(uint32_t percent) {
    return percent > 0 && percent <= INT32_MAX;
}

static inline int16_t mouse_speed_scale(int32_t value, uint32_t percent, int16_t *remainder) {
    int64_t total = (int64_t)value * percent + *remainder;
    int64_t scaled = total / 100;
    if (scaled > INT16_MAX || scaled < INT16_MIN) {
        *remainder = 0;
        return scaled > INT16_MAX ? INT16_MAX : INT16_MIN;
    }
    *remainder = (int16_t)(total - scaled * 100);
    return (int16_t)scaled;
}
