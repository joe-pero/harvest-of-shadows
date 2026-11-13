#pragma once

#include <Arduino.h>

// ===================== Protocol types =====================
enum Zone : uint8_t { ZNONE = 0, ZFAR = 1, ZMID = 2, ZCLOSE = 3, ZCOUNT = 4 };

static inline const char *zname(Zone z) {
  switch (z) {
    case ZNONE:
      return "NONE";
    case ZFAR:
      return "FAR";
    case ZMID:
      return "MID";
    case ZCLOSE:
      return "CLOSE";
    default:
      return "?";
  }
}

// Avoid clash with ESP-IDF's STATUS typedef; use MSG_STATUS instead
enum MsgType : uint8_t { PING = 1, HB = 2, START = 3, END_ = 4, ACK = 5, EVENT = 6, MSG_STATUS = 7 };

// Fixed-size message with optional payload for EVENT/STATUS.
// First 12 bytes are the header; payload fields may be zero for simple types.
struct __attribute__((packed)) Msg {
  uint8_t magic;
  uint8_t version;
  uint8_t type;       // MsgType
  uint8_t zone;       // Zone
  uint32_t session_id;
  uint32_t request_id;
  uint32_t payload_a;  // e.g., track_id or state
  uint32_t payload_b;  // e.g., elapsed_ms or reserved
  uint16_t crc16;      // CCITT over all prior bytes
};
static_assert(sizeof(Msg) == 22, "Msg must be 22 bytes");

static const uint8_t MSG_MAGIC = 0xA5;
static const uint8_t MSG_VER = 1;

