#include "brightsign.h"
#include "skeletons.h"

// Serial link to the XT1143's autorun.brs. Fire-and-forget commands; the card's
// replies are logged, and its 30 s heartbeat is the liveness signal.

static const uint32_t LINK_TIMEOUT_MS = 75000;

static uint32_t s_lastHeardMs = 0;
static char     s_rx[256];          // CUES lines run to ~190 chars
static size_t   s_rxLen = 0;
static char     s_mode[8] = "?";
static std::vector<CardSong> s_songs, s_pending;
static bool     s_listing = false;
static uint32_t s_songsVersion = 0;
static uint32_t s_lastListReqMs = 0;
static int      s_txPin = BS_TX_PIN, s_rxPin = BS_RX_PIN;
static uint32_t s_rxBytes = 0;   // every byte from the card, even garbage: 0 = nothing on the RX wire
static int      s_nowN = 0;
static String   s_nowLine, s_lastErr;

static void requestList() {
    s_lastListReqMs = millis();
    s_listing = false;
    s_pending.clear();
    brightsign_send("LIST");
}

static void ts() { Serial.printf("[%lu] ", (unsigned long)millis()); }

void brightsign_send(const char* line) {
    Serial1.print(line);
    Serial1.print('\r');
    ts(); Serial.printf("[brightsign] -> %s\n", line);
}

void brightsign_begin() {
    // LIST sends ~7.8 KB back to back (153 lines); hold all of it even if an MQTT
    // reconnect stalls the loop for a few seconds. A short list is still caught (LISTEND count).
    Serial1.setRxBufferSize(10240);
    Serial1.begin(115200, SERIAL_8N1, s_rxPin, s_txPin);
    ts(); Serial.printf("[brightsign] Serial1 115200 TX=%d RX=%d\n", s_txPin, s_rxPin);
    brightsign_send("PING");
    requestList();
}

static void onLine(const char* l) {
    s_lastHeardMs = millis();
    // Pick list: collected quietly (not logged line by line).
    if (strncmp(l, "SONG ", 5) == 0) {
        const char* sp = strchr(l + 5, ' ');
        int n = atoi(l + 5);
        if (n > 0 && sp) { s_pending.push_back({(uint16_t)n, String(sp + 1)}); s_listing = true; }
        return;
    }
    if (strncmp(l, "LISTEND", 7) == 0) {
        int expect = atoi(l + 8);
        if ((int)s_pending.size() == expect && expect > 0) {
            s_songs.swap(s_pending);
            s_songsVersion++;
            ts(); Serial.printf("[brightsign] pick list: %u entries\n", (unsigned)s_songs.size());
        } else {
            ts(); Serial.printf("[brightsign] !! pick list incomplete (%u of %d lines) - will ask again\n",
                                (unsigned)s_pending.size(), expect);
            s_lastListReqMs = millis() - 50000;   // retry in ~10 s
        }
        s_pending.clear();
        s_listing = false;
        return;
    }
    if (strncmp(l, "NOW ", 4) == 0) { s_nowN = atoi(l + 4); s_nowLine = String(l + 4); skel_on_now(s_nowN); }
    if (strncmp(l, "CUES ", 5) == 0) {          // skeleton moments for the clip that just started
        const char* sp = strchr(l + 5, ' ');
        if (sp) skel_on_cues(atoi(l + 5), sp + 1);
    }
    if (strncmp(l, "ERR", 3) == 0)  s_lastErr = String(l);
    if (strncmp(l, "READY", 5) == 0) requestList();   // card rebooted: refresh numbers
    // Track what the card says it is doing: "MODE SHOW ..." / "MODE DJ" / "HB mode=DJ ...".
    const char* m = nullptr;
    if (strncmp(l, "MODE ", 5) == 0) m = l + 5;
    else if (strncmp(l, "HB mode=", 8) == 0) m = l + 8;
    else if (strncmp(l, "READY", 5) == 0) m = "DJ";
    if (m) {
        size_t n = 0;
        while (m[n] && m[n] != ' ' && n < sizeof(s_mode) - 1) { s_mode[n] = m[n]; n++; }
        s_mode[n] = 0;
    }
    ts();
    if (strncmp(l, "ERR", 3) == 0) Serial.printf("[brightsign] !! CARD ERROR: %s\n", l);
    else                           Serial.printf("[brightsign] <- %s\n", l);
}

void brightsign_tick() {
    // No list yet: ask every 60 s. A list that started but never finished (LISTEND lost): ask again after 20 s.
    if ((s_songs.empty() && !s_listing && millis() - s_lastListReqMs > 60000) ||
        (s_listing && millis() - s_lastListReqMs > 20000)) requestList();
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        s_rxBytes++;
        if (c == '\r' || c == '\n') {
            if (s_rxLen) { s_rx[s_rxLen] = 0; onLine(s_rx); s_rxLen = 0; }
        } else if (s_rxLen < sizeof(s_rx) - 1) {
            s_rx[s_rxLen++] = c;
        }
    }
}

void brightsign_print_pins() {
    ts(); Serial.printf("[brightsign] pins TX=%d RX=%d%s, bytes from card so far: %lu\n", s_txPin, s_rxPin,
                        s_txPin == BS_TX_PIN ? " (as built)" : " (SWAPPED, until reboot)", (unsigned long)s_rxBytes);
}

// Bench wiring check: if green/white (GPIO22/21) are the wrong way round, this flips them in
// software so nobody has to rewire. Not saved: a reboot goes back to BS_TX_PIN / BS_RX_PIN.
void brightsign_swap_pins() {
    Serial1.end();
    int t = s_txPin; s_txPin = s_rxPin; s_rxPin = t;
    s_rxBytes = 0;
    s_rxLen = 0;
    Serial1.begin(115200, SERIAL_8N1, s_rxPin, s_txPin);
    brightsign_print_pins();
    brightsign_send("PING");
}

void brightsign_show_start() { brightsign_send("SHOW"); }
void brightsign_show_end()   { brightsign_send("END"); }

void brightsign_jukebox(uint16_t songN, bool now) {
    char b[24];
    snprintf(b, sizeof b, now ? "PLAY %u NOW" : "PLAY %u", (unsigned)songN);
    brightsign_send(b);
}

bool brightsign_link_ok() {
    return s_lastHeardMs != 0 && (millis() - s_lastHeardMs) < LINK_TIMEOUT_MS;
}

const char* brightsign_card_mode() { return s_mode; }

const std::vector<CardSong>& brightsign_songs() { return s_songs; }
uint32_t brightsign_songs_version() { return s_songsVersion; }
int brightsign_now_n() { return s_nowN; }
const String& brightsign_now_line() { return s_nowLine; }
const String& brightsign_last_error() { return s_lastErr; }
