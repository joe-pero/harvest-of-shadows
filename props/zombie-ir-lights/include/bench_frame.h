#pragma once
// =============================================================================
// Bench-only frame for testing zombie-ir-lights WITHOUT touching the live show
// =============================================================================
// The Zombie Container is DEPLOYED. A bench ESP32 that broadcast a real
// CMD_SEGMENT_START or CMD_SET_THEME=ROTLD Msg on ch 11 could fire the real
// PicoDMX show (zombie-relay-esp01s + touchpad both act on those). So the bench
// sender uses this separate frame, which every fleet receiver drops:
//   - length 12, not 22: every Msg receiver checks sizeof(Msg) (audited
//     2026-09-30: relay, touchpad, scarecrow, burt, tree, pumpkin-men,
//     singing-pumpkins, patch, crows, shoulder-crows; plus the Halloween-Dev
//     eave_coordinator, zombie_container, parkway, stain-glass nodes).
//   - byte[0] = 0xB7, not MSG_MAGIC 0xA5, so msg_valid() fails even without
//     a length check.
//   - byte[1] = 0xFF, the CmdPacket effectID "ignored" sentinel, so a 6-byte
//     CmdPacket parser (area-lights/parkway style) that skips the length
//     check still ignores it. area-lights (len >= 6, no effectID check) drops
//     it on targetID: 0xB7 is not 0x01-0x06 or 0xFF.
// Only zombie-ir-lights accepts it.
// =============================================================================
#include <stdint.h>
#include "ProjectConstants.h"   // crc16_ccitt

static const uint8_t BENCH_MAGIC  = 0xB7;
static const uint8_t BENCH_IGNORE = 0xFF;

enum BenchCmd : uint8_t {
    BENCH_START = 1,   // arg = start offset ms (0 = from the trigger)
    BENCH_END   = 2,   // like CMD_SEGMENT_END
    BENCH_OFF   = 3,   // like CMD_SHOW_STATE=SHOW_OFF
};

struct __attribute__((packed)) BenchFrame {
    uint8_t  magic;    // BENCH_MAGIC
    uint8_t  ignore;   // BENCH_IGNORE
    char     tag[2];   // 'I','R'
    uint8_t  cmd;      // BenchCmd
    uint8_t  pad;
    uint32_t arg;
    uint16_t crc16;    // crc16_ccitt over bytes [0..9]
};
static_assert(sizeof(BenchFrame) == 12, "BenchFrame must be 12 bytes");

static inline void bench_seal(BenchFrame& f) {
    f.magic = BENCH_MAGIC; f.ignore = BENCH_IGNORE; f.tag[0] = 'I'; f.tag[1] = 'R';
    f.crc16 = crc16_ccitt(reinterpret_cast<const uint8_t*>(&f), 10);
}
static inline bool bench_valid(const BenchFrame& f) {
    return f.magic == BENCH_MAGIC && f.ignore == BENCH_IGNORE && f.tag[0] == 'I' &&
           f.tag[1] == 'R' && crc16_ccitt(reinterpret_cast<const uint8_t*>(&f), 10) == f.crc16;
}
