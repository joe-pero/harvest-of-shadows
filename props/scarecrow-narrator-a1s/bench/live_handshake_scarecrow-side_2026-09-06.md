# Scarecrow A1S — live 2-board handshake (Scarecrow side)

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
 Scarecrow Narrator A1S — Steps 4+5 integration (placeholder audio)
=================================================================
[main] ESP32-D0WD-V3 rev3  heap 284676  PSRAM 4192123
[main] on-board amp (GPIO21) = LOW
[main] ES8388 init... OK
[main] I2S init... [audio_out] I2S up: 22050 Hz, 16-bit stereo, MCLK=GPIO0 BCLK=GPIO27 LRCK=GPIO25 DOUT=GPIO26
OK
[main] SD pools...
E (837) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (837) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[   842][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
[pools] SD mount failed (no card? DIP? pull-ups?) — running without pools
[main] placeholder WAVs in PSRAM: intro 88244B outro 44844B
[main] fleet (WiFi + ESP-NOW)...
[00:02.029] [fleet] WiFi assoc to "<your-ssid>".
[00:02.280] [fleet] WiFi up: IP 192.168.1.x  RSSI -19  channel 11
[00:02.280] [fleet] ESP-NOW up (broadcast + Burt AA:BB:CC:DD:EE:FF). session=0x94E40FB2
[net] wifi_up=1  channel=11 (want 11)  ip=192.168.1.x
[net] heap=223816  psram_free=4059003
[main] ready. c=cycle a=auto x/X=coex b=cueBurt h=hs w=net d=pools

[cycle] ===== session 0xF9AEA2BC  bucket=corn_friendly =====
[00:12.002] [fleet] -> THEME burst  theme=0 tone=0 session=0xF9AEA2BC (3x each)
[cycle] INTRO: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 88200 bytes PCM (~2000 ms)
[middle] polling http://192.168.1.x:8080/middle/f9aea2bc_sc.wav (deadline 4000 ms)
[ 14489][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 15005][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 15578][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 16119][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 16638][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 17159][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 17673][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 18196][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[middle] deadline — 1.5 s pad, then bridge
[cycle] OUTRO: no SD pool -> placeholder WAV
[wav] 16000 Hz, 1 ch, 16-bit, 44800 bytes PCM (~1400 ms)
[audio_out] rate -> 16000 Hz
[00:21.925] [fleet] -> NARRATOR_1_DONE (3x)
[00:21.934] [fleet] <- Burt CMD_ACK (starting), session=0xF9AEA2BC  [total 1]
[00:22.331] [fleet] H1b cue Burt: SUCCESS after 1 attempt(s) — proceeding regardless
[00:22.731] [fleet] -> SHOW_STATE 3 (3x)
[cycle] ===== done =====


[cycle] ===== session 0x9F400101  bucket=rotld_scary =====
[00:42.002] [fleet] -> THEME burst  theme=1 tone=1 session=0x9F400101 (3x each)
[cycle] INTRO: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 88200 bytes PCM (~2000 ms)
[audio_out] rate -> 22050 Hz
[middle] polling http://192.168.1.x:8080/middle/9f400101_sc.wav (deadline 4000 ms)
[ 44556][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 45077][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 45622][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 46139][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 46656][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 47194][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 47710][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 48226][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[middle] deadline — 1.5 s pad, then bridge
[cycle] OUTRO: no SD pool -> placeholder WAV
[wav] 16000 Hz, 1 ch, 16-bit, 44800 bytes PCM (~1400 ms)
[audio_out] rate -> 16000 Hz
[00:51.953] [fleet] -> NARRATOR_1_DONE (3x)
[00:51.966] [fleet] <- Burt CMD_ACK (starting), session=0x9F400101  [total 2]
[00:52.359] [fleet] H1b cue Burt: SUCCESS after 1 attempt(s) — proceeding regardless
[00:52.759] [fleet] -> SHOW_STATE 3 (3x)
[cycle] ===== done =====


[cycle] ===== session 0x537E6C60  bucket=rotld_friendly =====
[01:12.005] [fleet] -> THEME burst  theme=1 tone=0 session=0x537E6C60 (3x each)
[cycle] INTRO: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 88200 bytes PCM (~2000 ms)
[audio_out] rate -> 22050 Hz
[middle] polling http://192.168.1.x:8080/middle/537e6c60_sc.wav (deadline 4000 ms)
[ 74558][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 75079][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 75658][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 76184][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 76698][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 77212][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 77728][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[ 78244][E][WiFiClient.cpp:275] connect(): socket error on fd 48, errno: 104, "Connection reset by peer"
[middle] deadline — 1.5 s pad, then bridge
[cycle] OUTRO: no SD pool -> placeholder WAV
[wav] 16000 Hz, 1 ch, 16-bit, 44800 bytes PCM (~1400 ms)
[audio_out] rate -> 16000 Hz
[01:21.968] [fleet] -> NARRATOR_1_DONE (3x)
[01:21.977] [fleet] <- Burt CMD_ACK (starting), session=0x537E6C60  [total 3]
[01:22.372] [fleet] H1b cue Burt: SUCCESS after 1 attempt(s) — proceeding regardless
[01:22.772] [fleet] -> SHOW_STATE 3 (3x)
[cycle] ===== done =====

[01:34.639] [fleet] handshake rollup: cue ok=3 fail=0 (avg 1.0 tries)  burt-acks-rx=3
```
