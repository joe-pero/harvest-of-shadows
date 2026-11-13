// =============================================================================
// Scarecrow Narrator A1S — Step 1 bench characterization
// =============================================================================
//
// Throwaway sketch. Answers the "NOT verified — needs the board on the bench"
// rows of docs/a1s-board-and-audio-architecture.md Part 0:
//
//   1. Which audio codec is on THIS board  (I2C scan: 0x10 ES8388 / 0x1A AC101)
//   2. What the current DIP-switch bank actually configures  (SD mode probe +
//      GPIO idle-level truth table for the 5 shared pins)
//   3. Which GPIOs are genuinely free in that configuration
//   4. PSRAM / flash / chip revision
//
// It is SAFE: it never drives a pin as OUTPUT. It only reads — floating, with
// internal pull-up, and with internal pull-down — so an externally driven line
// (e.g. an SD card's on-board pull-ups) is distinguishable from a floating one.
//
// Loop behaviour: every LOOP_MS it re-runs the I2C scan, the GPIO truth table,
// and a fresh SD mount attempt, printing a numbered PASS block. Flip one DIP
// switch, watch the next PASS block, record what changed.
//
// Serial commands (type + Enter):
//   i   re-run I2C scan now
//   g   re-run GPIO truth table now
//   s   re-run SD mount probe now (1-bit then 4-bit)
//
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <FS.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp32-hal-psram.h>
#include <WiFi.h>

// ── Pin map (AI-Thinker Audio Kit v2.2, cross-checked across esp-adf / NuttX /
//    arduino-audiokit — see architecture doc §1.1) ──────────────────────────────
static const int I2C_SDA = 33;
static const int I2C_SCL = 32;

// The 5 DIP-muxed pins: microSD slot <-> ESP32. Community tables disagree on the
// switch->pin order and ON/OFF polarity; this sketch measures the result rather
// than trusting the table.
struct NamedPin { int gpio; const char* name; };
static const NamedPin SHARED_PINS[] = {
    { 14, "SD_CLK  (SDMMC CLK)" },
    { 15, "SD_CMD  (SDMMC CMD / SPI MOSI)" },
    {  2, "SD_D0   (SDMMC D0  / SPI MISO)" },
    {  4, "SD_D1   (SDMMC D1, 4-bit only)" },
    { 12, "SD_D2   (SDMMC D2, 4-bit only) / strap MTDI" },
    { 13, "SD_D3   (SDMMC D3 / SPI CS) / KEY2" },
};

// KEY header — the prop's real free-GPIO budget lives here (architecture doc §1.3)
static const NamedPin KEY_PINS[] = {
    { 36, "KEY1 (GPIO36, input-only)" },
    { 19, "KEY3 (GPIO19)" },
    { 23, "KEY4 (GPIO23)" },
    { 18, "KEY5 (GPIO18)" },
    {  5, "KEY6 (GPIO5, mild strap)" },
    { 22, "LED4 (GPIO22, on-board green LED)" },
};

static uint32_t s_pass = 0;

static void printLine() { Serial.println(F("-----------------------------------------------------------------")); }

