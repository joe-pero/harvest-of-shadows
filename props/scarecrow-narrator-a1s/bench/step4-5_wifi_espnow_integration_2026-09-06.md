# Scarecrow A1S — Steps 4+5 WiFi + ESP-NOW + integration

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
[00:02.025] [fleet] WiFi assoc to "<your-ssid>".........
[00:04.276] [fleet] WiFi up: IP 192.168.1.x  RSSI -48  channel 11
[00:04.276] [fleet] ESP-NOW up (broadcast + Burt AA:BB:CC:DD:EE:FF). session=0xAD221E08
[net] wifi_up=1  channel=11 (want 11)  ip=192.168.1.x
[net] heap=223924  psram_free=4059003
[main] ready. c=cycle a=auto x/X=coex b=cueBurt h=hs w=net d=pools
[net] wifi_up=1  channel=11 (want 11)  ip=192.168.1.x
[net] heap=223788  psram_free=4059003
[00:22.579] [coex] START  url=http://192.168.1.x:8080/coex_probe.bin  playback=0
[00:25.649] [coex] broadcasts sent=60   assoc held=1   ch 11->11 (want 11)
[00:25.650] [coex] HTTP code=200  content-len=200000  body-read=200000  RSSI -55->-55
[00:25.660] [coex] RESULT: PASS — WiFi STA + ESP-NOW TX coexist on ch 11 (leader role viable)
[00:32.586] [coex] START  url=http://192.168.1.x:8080/coex_probe.bin  playback=1
[00:33.633] [coex] broadcasts sent=60   assoc held=1   ch 11->11 (want 11)
[00:33.633] [coex] HTTP code=200  content-len=200000  body-read=200000  RSSI -55->-54
[00:33.643] [coex] RESULT: PASS — WiFi STA + ESP-NOW TX coexist on ch 11 (leader role viable)
[main] fixed-session cycle (0x00000001)

[cycle] ===== session 0x00000001  bucket=rotld_scary =====
[00:42.600] [fleet] -> THEME burst  theme=1 tone=1 session=0x00000001 (3x each)
[cycle] INTRO: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 88200 bytes PCM (~2000 ms)
[middle] polling http://192.168.1.x:8080/middle/00000001_sc.wav (deadline 4000 ms)
[middle] downloaded 66194 / 66194 bytes  ok=1
[middle] playing live MIDDLE
[wav] 22050 Hz, 1 ch, 16-bit, 66150 bytes PCM (~1500 ms)
[cycle] OUTRO: no SD pool -> placeholder WAV
[wav] 16000 Hz, 1 ch, 16-bit, 44800 bytes PCM (~1400 ms)
[audio_out] rate -> 16000 Hz
[00:48.489] [fleet] -> NARRATOR_1_DONE (3x)
[00:49.064] [fleet] H1b cue Burt: no confirmation after 3 attempt(s) — proceeding regardless
[00:49.464] [fleet] -> SHOW_STATE 3 (3x)
[cycle] ===== done =====


[cycle] ===== session 0x67CEE985  bucket=corn_friendly =====
[00:56.610] [fleet] -> THEME burst  theme=0 tone=0 session=0x67CEE985 (3x each)
[cycle] INTRO: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 88200 bytes PCM (~2000 ms)
[audio_out] rate -> 22050 Hz
[middle] polling http://192.168.1.x:8080/middle/67cee985_sc.wav (deadline 4000 ms)
[middle] deadline — 1.5 s pad, then bridge
[cycle] OUTRO: no SD pool -> placeholder WAV
[wav] 16000 Hz, 1 ch, 16-bit, 44800 bytes PCM (~1400 ms)
[audio_out] rate -> 16000 Hz
[01:06.747] [fleet] -> NARRATOR_1_DONE (3x)
[01:07.286] [fleet] H1b cue Burt: no confirmation after 3 attempt(s) — proceeding regardless
[01:07.686] [fleet] -> SHOW_STATE 3 (3x)
[cycle] ===== done =====

[01:12.617] [fleet] handshake rollup: cue ok=0 fail=2 (avg 3.0 tries)  burt-acks-rx=0
```
