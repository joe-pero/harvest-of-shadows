#pragma once

#include <Arduino.h>
#include <stdint.h>

// =============================================================================
// MAC Addresses
// All ESP-NOW peer addresses for the 2026 show network.
// Source of truth: docs/mac_addresses.md
//
// Bench-confirmed 2026-09-06 (read off the chips, not trusted from records):
//   MAC_SCARECROW_A1S  AA:BB:CC:DD:EE:FF  — verified, matches
//   MAC_SKELETON_A1S   AA:BB:CC:DD:EE:FF  — verified, matches
// =============================================================================

static const uint8_t MAC_PARKWAY_1[6]        = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_PARKWAY_2[6]        = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;

static const uint8_t MAC_MAIN_SWITCH[6]      = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_PUMPKIN_MEN[6]      = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_SPOOKY_TREE[6]      = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;  // 2026-10-28 replacement board (old AA:BB:CC:DD:EE:FF killed by reversed 5 V)
// 2026-10-01: was AA:BB:CC:DD:EE:FF = the 2025 Talking Skeleton board, which is
// now Patch Left (props/yard-boombox-1 has no firmware; nothing used this).
static const uint8_t MAC_YARD_BOOMBOX[6]     = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
// 2026-10-01: was AA:BB:CC:DD:EE:FF = the 2025 Singing Pumpkins board, which is
// now Patch Right. This year's Singing Pumpkins ESP32 is AA:BB:CC:DD:EE:FF.
static const uint8_t MAC_SINGING_PUMPKINS[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
// Patch audio nodes (2026-10-01), both read over USB before their first Patch
// flash. RIGHT = the reused 2025 Singing Pumpkins ESP32, LEFT = the reused 2025
// Talking Skeleton ESP32. All zero would mean "unknown": the firmware then only
// logs its MAC instead of verifying it.
static const uint8_t MAC_PATCH_RIGHT[6]      = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_PATCH_LEFT[6]       = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_SCARECROW_A1S[6]    = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_SKELETON_A1S[6]     = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_ZOMBIE_CONTAINER[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
// The Hosyond 4" touch board — historically registered as the zombie-container
// board (MAC_ZOMBIE_CONTAINER), now built out as the "god-mode" touchpad panel
// running NODE_OTA_CONTROLLER (0x0B). Same physical board, same MAC.
// Bench-confirmed off the chip 2026-09-07. See docs/zombie-container.md.
static const uint8_t MAC_OTA_CONTROLLER[6]   = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_STAIN_GLASS[6]      = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_EAVE_COORDINATOR[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;
static const uint8_t MAC_BROADCAST[6]        = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// =============================================================================
// Node Identity
// Used in Msg.sender and Msg.target fields.
// NODE_BROADCAST (0xFF) sends to all registered peers.
// =============================================================================

enum NodeID : uint8_t {
    NODE_MAIN_SWITCH       = 0x01,
    NODE_PUMPKIN_MEN       = 0x02,
    NODE_SPOOKY_TREE       = 0x03,
    NODE_YARD_BOOMBOX      = 0x04,
    NODE_SINGING_PUMPKINS  = 0x05,
    NODE_SCARECROW_A1S     = 0x06,
    NODE_SKELETON_A1S      = 0x07,
    NODE_ZOMBIE_CONTAINER  = 0x08,
    NODE_STAIN_GLASS       = 0x09,
    NODE_LINUX_MASTER      = 0x0A,
    NODE_OTA_CONTROLLER    = 0x0B,  // the Hosyond 4" touch "god-mode" override panel — OTA broadcast + e-stop + Zombie Container DMX trigger + pumpkin jukebox. MAC TBD (read off the board). props/touchpad-ota-controller/, docs/zombie-container.md
    NODE_PARKWAY_1         = 0x0C,  // ESP-01S landscape node — "Harvest of Shadows"; 2x WS2811; sub-pixel IDs 0x70–0x71
    NODE_EAVE_COORDINATOR  = 0x0D,  // front-door eave — calendar authority + 6-PWM/relay/DHT11 lighting node (MAC captured 2026-08-30)
    NODE_PARKWAY_2         = 0x0E,  // ESP-01S landscape node — 2nd physical unit, identical hardware/firmware to NODE_PARKWAY_1
    NODE_PATCH_LEFT        = 0x0F,  // spatial audio — LEFT channel of the pre-mixed theme soundscape (post-Burt phase). ESP32 + YX5300. MAC AA:BB:CC:DD:EE:FF (2026-10-01).
    NODE_PATCH_RIGHT       = 0x10,  // spatial audio — RIGHT channel. Identical firmware to PATCH_LEFT; differs only by SD content + this ID. MAC AA:BB:CC:DD:EE:FF (2026-10-01).
    NODE_BROADCAST         = 0xFF,
};

// =============================================================================
// Show Mode
// Controls intensity/audience-appropriateness.
// =============================================================================

enum ShowMode : uint8_t {
    MODE_FRIENDLY = 0,
    MODE_SCARY    = 1,
    MODE_INTENSE  = 2,
};

// =============================================================================
// Show Theme
// Determines which audio/visual content set is active.
// =============================================================================

enum ShowTheme : uint8_t {
    THEME_CORN             = 0,
    THEME_ROTLD            = 1,
    THEME_MUTANT_PUMPKINS  = 2,
};

// =============================================================================
// Show State
// Current phase of the show sequence, broadcast via CMD_SHOW_STATE heartbeats.
// =============================================================================

enum ShowState : uint8_t {
    SHOW_IDLE     = 0,
    SHOW_PENDING  = 1,
    SHOW_NARRATOR = 2,
    SHOW_ACTIVE   = 3,
    SHOW_OFF      = 4,
};

// =============================================================================
// Show timing
// =============================================================================
// SHOW_ACTIVE_MS — how long the post-narrator "creepy-theme-music phase" runs
// before the leader resets the cycle. Confirmed 2026-09-07: **210 s (3:30)** —
// the ~3:04 zombie-scene track plus a ~26 s buffer Joe added. Replaces the old
// magic 180000 in main_switch_controller and the "interim 240 s" in the a1s doc.
// The Zombie Container DMX scene runs ~184 s from its trigger and finishes
// within this window — see docs/zombie-container-dmx-timeline.md.
static const uint32_t SHOW_ACTIVE_MS = 210000;

// =============================================================================
// Command Vocabulary
// All valid values for Msg.command.
// =============================================================================

enum Command : uint8_t {
    // Show control
    CMD_START_SCENE    = 0x01,
    CMD_SET_THEME      = 0x02,
    CMD_SET_MODE       = 0x03,
    CMD_SET_AMBIENT    = 0x04,
    CMD_RESET_SCENE    = 0x05,
    CMD_TRIGGER        = 0x06,
    CMD_STOP           = 0x07,
    CMD_LOOP           = 0x08,

    // Prop/effect commands
    CMD_DUCK           = 0x10,  // lower audio volume momentarily
    CMD_FLASH          = 0x11,  // fire a flash/strobe
    CMD_HISS           = 0x12,
    CMD_TAUNT          = 0x13,
    CMD_GROWL          = 0x14,

    // Authority commands
    CMD_SHOW_LOCK      = 0x20,  // lock show state — no new triggers accepted
    CMD_SHOW_UNLOCK    = 0x21,
    CMD_FORCE_AMBIENT  = 0x22,
    CMD_FORCE_RESET    = 0x23,
    CMD_FORCE_THEME    = 0x24,
    CMD_FORCE_MODE     = 0x25,
    CMD_MANUAL_LOCK       = 0x26,
    CMD_MANUAL_UNLOCK     = 0x27,
    CMD_SHOW_STATE        = 0x28,  // payload_a = ShowState — heartbeat carrying current state
    CMD_NARRATOR_1_START  = 0x2B,  // main → scarecrow: begin narration
    CMD_NARRATOR_1_DONE   = 0x2C,  // scarecrow → all: narration complete
    CMD_NARRATOR_2_START  = 0x2D,  // main → skeleton: begin narration
    CMD_NARRATOR_2_DONE   = 0x2E,  // skeleton → all: narration complete
    CMD_NARRATOR_ACK      = 0x2F,  // A1S → main: "got it, switching to WiFi"; payload_a = node ID

    // Network / protocol
    CMD_HEARTBEAT      = 0x40,
    CMD_NODE_ONLINE    = 0x41,
    CMD_NODE_OFFLINE   = 0x42,
    CMD_ACK            = 0x43,
    CMD_OTA_MODE       = 0x44,  // broadcast: payload_a = duration minutes (15/30/60); 0 = default 15min
    CMD_CALENDAR_REQUEST = 0x45,  // broadcast: sender requests current calendar-driven state
                                   // from eave_coordinator; reply arrives as a CmdPacket, not a Msg
    CMD_JUKEBOX_SELECT = 0x46,  // touchpad (NODE_OTA_CONTROLLER) -> NODE_SINGING_PUMPKINS jukebox
                                 // override: payload_a = song id, payload_b = 0 now | 1 next.
                                 // Broadcast (no per-node addressing on this mesh); only the
                                 // pumpkins node acts on it. docs/singing-pumpkins.md
    CMD_PROP_MOTION    = 0x47,  // a prop's own sensor saw movement (2026-09-24). Broadcast,
                                 // informational: payload_a = NodeID of the prop, payload_b =
                                 // 1 (detected). Sent by Spooky Tree's PIR. The Scarecrow reacts
                                 // with an idle-only, interruptible "tree aside" line (aside.cpp,
                                 // 2026-09-24); nothing else acts on it. Replaces
                                 // the tree's old CMD_TRIGGER motion frames (Main Switch retired).
};

// An audio prop's health as reported to HA (Spooky Tree over its own WiFi, 2026-10-26). PLAYING = the MP3 module itself confirmed "playing"
// (echoes can't fake it, SafeYX5300 2026-10-26) — the no-microphone sound check.
enum PropHealth : uint8_t {
    HEALTH_BOOTING = 0, HEALTH_PLAYING = 1, HEALTH_NOT_PLAYING = 2, HEALTH_NO_MODULE = 3, HEALTH_ECHO = 4,
};

// =============================================================================
// Wire Protocol — 22-byte packed Msg
//
// Field layout (22 bytes total):
//   [0]     magic       — must be MSG_MAGIC (0xA5)
//   [1]     version     — must be MSG_VERSION (1)
//   [2]     command     — Command enum
//   [3]     sender      — NodeID of sending node
//   [4]     target      — NodeID of intended recipient; 0xFF = broadcast
//   [5]     flags       — 0 except Burt's CMD_ACK (ACK_FLAG_* below, 2026-09-25)
//   [6–9]   session_id  — show session identifier (uint32_t, little-endian)
//   [10–11] seq         — per-message sequence counter (uint16_t)
//   [12–15] payload_a   — primary argument (theme, mode, track ID, etc.)
//   [16–19] payload_b   — secondary argument or 0
//   [20–21] crc16       — CRC16-CCITT over bytes [0..19]
//
// CRC validation: compute crc16_ccitt(data, 20), compare to msg.crc16.
// Frames failing magic, version, or CRC checks are silently dropped.
// =============================================================================

static const uint8_t MSG_MAGIC   = 0xA5;
// 2026-09-25: Burt's CMD_ACK carries his health in `flags` so the Scarecrow can
// report it (leader/status "burt_sd"). STATUS_VALID distinguishes "SD failed"
// from older firmware that leaves flags = 0.
static const uint8_t ACK_FLAG_SD_OK        = 0x01;
static const uint8_t ACK_FLAG_STATUS_VALID = 0x80;
static const uint8_t MSG_VERSION = 1;
static const uint8_t MSG_BROADCAST_TARGET = 0xFF;

struct __attribute__((packed)) Msg {
    uint8_t  magic;       // 0xA5
    uint8_t  version;     // MSG_VERSION
    uint8_t  command;     // Command enum
    uint8_t  sender;      // NodeID
    uint8_t  target;      // NodeID; 0xFF = broadcast
    uint8_t  flags;       // reserved, 0
    uint32_t session_id;
    uint16_t seq;
    uint32_t payload_a;
    uint32_t payload_b;
    uint16_t crc16;
};
static_assert(sizeof(Msg) == 22, "Msg must be exactly 22 bytes");

// =============================================================================
// CRC16-CCITT (poly 0x1021, init 0xFFFF)
// Computed over the first 20 bytes of Msg (all fields except crc16 itself).
// =============================================================================

static inline uint16_t crc16_ccitt(const uint8_t* data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// Stamp and compute CRC on a Msg before sending.
static inline void msg_seal(Msg& m) {
    m.magic   = MSG_MAGIC;
    m.version = MSG_VERSION;
    m.crc16   = crc16_ccitt(reinterpret_cast<const uint8_t*>(&m), 20);
}

// Validate a received Msg. Returns true if magic, version, and CRC all match.
static inline bool msg_valid(const Msg& m) {
    if (m.magic != MSG_MAGIC || m.version != MSG_VERSION) return false;
    return crc16_ccitt(reinterpret_cast<const uint8_t*>(&m), 20) == m.crc16;
}

// =============================================================================
// CmdPacket wire format — area-lights coordinator (6-byte)
// Sent by a coordinator to individual area-lights nodes or sub-pixel addresses.
// Separate from the 22-byte Msg used by the Main Switch Controller.
//
// targetID routing (per receiving node):
//   THIS_NODE / BROADCAST_ID (0xFF) → all pixels on that node
//   Sub-pixel IDs (node-specific, e.g. 0x70–0x72 for parkway) → single pixel
//
// effectID 0xFF is reserved / ignore sentinel.
// FX_STROBE + param1=0: latching strobe.
// FX_STROBE + param1=1: 2-second burst then auto-restore prior pixel state.
// =============================================================================

struct __attribute__((packed)) CmdPacket {
    uint8_t targetID;    // node address or sub-pixel address (node-specific)
    uint8_t effectID;    // effect to apply; 0xFF = ignored
    uint8_t brightness;
    uint8_t speed;
    uint8_t param1;      // effect-specific; FX_STROBE: 0=latch, 1=2s burst+restore
    uint8_t otaDuration; // reserved for CMD_OTA_MODE; set 0 for normal effect packets
};
static_assert(sizeof(CmdPacket) == 6, "CmdPacket must be exactly 6 bytes");

// =============================================================================
// NodeID → MAC lookup
// Returns a pointer to the 6-byte MAC array for the given NodeID,
// or MAC_BROADCAST if the ID is not found.
// =============================================================================

static inline const uint8_t* nodeIdToMac(NodeID id) {
    switch (id) {
        case NODE_MAIN_SWITCH:      return MAC_MAIN_SWITCH;
        case NODE_PUMPKIN_MEN:      return MAC_PUMPKIN_MEN;
        case NODE_SPOOKY_TREE:      return MAC_SPOOKY_TREE;
        case NODE_YARD_BOOMBOX:     return MAC_YARD_BOOMBOX;
        case NODE_SINGING_PUMPKINS: return MAC_SINGING_PUMPKINS;
        case NODE_SCARECROW_A1S:    return MAC_SCARECROW_A1S;
        case NODE_SKELETON_A1S:     return MAC_SKELETON_A1S;
        case NODE_ZOMBIE_CONTAINER: return MAC_ZOMBIE_CONTAINER;
        case NODE_OTA_CONTROLLER:   return MAC_OTA_CONTROLLER;
        case NODE_STAIN_GLASS:      return MAC_STAIN_GLASS;
        case NODE_PARKWAY_1:        return MAC_PARKWAY_1;
        case NODE_PARKWAY_2:        return MAC_PARKWAY_2;
        case NODE_EAVE_COORDINATOR: return MAC_EAVE_COORDINATOR;
        case NODE_PATCH_RIGHT:      return MAC_PATCH_RIGHT;
        case NODE_PATCH_LEFT:       return MAC_PATCH_LEFT;
        default:                    return MAC_BROADCAST;
    }
}
