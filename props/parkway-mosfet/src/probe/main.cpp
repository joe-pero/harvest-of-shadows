// Parkway MOSFET probe: drive the Hilitand 4-ch module one channel at a time
// so Joe can map which light is on which output.
//
// Serial commands (115200, newline-terminated):
//   1..4        solo that channel at full, all others off
//   0           all off
//   A           all on full
//   L<ch> <v>   set one channel to level v (0-255), others untouched
//   D<ch>       dim ramp on one channel (0->255->0, ~4 s) to check PWM
//   C           cycle 1->2->3->4, 3 s each, then off
//   S           status
#include <Arduino.h>

static const uint8_t PINS[4] = {25, 26, 27, 33};
static uint8_t level[4] = {0, 0, 0, 0};
static const uint32_t PWM_HZ = 1000;  // MOSFET/opto-friendly; raise later if flicker on camera
static const uint8_t PWM_BITS = 8;

static void set(int ch, uint8_t v) {
  level[ch] = v;
  ledcWrite(ch, v);
}

static void allOff() { for (int i = 0; i < 4; i++) set(i, 0); }

static void status() {
  Serial.printf("[probe] CH1(G25)=%u CH2(G26)=%u CH3(G27)=%u CH4(G33)=%u\n",
                level[0], level[1], level[2], level[3]);
}

void setup() {
  // Drive every output LOW before anything else so the lights don't flash at boot.
  for (int i = 0; i < 4; i++) { pinMode(PINS[i], OUTPUT); digitalWrite(PINS[i], LOW); }
  for (int i = 0; i < 4; i++) { ledcSetup(i, PWM_HZ, PWM_BITS); ledcAttachPin(PINS[i], i); ledcWrite(i, 0); }
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[probe] parkway-mosfet probe ready. 1-4 solo, 0 off, A all, L<ch> <v>, D<ch>, C cycle, S status");
  status();
}

void loop() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.isEmpty()) return;
  char c = toupper(cmd[0]);
  if (c >= '1' && c <= '4' && cmd.length() == 1) {
    int ch = c - '1';
    allOff(); set(ch, 255);
    Serial.printf("[probe] SOLO CH%d (GPIO%u) full\n", ch + 1, PINS[ch]);
  } else if (c == '0') {
    allOff(); Serial.println("[probe] all off");
  } else if (c == 'A') {
    for (int i = 0; i < 4; i++) set(i, 255);
    Serial.println("[probe] all on");
  } else if (c == 'L') {
    int ch = 0, v = 0;
    if (sscanf(cmd.c_str() + 1, "%d %d", &ch, &v) == 2 && ch >= 1 && ch <= 4) {
      set(ch - 1, constrain(v, 0, 255));
      Serial.printf("[probe] CH%d = %d\n", ch, level[ch - 1]);
    } else Serial.println("[probe] usage: L<ch> <0-255>");
  } else if (c == 'D') {
    int ch = cmd.substring(1).toInt();
    if (ch < 1 || ch > 4) { Serial.println("[probe] usage: D<ch>"); return; }
    Serial.printf("[probe] dim ramp CH%d\n", ch);
    for (int v = 0; v <= 255; v += 3) { set(ch - 1, v); delay(8); }
    for (int v = 255; v >= 0; v -= 3) { set(ch - 1, v); delay(8); }
    set(ch - 1, 0);
  } else if (c == 'C') {
    for (int i = 0; i < 4; i++) {
      allOff(); set(i, 255);
      Serial.printf("[probe] cycle: CH%d (GPIO%u)\n", i + 1, PINS[i]);
      delay(3000);
    }
    allOff(); Serial.println("[probe] cycle done, all off");
  } else if (c == 'S') {
    status();
  } else {
    Serial.println("[probe] ? 1-4 solo, 0 off, A all, L<ch> <v>, D<ch>, C cycle, S status");
  }
  status();
}
