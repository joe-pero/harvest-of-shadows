#pragma once

#include <stdint.h>

// Registers an ESP-NOW receive callback compatible with both
// IDF 4.x and IDF 5.x. The provided function receives a normalized
// (mac, data, len) view regardless of underlying SDK signature.
void espnow_register_recv_bridge(void (*common)(const uint8_t *mac, const uint8_t *data, int len));

