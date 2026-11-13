#pragma once
// =============================================================================
// "The media/WiZ player" — STUB. DEVICE UNCONFIRMED — ASK JOE.
// =============================================================================
// Joe's phrase from the handoff: "Control the media/WiZ player". The exact
// device is NOT known from available context. Candidates and what each implies:
//
//   1. MediaWiz 1080 (MedeaWiz Sprite) — the Zombie Container's zombie-video
//      player, mentioned in an earlier message. Control = a DRY-CONTACT RELAY
//      pulse into its trigger input  →  use RELAY_AUX_PIN, same pattern as the
//      PicoDMX trigger. Possibly also a 4800-baud serial "play track N" byte.
//
//   2. Philips WiZ smart lighting (WiFi bulbs / plugs / strips) — "WiZ" is a
//      brand. Control = UDP JSON on port 38899 over the LAN (needs WiFi STA
//      association, not just the ch-11 ESP-NOW lock; or via the Wyze/HA stack).
//      RELAY_AUX_PIN is then unused for this.
//
//   3. A BrightSign / other media player — serial (see docs/singing-pumpkins.md).
//
// Until Joe confirms: this module does nothing but log. Do NOT wire RELAY_AUX or
// bring up WiFi STA for it on a guess.
// =============================================================================

#include <Arduino.h>

void media_setup();
void media_play();        // "start / trigger" — stub
void media_stop();        // stub
