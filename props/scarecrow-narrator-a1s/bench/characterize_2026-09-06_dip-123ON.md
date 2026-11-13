# Scarecrow A1S — Step 1 characterization (DIP 1,2,3 ON)

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
 Scarecrow Narrator A1S — Step 1 bench characterization
=================================================================
-----------------------------------------------------------------
CHIP / MEMORY
  model            : ESP32-D0WD-V3  (rev detected by Arduino: 3)
  cores            : 2
  CPU freq         : 240 MHz
  flash (esp_flash): 4194304 bytes (4.0 MB)
  flash (SDK cfg)  : 4194304 bytes
  PSRAM found      : YES
  PSRAM size       : 4192123 bytes (4.00 MB)
  PSRAM free       : 4192123 bytes
  PSRAM max alloc  : 4128756 bytes
  WiFi STA MAC     : AA:BB:CC:DD:EE:FF
     expect AA:BB:CC:DD:EE:FF = MAC_SCARECROW_A1S (NodeID 0x06)
-----------------------------------------------------------------
I2C SCAN  (SDA=GPIO33, SCL=GPIO32)
  @ 100000 Hz:  0x10
  @ 400000 Hz:  0x10
  VERDICT: ES8388  (0x10/0x11)  -> use ES8388 codec init
-----------------------------------------------------------------
GPIO TRUTH TABLE  (before any SD driver init this pass)
  -- DIP-muxed microSD/JTAG pins --
   gpio  float  pull-up  pull-down   interpretation
     14   1      1        1        SD_CLK  (SDMMC CLK)  EXTERNAL PULL-UP present (line in use)
     15   1      1        1        SD_CMD  (SDMMC CMD / SPI MOSI)  EXTERNAL PULL-UP present (line in use)
      2   1      1        1        SD_D0   (SDMMC D0  / SPI MISO)  EXTERNAL PULL-UP present (line in use)
      4   1      1        1        SD_D1   (SDMMC D1, 4-bit only)  EXTERNAL PULL-UP present (line in use)
     12   0      1        0        SD_D2   (SDMMC D2, 4-bit only) / strap MTDI  no external pull -> FREE
     13   1      1        1        SD_D3   (SDMMC D3 / SPI CS) / KEY2  EXTERNAL PULL-UP present (line in use)
  -- KEY header / prop I/O candidates --
   gpio  float  pull-up  pull-down   interpretation
     36   1      1        -        KEY1 (GPIO36, input-only)  input-only pin (no internal pulls)
     19   1      1        1        KEY3 (GPIO19)   EXTERNAL PULL-UP present (line in use)
     23   0      1        0        KEY4 (GPIO23)   no external pull -> FREE
     18   0      1        0        KEY5 (GPIO18)   no external pull -> FREE
      5   1      1        0        KEY6 (GPIO5, mild strap)  no external pull -> FREE
     22   1      1        0        LED4 (GPIO22, on-board green LED)  no external pull -> FREE
-----------------------------------------------------------------
SD CARD MOUNT PROBE
  target config (arch §1.2): 1-bit SD, DIP 3/4/5 only, frees GPIO12/13
