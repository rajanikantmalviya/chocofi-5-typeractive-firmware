#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int viewer_request(uint8_t command, bool ble);
void viewer_changed(void);
bool viewer_ble_streaming(void);
bool viewer_ble_ready(void);
int viewer_ble_send(const char *data, size_t size);
void viewer_ble_flush(void);
