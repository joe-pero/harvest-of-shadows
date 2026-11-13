# Skeleton A1S (Burt) — live 2-board handshake (Burt side)

Raw serial capture, 2026-09-06. Verbatim USB console output.

```text
ets Jul 29 2019 12:21:46

rst:0x1 (POWERON_RESET),boot:0x1f (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:2
load:0x3fff0030,len:1184
load:0x40078000,len:13232
load:0x40080400,len:3028
entry 0x400805e4

=================================================================
 Skeleton Narrator A1S ("Burt") — follower firmware
=================================================================
[main] ESP32-D0WD-V3 rev3  heap 289924  PSRAM 4192123
[main] on-board amp (GPIO21) = LOW
[main] ES8388 init... OK
[main] I2S init... [audio_out] I2S up: 22050 Hz, 16-bit stereo, MCLK=GPIO0 BCLK=GPIO27 LRCK=GPIO25 DOUT=GPIO26
OK
[main] SD pools...
E (836) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (836) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[   842][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
[pools] SD mount failed (no card? DIP? pull-ups?) — running without pools
[main] placeholder line WAV in PSRAM: 79424B (D4, 1.8 s)
[main] link (WiFi + ESP-NOW)...
[00:01.573] [burt] WiFi assoc to "<your-ssid>".........
[00:03.824] [burt] WiFi up: IP 192.168.1.x  RSSI -23  channel 11
[00:03.824] [burt] ESP-NOW up (broadcast + Scarecrow AA:BB:CC:DD:EE:FF)
[main] ready — waiting for CMD_NARRATOR_2_START from the Scarecrow.
[main] cmds: l w d t p +/-
[00:20.000] [burt] rx: frames=1 cues=0 | starting-ack ok=0 fail=0 | latched theme=0 tone=1 mainshow=0
[00:30.420] [burt] <- CUE  session=0xCA527D91 theme=1 tone=1 (pre-show)
[00:30.423] [burt] -> starting-ack to Scarecrow: SUCCESS after 1 attempt(s) (diagnostic; not waited on)
[burt] line: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 79380 bytes PCM (~1800 ms)
[00:32.557] [burt] -> NARRATOR_2_DONE (3x)  mainshow=0
[00:40.002] [burt] rx: frames=21 cues=1 | starting-ack ok=1 fail=0 | latched theme=1 tone=1 mainshow=0
[01:00.002] [burt] rx: frames=31 cues=1 | starting-ack ok=1 fail=0 | latched theme=1 tone=0 mainshow=0
[01:00.582] [burt] <- CUE  session=0xD4848DFC theme=1 tone=0 (pre-show)
[01:00.584] [burt] -> starting-ack to Scarecrow: SUCCESS after 1 attempt(s) (diagnostic; not waited on)
[burt] line: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 79380 bytes PCM (~1800 ms)
[01:02.720] [burt] -> NARRATOR_2_DONE (3x)  mainshow=0
[01:20.005] [burt] rx: frames=43 cues=2 | starting-ack ok=2 fail=0 | latched theme=1 tone=0 mainshow=0
[01:30.580] [burt] <- CUE  session=0xEE70491A theme=2 tone=1 (pre-show)
[01:30.584] [burt] -> starting-ack to Scarecrow: SUCCESS after 1 attempt(s) (diagnostic; not waited on)
[burt] line: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 79380 bytes PCM (~1800 ms)
[01:32.708] [burt] -> NARRATOR_2_DONE (3x)  mainshow=0
[01:39.713] [burt] rx: frames=63 cues=3 | starting-ack ok=3 fail=0 | latched theme=2 tone=1 mainshow=0
[01:40.008] [burt] rx: frames=63 cues=3 | starting-ack ok=3 fail=0 | latched theme=2 tone=1 mainshow=0
```