E (1056) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (1056) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[  1061][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [1-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  falling back to 4-bit attempt...
E (1128) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (1128) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[  1133][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [4-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  -> neither mode mounted. Check card seated + DIP switches.

Entering loop: re-scan every 8 s. Flip one DIP switch, watch the next PASS block. Commands: i / g / s

################  PASS 1  (t=8s)  ################
-----------------------------------------------------------------
I2C SCAN  (SDA=GPIO33, SCL=GPIO32)
  @ 100000 Hz:  0x10
  @ 400000 Hz:  0x10
  VERDICT: ES8388  (0x10/0x11)  -> use ES8388 codec init
-----------------------------------------------------------------
GPIO TRUTH TABLE  (before any SD driver init this pass)
  -- DIP-muxed microSD/JTAG pins --
   gpio  float  pull-up  pull-down   interpretation
     14   1      1        1        SD_CLK  (SDMMC CLK)  EXTERNAL PULL-UP present (line in use)
     15   1      1        1        SD_CMD  (SDMMC CMD / SPI MOSI)  EXTERNAL PULL-UP present (line in use)
      2   1      1        1        SD_D0   (SDMMC D0  / SPI MISO)  EXTERNAL PULL-UP present (line in use)
      4   1      1        1        SD_D1   (SDMMC D1, 4-bit only)  EXTERNAL PULL-UP present (line in use)
     12   0      1        0        SD_D2   (SDMMC D2, 4-bit only) / strap MTDI  no external pull -> FREE
     13   1      1        1        SD_D3   (SDMMC D3 / SPI CS) / KEY2  EXTERNAL PULL-UP present (line in use)
  -- KEY header / prop I/O candidates --
   gpio  float  pull-up  pull-down   interpretation
     36   1      1        -        KEY1 (GPIO36, input-only)  input-only pin (no internal pulls)
     19   1      1        1        KEY3 (GPIO19)   EXTERNAL PULL-UP present (line in use)
     23   0      1        0        KEY4 (GPIO23)   no external pull -> FREE
     18   0      1        0        KEY5 (GPIO18)   no external pull -> FREE
      5   0      1        0        KEY6 (GPIO5, mild strap)  no external pull -> FREE
     22   1      1        0        LED4 (GPIO22, on-board green LED)  no external pull -> FREE
-----------------------------------------------------------------
SD CARD MOUNT PROBE
  target config (arch §1.2): 1-bit SD, DIP 3/4/5 only, frees GPIO12/13
E (8239) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (8239) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[  8244][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [1-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  falling back to 4-bit attempt...
E (8311) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (8311) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[  8316][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [4-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  -> neither mode mounted. Check card seated + DIP switches.

################  PASS 2  (t=16s)  ################
-----------------------------------------------------------------
I2C SCAN  (SDA=GPIO33, SCL=GPIO32)
  @ 100000 Hz:  0x10
  @ 400000 Hz:  0x10
  VERDICT: ES8388  (0x10/0x11)  -> use ES8388 codec init
-----------------------------------------------------------------
GPIO TRUTH TABLE  (before any SD driver init this pass)
  -- DIP-muxed microSD/JTAG pins --
   gpio  float  pull-up  pull-down   interpretation
     14   1      1        1        SD_CLK  (SDMMC CLK)  EXTERNAL PULL-UP present (line in use)
     15   1      1        1        SD_CMD  (SDMMC CMD / SPI MOSI)  EXTERNAL PULL-UP present (line in use)
      2   1      1        1        SD_D0   (SDMMC D0  / SPI MISO)  EXTERNAL PULL-UP present (line in use)
      4   1      1        1        SD_D1   (SDMMC D1, 4-bit only)  EXTERNAL PULL-UP present (line in use)
     12   0      1        0        SD_D2   (SDMMC D2, 4-bit only) / strap MTDI  no external pull -> FREE
     13   1      1        1        SD_D3   (SDMMC D3 / SPI CS) / KEY2  EXTERNAL PULL-UP present (line in use)
  -- KEY header / prop I/O candidates --
   gpio  float  pull-up  pull-down   interpretation
     36   1      1        -        KEY1 (GPIO36, input-only)  input-only pin (no internal pulls)
     19   1      1        1        KEY3 (GPIO19)   EXTERNAL PULL-UP present (line in use)
     23   0      1        0        KEY4 (GPIO23)   no external pull -> FREE
     18   0      1        0        KEY5 (GPIO18)   no external pull -> FREE
      5   0      1        0        KEY6 (GPIO5, mild strap)  no external pull -> FREE
     22   1      1        0        LED4 (GPIO22, on-board green LED)  no external pull -> FREE
-----------------------------------------------------------------
SD CARD MOUNT PROBE
  target config (arch §1.2): 1-bit SD, DIP 3/4/5 only, frees GPIO12/13
E (16239) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (16239) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[ 16244][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [1-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  falling back to 4-bit attempt...
E (16311) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (16311) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[ 16316][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [4-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  -> neither mode mounted. Check card seated + DIP switches.

################  PASS 3  (t=24s)  ################
-----------------------------------------------------------------
I2C SCAN  (SDA=GPIO33, SCL=GPIO32)
  @ 100000 Hz:  0x10
  @ 400000 Hz:  0x10
  VERDICT: ES8388  (0x10/0x11)  -> use ES8388 codec init
-----------------------------------------------------------------
GPIO TRUTH TABLE  (before any SD driver init this pass)
  -- DIP-muxed microSD/JTAG pins --
   gpio  float  pull-up  pull-down   interpretation
     14   1      1        1        SD_CLK  (SDMMC CLK)  EXTERNAL PULL-UP present (line in use)
     15   1      1        1        SD_CMD  (SDMMC CMD / SPI MOSI)  EXTERNAL PULL-UP present (line in use)
      2   1      1        1        SD_D0   (SDMMC D0  / SPI MISO)  EXTERNAL PULL-UP present (line in use)
      4   1      1        1        SD_D1   (SDMMC D1, 4-bit only)  EXTERNAL PULL-UP present (line in use)
     12   0      1        0        SD_D2   (SDMMC D2, 4-bit only) / strap MTDI  no external pull -> FREE
     13   1      1        1        SD_D3   (SDMMC D3 / SPI CS) / KEY2  EXTERNAL PULL-UP present (line in use)
  -- KEY header / prop I/O candidates --
   gpio  float  pull-up  pull-down   interpretation
     36   1      1        -        KEY1 (GPIO36, input-only)  input-only pin (no internal pulls)
     19   1      1        1        KEY3 (GPIO19)   EXTERNAL PULL-UP present (line in use)
     23   0      1        0        KEY4 (GPIO23)   no external pull -> FREE
     18   0      1        0        KEY5 (GPIO18)   no external pull -> FREE
      5   0      1        0        KEY6 (GPIO5, mild strap)  no external pull -> FREE
     22   1      1        0        LED4 (GPIO22, on-board green LED)  no external pull -> FREE
-----------------------------------------------------------------
SD CARD MOUNT PROBE
  target config (arch §1.2): 1-bit SD, DIP 3/4/5 only, frees GPIO12/13
E (24239) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (24239) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[ 24244][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [1-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  falling back to 4-bit attempt...
E (24311) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (24311) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[ 24316][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [4-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  -> neither mode mounted. Check card seated + DIP switches.

################  PASS 4  (t=32s)  ################
-----------------------------------------------------------------
I2C SCAN  (SDA=GPIO33, SCL=GPIO32)
  @ 100000 Hz:  0x10
  @ 400000 Hz:  0x10
  VERDICT: ES8388  (0x10/0x11)  -> use ES8388 codec init
-----------------------------------------------------------------
GPIO TRUTH TABLE  (before any SD driver init this pass)
  -- DIP-muxed microSD/JTAG pins --
   gpio  float  pull-up  pull-down   interpretation
     14   1      1        1        SD_CLK  (SDMMC CLK)  EXTERNAL PULL-UP present (line in use)
     15   1      1        1        SD_CMD  (SDMMC CMD / SPI MOSI)  EXTERNAL PULL-UP present (line in use)
      2   1      1        1        SD_D0   (SDMMC D0  / SPI MISO)  EXTERNAL PULL-UP present (line in use)
      4   1      1        1        SD_D1   (SDMMC D1, 4-bit only)  EXTERNAL PULL-UP present (line in use)
     12   0      1        0        SD_D2   (SDMMC D2, 4-bit only) / strap MTDI  no external pull -> FREE
     13   1      1        1        SD_D3   (SDMMC D3 / SPI CS) / KEY2  EXTERNAL PULL-UP present (line in use)
  -- KEY header / prop I/O candidates --
   gpio  float  pull-up  pull-down   interpretation
     36   1      1        -        KEY1 (GPIO36, input-only)  input-only pin (no internal pulls)
     19   1      1        1        KEY3 (GPIO19)   EXTERNAL PULL-UP present (line in use)
     23   0      1        0        KEY4 (GPIO23)   no external pull -> FREE
     18   0      1        0        KEY5 (GPIO18)   no external pull -> FREE
      5   0      1        0        KEY6 (GPIO5, mild strap)  no external pull -> FREE
     22   1      1        0        LED4 (GPIO22, on-board green LED)  no external pull -> FREE
-----------------------------------------------------------------
SD CARD MOUNT PROBE
  target config (arch §1.2): 1-bit SD, DIP 3/4/5 only, frees GPIO12/13
E (32239) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (32239) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[ 32244][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [1-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  falling back to 4-bit attempt...
E (32311) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (32311) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[ 32316][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [4-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  -> neither mode mounted. Check card seated + DIP switches.

################  PASS 5  (t=40s)  ################
-----------------------------------------------------------------
I2C SCAN  (SDA=GPIO33, SCL=GPIO32)
  @ 100000 Hz:  0x10
  @ 400000 Hz:  0x10
  VERDICT: ES8388  (0x10/0x11)  -> use ES8388 codec init
-----------------------------------------------------------------
GPIO TRUTH TABLE  (before any SD driver init this pass)
  -- DIP-muxed microSD/JTAG pins --
   gpio  float  pull-up  pull-down   interpretation
     14   1      1        1        SD_CLK  (SDMMC CLK)  EXTERNAL PULL-UP present (line in use)
     15   1      1        1        SD_CMD  (SDMMC CMD / SPI MOSI)  EXTERNAL PULL-UP present (line in use)
      2   1      1        1        SD_D0   (SDMMC D0  / SPI MISO)  EXTERNAL PULL-UP present (line in use)
      4   1      1        1        SD_D1   (SDMMC D1, 4-bit only)  EXTERNAL PULL-UP present (line in use)
     12   0      1        0        SD_D2   (SDMMC D2, 4-bit only) / strap MTDI  no external pull -> FREE
     13   1      1        1        SD_D3   (SDMMC D3 / SPI CS) / KEY2  EXTERNAL PULL-UP present (line in use)
  -- KEY header / prop I/O candidates --
   gpio  float  pull-up  pull-down   interpretation
     36   1      1        -        KEY1 (GPIO36, input-only)  input-only pin (no internal pulls)
     19   1      1        1        KEY3 (GPIO19)   EXTERNAL PULL-UP present (line in use)
     23   0      1        0        KEY4 (GPIO23)   no external pull -> FREE
     18   0      1        0        KEY5 (GPIO18)   no external pull -> FREE
      5   0      1        0        KEY6 (GPIO5, mild strap)  no external pull -> FREE
     22   1      1        0        LED4 (GPIO22, on-board green LED)  no external pull -> FREE
-----------------------------------------------------------------
SD CARD MOUNT PROBE
  target config (arch §1.2): 1-bit SD, DIP 3/4/5 only, frees GPIO12/13
E (40239) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (40239) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[ 40244][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [1-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  falling back to 4-bit attempt...
E (40311) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (40311) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
[ 40316][E][SD_MMC.cpp:148] begin(): Failed to initialize the card (0x107). Make sure SD card lines have pull-up resistors in place.
  [4-bit] begin() FAILED (no card / wrong DIP config / no pull-ups)
  -> neither mode mounted. Check card seated + DIP switches.
```