// ── chip / memory ────────────────────────────────────────────────────────────
static void reportChip() {
    printLine();
    Serial.println(F("CHIP / MEMORY"));
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    Serial.printf("  model            : %s  (rev detected by Arduino: %d)\n",
                  ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("  cores            : %d\n", ci.cores);
    Serial.printf("  CPU freq         : %u MHz\n", (unsigned)getCpuFrequencyMhz());
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    Serial.printf("  flash (esp_flash): %u bytes (%.1f MB)\n",
                  (unsigned)flash, flash / 1048576.0);
    Serial.printf("  flash (SDK cfg)  : %u bytes\n", (unsigned)ESP.getFlashChipSize());

    bool psram_ok = psramFound();
    Serial.printf("  PSRAM found      : %s\n", psram_ok ? "YES" : "NO");
    if (psram_ok) {
        Serial.printf("  PSRAM size       : %u bytes (%.2f MB)\n",
                      (unsigned)ESP.getPsramSize(), ESP.getPsramSize() / 1048576.0);
        Serial.printf("  PSRAM free       : %u bytes\n", (unsigned)ESP.getFreePsram());
        Serial.printf("  PSRAM max alloc  : %u bytes\n", (unsigned)ESP.getMaxAllocPsram());
    } else {
        Serial.println(F("  -> PSRAM not usable. MIDDLE push-then-play buffer (arch §2.2)"));
        Serial.println(F("     depends on this. Check board_build PSRAM flags / module variant."));
    }

    // MAC — identifies which narrator node this board is.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.printf("  WiFi STA MAC     : %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.println(F("     expect AA:BB:CC:DD:EE:FF = MAC_SCARECROW_A1S (NodeID 0x06)"));
}

// ── I2C codec scan ───────────────────────────────────────────────────────────
static void scanI2C() {
    printLine();
    Serial.println(F("I2C SCAN  (SDA=GPIO33, SCL=GPIO32)"));
    for (uint32_t hz : { 100000u, 400000u }) {
        Wire.end();
        Wire.begin(I2C_SDA, I2C_SCL, hz);
        delay(5);
        int found = 0;
        Serial.printf("  @ %lu Hz:", (unsigned long)hz);
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                Serial.printf("  0x%02X", addr);
                found++;
            }
        }
        if (!found) Serial.print(F("  (none)"));
        Serial.println();
    }
    // Interpret
    Wire.end();
    Wire.begin(I2C_SDA, I2C_SCL, 100000u);
    delay(5);
    auto present = [](uint8_t a) {
        Wire.beginTransmission(a);
        return Wire.endTransmission() == 0;
    };
    bool es8388 = present(0x10) || present(0x11);
    bool ac101  = present(0x1A);
    Serial.print(F("  VERDICT: "));
    if (es8388 && !ac101) Serial.println(F("ES8388  (0x10/0x11)  -> use ES8388 codec init"));
    else if (ac101 && !es8388) Serial.println(F("AC101   (0x1A)       -> use AC101 codec init (EOL part)"));
    else if (es8388 && ac101) Serial.println(F("BOTH addresses answered?? re-check wiring"));
    else Serial.println(F("NEITHER 0x10 nor 0x1A answered — codec not on this bus / wrong pins / bad board"));
}

// ── GPIO idle-level truth table ──────────────────────────────────────────────
// float / pull-up / pull-down reads. Reading pattern:
//   HIGH,HIGH,HIGH  -> strong external pull-up  (e.g. SD card DAT/CMD lines wired)
//   LOW ,LOW ,LOW   -> strong external pull-down / driven low
//   HIGH,HIGH,LOW   -> weak/none external; follows internal pull  -> pin is FREE
//   x   ,HIGH,LOW   -> floating -> pin is FREE
static void gpioTruthTable(const NamedPin* pins, size_t n, const char* title) {
    Serial.println(title);
    Serial.println(F("   gpio  float  pull-up  pull-down   interpretation"));
    for (size_t i = 0; i < n; i++) {
        int g = pins[i].gpio;
        pinMode(g, INPUT);          delayMicroseconds(200); int f  = digitalRead(g);
        pinMode(g, INPUT_PULLUP);   delayMicroseconds(200); int pu = digitalRead(g);
        int pd = -1;
        // GPIO34-39 are input-only: no internal pull resistors at all.
        if (g <= 33) { pinMode(g, INPUT_PULLDOWN); delayMicroseconds(200); pd = digitalRead(g); }
        pinMode(g, INPUT);

        const char* interp;
        if (g >= 34)                       interp = "input-only pin (no internal pulls)";
        else if (pu == 1 && pd == 1)       interp = "EXTERNAL PULL-UP present (line in use)";
        else if (pu == 0 && pd == 0)       interp = "EXTERNAL PULL-DOWN / driven low";
        else                               interp = "no external pull -> FREE";

        Serial.printf("   %4d   %d      %d        %s        %-14s  %s\n",
                      g, f, pu, (pd < 0 ? "-" : (pd ? "1" : "0")), pins[i].name, interp);
    }
}

static void gpioProbe() {
    printLine();
    Serial.println(F("GPIO TRUTH TABLE  (before any SD driver init this pass)"));
    gpioTruthTable(SHARED_PINS, sizeof(SHARED_PINS) / sizeof(SHARED_PINS[0]),
                   "  -- DIP-muxed microSD/JTAG pins --");
    gpioTruthTable(KEY_PINS, sizeof(KEY_PINS) / sizeof(KEY_PINS[0]),
                   "  -- KEY header / prop I/O candidates --");
}

