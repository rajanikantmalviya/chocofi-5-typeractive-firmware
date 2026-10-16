#include <assert.h>
#include <stdint.h>
#include "runtime_codec.h"

int main(void) {
    uint32_t value;
    size_t offset = 0;
    const uint8_t max[] = {255, 255, 255, 255, 15};
    assert(runtime_uvar(max, sizeof(max), &offset, &value));
    assert(value == UINT32_MAX && offset == 5);
    const uint8_t overflow[] = {255, 255, 255, 255, 31};
    offset = 0;
    assert(!runtime_uvar(overflow, sizeof(overflow), &offset, &value));
    const uint8_t truncated[] = {128};
    offset = 0;
    assert(!runtime_uvar(truncated, sizeof(truncated), &offset, &value));
    const uint8_t reference[] = {1, 3, 42, 7, 0};
    assert(runtime_body_references(reference, sizeof(reference), 42, 7));
    assert(!runtime_body_references(reference, sizeof(reference), 42, 6));
    assert(!runtime_body_references(reference, sizeof(reference), 41, 7));
    const uint8_t packed[] = {1, 5, 3, 42, 7, 0, 4, 30};
    assert(!runtime_body_references(packed, sizeof(packed), 42, 7));
    assert(runtime_body_references(packed, sizeof(packed) - 1, 42, 7));
    assert(!runtime_body_references(NULL, 0, 42, 7));
    return 0;
}
