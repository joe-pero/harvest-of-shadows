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

// 2026-09-29: the unused 2025 Msg/MsgType definitions were removed. The crows
// now read the 2026 show Msg from harvest-of-shadows ProjectConstants.h, which
// defines its own Msg/MSG_MAGIC (the two would clash).

