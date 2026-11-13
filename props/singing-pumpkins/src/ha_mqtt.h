#pragma once
// =============================================================================
// Home Assistant control of the Singing Pumpkins (Joe 2026-09-27: HA is the manual
// override / jukebox; the Zombie touchpad path stays but is secondary).
// =============================================================================
// MQTT on the detect-server broker (192.168.1.x, the one HA uses; always on,
// unlike the Workshop's show broker which suspends). HA auto-discovery creates a
// "Singing Pumpkins" device with no YAML:
//
//   select  Play now        pick list from the card (LIST) -> "PLAY n NOW" (ends a running show)
//   button  Start show      "SHOW FORCE"  (run it early / after a misfire; ignores cooldown)
//   button  End show        "END"         (card plays the close-out)
//   button  Skip            "SKIP"
//   button  Projector power "IRPWR"       (ESP32 sends Hitachi POWER over IR; not sent to the card)
//   number  Volume          "VOL n"
//   sensor  Now playing / Mode / Last card error
//   binary_sensor  Player link (card heartbeat seen in the last 75 s)
//
// Raw backdoor: publish any card line to harvest/pumpkins/cmd (e.g. "PLAY 12").
// WiFi joins the ch-11 AP (ESP-NOW rides on the same channel); retried every 10 s
// forever, same as the Scarecrow's 2026-09-25 fix. ESP-NOW keeps working without WiFi.
// =============================================================================

void ha_begin();   // after espnowInit()
void ha_tick();    // every loop
bool ha_connected();
