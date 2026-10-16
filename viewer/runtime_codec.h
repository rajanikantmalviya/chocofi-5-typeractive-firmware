#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool runtime_uvar(const uint8_t *data, size_t size, size_t *offset,
                                uint32_t *value) {
    *value = 0;
    for (uint32_t shift = 0; shift < 32; shift += 7) {
        if (*offset >= size) {
            return false;
        }
        uint8_t byte = data[(*offset)++];
        if (shift == 28 && byte > 15) {
            return false;
        }
        *value |= (uint32_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            return true;
        }
    }
    return false;
}

/* An opaque/malformed body may hide a reference: conservatively guard deletion. */
static inline bool runtime_body_references(const uint8_t *data, size_t size,
                                          uint32_t behavior, uint32_t slot) {
    if (!size) {
        return false;
    }
    if (data[0] != 1) {
        return true;
    }
    size_t offset = 1;
    while (offset < size) {
        uint8_t opcode = data[offset++];
        uint32_t id, p1, p2;
        if (!runtime_uvar(data, size, &offset, &id)) {
            return true;
        }
        if (opcode == 4) {
            continue;
        }
        if (opcode == 5) {
            if (id > size - offset) {
                return true;
            }
            offset += id;
            continue;
        }
        if (opcode < 1 || opcode > 3 ||
            !runtime_uvar(data, size, &offset, &p1) ||
            !runtime_uvar(data, size, &offset, &p2)) {
            return true;
        }
        if (id == behavior && p1 == slot) {
            return true;
        }
    }
    return false;
}
