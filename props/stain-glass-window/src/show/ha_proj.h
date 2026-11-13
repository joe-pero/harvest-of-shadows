// Projector power from Home Assistant (2026-10-20, Joe: control this ESP32 from the garage display).
// env:show only. WiFi + MQTT to HA's broker on detect-server (always on), HA MQTT discovery:
//   switch "Window projector" / "Door projector"   command harvest/stainglass/<wp|dp>/set  ON|OFF
//                                                   state   harvest/stainglass/<wp|dp>/want ON|OFF
//   sensor "... power" = what the projector itself answers over RS-232: ON / OFF / COOLING / NO REPLY / FAULT
//                                                   state   harvest/stainglass/<wp|dp>/power
//   availability harvest/stainglass/status online|offline (LWT)
// OFF = Hitachi standby (lamp cools itself) + keep-on stopped; ON = power on + keep-on. HA's show-power OFF
// waits for both sensors to read OFF before it cuts "Long straight power" (tools/ha_show_power.py).
// Every reboot (= every Long straight power-on) is still projectors ON, as before.
#pragma once

void haProjBegin();
void haProjTick();
void haProjMon();

// provided by bench/main.cpp (the projector keep-on state lives there); i = 0 window, 1 door
bool        sgProjWant(int i);
const char* sgProjState(int i);
void        sgProjSet(int i, bool on, const char* who);
