#include <assert.h>
#include <stdint.h>
#include "mouse_speed_math.h"

int main(void) {
    int16_t remainder = 0;
    assert(mouse_speed_percent_valid(100));
    assert(mouse_speed_percent_valid(150));
    assert(mouse_speed_percent_valid(200));
    assert(mouse_speed_percent_valid(300));
    assert(!mouse_speed_percent_valid(0));
    assert(!mouse_speed_percent_valid(UINT32_MAX));

    assert(mouse_speed_scale(10, 100, &remainder) == 10);
    assert(mouse_speed_scale(-10, 100, &remainder) == -10);
    assert(mouse_speed_scale(10, 150, &remainder) == 15);
    assert(mouse_speed_scale(10, 200, &remainder) == 20);
    assert(mouse_speed_scale(10, 300, &remainder) == 30);

    assert(mouse_speed_scale(1, 50, &remainder) == 0);
    assert(mouse_speed_scale(1, 50, &remainder) == 1);
    assert(remainder == 0);
    assert(mouse_speed_scale(-1, 150, &remainder) == -1);
    assert(mouse_speed_scale(-1, 150, &remainder) == -2);
    assert(remainder == 0);
    assert(mouse_speed_scale(1, 150, &remainder) == 1);
    assert(mouse_speed_scale(-1, 150, &remainder) == -1);
    assert(remainder == 0);

    int total = 0;
    for (int i = 0; i < 100; i++) {
        total += mouse_speed_scale(1, 33, &remainder);
    }
    assert(total == 33);
    assert(remainder == 0);

    assert(mouse_speed_scale(INT32_MAX, INT32_MAX, &remainder) == INT16_MAX);
    assert(remainder == 0);
    assert(mouse_speed_scale(INT32_MIN, INT32_MAX, &remainder) == INT16_MIN);
    assert(remainder == 0);
    assert(mouse_speed_scale(INT16_MAX, 100, &remainder) == INT16_MAX);
    assert(mouse_speed_scale(INT16_MIN, 100, &remainder) == INT16_MIN);
    return 0;
}