// ── SD mount probe ───────────────────────────────────────────────────────────
static void listRoot() {
    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) { Serial.println(F("    (cannot open root)")); return; }
    int count = 0;
    for (File e = root.openNextFile(); e; e = root.openNextFile()) {
        Serial.printf("    %-24s %s%u\n", e.name(),
                      e.isDirectory() ? "<dir>" : "", e.isDirectory() ? 0 : (unsigned)e.size());
        if (++count >= 20) { Serial.println(F("    ... (truncated)")); break; }
    }
    if (!count) Serial.println(F("    (card is empty)"));
    // The real firmware reads /index.csv once at boot (arch §2.1)
    if (SD_MMC.exists("/index.csv") || SD_MMC.exists("/scarecrow/index.csv"))
        Serial.println(F("    -> index.csv found (pool layout looks provisioned)"));
}

static bool trySD(bool oneBit) {
    SD_MMC.end();
    delay(20);
    bool pinsOk = oneBit ? SD_MMC.setPins(14, 15, 2)
                         : SD_MMC.setPins(14, 15, 2, 4, 12, 13);
    if (!pinsOk) { Serial.printf("  [%d-bit] setPins() rejected\n", oneBit ? 1 : 4); return false; }
    // begin(mountpoint, mode1bit, format_if_fail, freq_khz, maxfiles)
    // Conservative clock for the bench probe — a slow mount that succeeds beats
    // a fast one that false-fails on jumper leads. The real firmware can raise it.
    bool ok = SD_MMC.begin("/sdcard", oneBit, false, SDMMC_FREQ_DEFAULT, 5);
    if (!ok) { Serial.printf("  [%d-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)\n", oneBit ? 1 : 4); return false; }
    uint8_t ct = SD_MMC.cardType();
    const char* cts = ct == CARD_MMC ? "MMC" : ct == CARD_SD ? "SDSC" : ct == CARD_SDHC ? "SDHC" : "UNKNOWN";
    Serial.printf("  [%d-bit] MOUNTED  type=%s  size=%.2f GB  used=%.2f MB\n",
                  oneBit ? 1 : 4, cts,
                  SD_MMC.cardSize() / (1024.0 * 1024.0 * 1024.0),
                  SD_MMC.usedBytes() / (1024.0 * 1024.0));
    listRoot();
    return true;
}

static void sdProbe() {
    printLine();
    Serial.println(F("SD CARD MOUNT PROBE"));
    Serial.println(F("  target config (arch §1.2): 1-bit SD, DIP 3/4/5 only, frees GPIO12/13"));
    bool one = trySD(true);
    if (!one) {
        Serial.println(F("  falling back to 4-bit attempt..."));
        bool four = trySD(false);
        if (four) Serial.println(F("  -> 4-bit works, 1-bit does not on this board/DIP config."));
        else      Serial.println(F("  -> neither mode mounted. Check card seated + DIP switches."));
    }
    SD_MMC.end();
}

// ── serial command handling ──────────────────────────────────────────────────
static void handleSerial() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            line.trim();
            if (line == "i") scanI2C();
            else if (line == "g") gpioProbe();
            else if (line == "s") sdProbe();
            line = "";
        } else line += c;
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println(F("================================================================="));
    Serial.println(F(" Scarecrow Narrator A1S — Step 1 bench characterization"));
    Serial.println(F("================================================================="));
    reportChip();
    scanI2C();
    gpioProbe();
    sdProbe();
    Serial.println();
    Serial.println(F("Entering loop: re-scan every 8 s. Flip one DIP switch, watch the"
                     " next PASS block. Commands: i / g / s"));
}

void loop() {
    handleSerial();
    static uint32_t last = 0;
    const uint32_t LOOP_MS = 8000;
    if (millis() - last >= LOOP_MS) {
        last = millis();
        Serial.println();
        Serial.printf("################  PASS %lu  (t=%lus)  ################\n",
                      (unsigned long)++s_pass, (unsigned long)(millis() / 1000));
        scanI2C();
        gpioProbe();
        sdProbe();
    }
}
