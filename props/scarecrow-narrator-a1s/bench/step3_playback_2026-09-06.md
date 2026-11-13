# Scarecrow A1S — Step 3 playback path (placeholder audio)

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
 Scarecrow Narrator A1S — Step 3 playback path (placeholder audio)
=================================================================
[main] ESP32-D0WD-V3  heap 330372  PSRAM 4192123
[main] on-board amp (GPIO21) = LOW
[main] ES8388 init... OK
[main] I2S init... [audio_out] I2S up: 22050 Hz, 16-bit stereo, MCLK=GPIO0 BCLK=GPIO27 LRCK=GPIO25 DOUT=GPIO26
OK
[main] SD pools...
E (823) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (823) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[   828][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
[pools] SD mount failed (no card? DIP? pull-ups?) — running without pools
[main] pools mounted = 0
[main] placeholder WAVs in PSRAM: intro 88244 B (22050Hz), outro 44844 B (16000Hz)
[main] ready. c=cycle  a=auto  d=dump  r=regs  p=amp  +/-=vol

[cycle] ===== session 1  theme=corn_scary tone=scary  bucket=corn_scary =====
[cycle] INTRO: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 88200 bytes PCM (~2000 ms)
[cycle] MIDDLE: placeholder (live Gemini pipeline not wired)
[cycle] OUTRO: no SD pool -> placeholder WAV
[wav] 16000 Hz, 1 ch, 16-bit, 44800 bytes PCM (~1400 ms)
[audio_out] rate -> 16000 Hz
[cycle] ===== done =====


[cycle] ===== session 2  theme=corn_friendly tone=friendly  bucket=corn_friendly =====
[cycle] INTRO: no SD pool -> placeholder WAV
[wav] 22050 Hz, 1 ch, 16-bit, 88200 bytes PCM (~2000 ms)
[audio_out] rate -> 22050 Hz
[cycle] MIDDLE: placeholder (live Gemini pipeline not wired)
[cycle] OUTRO: no SD pool -> placeholder WAV
[wav] 16000 Hz, 1 ch, 16-bit, 44800 bytes PCM (~1400 ms)
[audio_out] rate -> 16000 Hz
[cycle] ===== done =====

[main] pool table:
   intro/corn_friendly      0    outro 0
   intro/corn_scary         0    outro 0
   intro/rotld_friendly     0    outro 0
   intro/rotld_scary        0    outro 0
   intro/pumpkins_friendly  0    outro 0
   intro/pumpkins_scary     0    outro 0
   bridge/generic 0   pool_version 0   mounted 0
```
