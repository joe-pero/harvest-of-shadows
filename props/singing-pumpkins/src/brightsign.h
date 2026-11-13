#pragma once
// =============================================================================
// BrightSign XT1143 serial link (docs/singing-pumpkins.md)
// =============================================================================
// The show logic lives ON THE PLAYER'S SD CARD (props/singing-pumpkins/sdcard/
// autorun.brs + show.json + library.json). This ESP32 is the controller: it
// hears the fleet over ESP-NOW and sends one short line per command. The card
// decides what to play.
//
//   ESP32 -> card:  SHOW [FORCE] | END | PLAY <n> [NOW] | SKIP | VOL <0-100> | LIST | STATUS | PING
//   card -> ESP32:  READY | NOW ... | MODE DJ|SHOW ... | ACK ... | BUSY ... | ERR ... | HB ... | PONG
//                   | SONG <n> <label> ... LISTEND <count>   (pick list, for Home Assistant)
//
// Hardware: XT1143 3.5 mm TRS serial (Tip = RX into player, Ring = TX out,
// Sleeve = GND), 115200 8N1, BrightScript roSerialPort 0. Joe's HW-027
// RS232<->TTL (MAX3232) board between it and the ESP32's Serial1.
// Pins below are the proposed wiring — confirm at the bench.
// =============================================================================

#include <Arduino.h>
#include <vector>

static const int BS_TX_PIN = 22;   // ESP32 TX -> HW-027 TTL RX -> 3.5 mm Tip
static const int BS_RX_PIN = 21;   // ESP32 RX <- HW-027 TTL TX <- 3.5 mm Ring

void brightsign_begin();                          // open Serial1, send PING
void brightsign_tick();                           // read + log card replies (call every loop)
void brightsign_show_start();                     // "SHOW"  trick-or-treat show
void brightsign_show_end();                       // "END"   finish early (card plays the close-out)
void brightsign_jukebox(uint16_t songN, bool now); // "PLAY n" / "PLAY n NOW"
void brightsign_send(const char* line);           // raw line (bench commands)
void brightsign_swap_pins();                      // bench: swap TX/RX live (wiring check, no reflash)
void brightsign_print_pins();                     // bench: current TX/RX + bytes received so far
bool brightsign_link_ok();                        // heard the card in the last 75 s (it sends HB every 30 s)
const char* brightsign_card_mode();               // "DJ", "SHOW" or "?" — last MODE/HB the card reported

// Pick list from the card (LIST). Requested at boot, whenever the card reports READY
// (it rebooted: numbers may have changed), and every 60 s until one arrives.
struct CardSong { uint16_t n; String label; };
const std::vector<CardSong>& brightsign_songs();
uint32_t brightsign_songs_version();              // bumps each time a complete list arrives
int      brightsign_now_n();                      // number of what's playing (0 = transition/none)
const String& brightsign_now_line();              // the card's last NOW line, for display
const String& brightsign_last_error();            // the card's last ERR line
