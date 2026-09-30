# Hardware reference — Lenovo Smart Clock (gen 1)

As found on one unit (model CD-24501F, board "smini", last stock build 11.51.6). Linux GPIO
numbers are `387 + SoC pin` (the SoC GPIO chip's base on this kernel).

## Core

| Item | Detail |
|---|---|
| SoC | MediaTek MT8167S (reports "MT8167A"), 4× Cortex-A35 |
| RAM | ~683 MB usable (at `0x40000000`) |
| Storage | eMMC "FN62MB", ~7.3 GiB user area, A/B layout |
| PMIC | MediaTek MT6392 |
| Stock OS | Android Things 1.x (oc-mr1 IoT), kernel 4.4.95 |
| Bootloader | MediaTek lk, loads the board DTB from `oem_bootloader`; arms the SoC watchdog |

## Buses and peripherals

| Function | Chip | Connection | Driver status |
|---|---|---|---|
| Display panel | ST7701S 480×800 | MIPI DSI, 2 lanes; reset GPIO 66 | Kernel LCM driver + `/dev/fb0` (patch 0004, [08](08-display.md)) |
| Backlight | SGM37603A | i2c-2 `0x36`; enable pin 22 (GPIO 409) | Kernel driver, `lcd-backlight` LED |
| Touch | FocalTech FT6336U | i2c-0 `0x38` | Kernel driver (`mtk-tpd`), not tested |
| Speaker amp | TI TAS5805M (PBTL) | i2c-2 `0x2c`; I2S 8CH out → `hw:0,0` | No driver; userspace init ([07](07-audio.md)) |
| Mic ADC | TI TLV320ADC3101 | i2c-1 `0x1b`; enable pin 24 (GPIO 411); TDM RX → `hw:0,1` | No driver; userspace init |
| Accelerometer | Bosch BMA253 | i2c-0 `0x18` | None |
| Light/proximity | Lite-On LTR-578ALS (`PART_ID` 0xB1) | i2c-0 `0x53` | No driver; read from userspace by `autobright` ([08](08-display.md#automatic-brightness)) |
| Wi-Fi / BT | MediaTek MT7668 | SDIO `mmc1` (`037a:7668` / `037a:7608`) | Modules + vendor firmware ([06](06-wifi.md)) |
| Buttons | MT6392 keys | `mtk-pmic-keys` | Kernel driver |
| RGB LED | — | `/sys/class/leds/{red,green,blue}` | Kernel driver |
| USB | MUSB (`musb-hdrc`), USB 2.0 on a USB-A port | Device mode via `swmode` | Gadget: ACM + ECM |
| UART | ttyMT0 on USB-A pins 5/6 | 1.8 V, 921600 8N1 | Not used here |

Other GPIOs used by the stock app: 15 and 16 (likely the mic-mute and camera switches).

## ALSA

| Device | Name | Use |
|---|---|---|
| `hw:0,0` | I2S8CH Playback | Speaker |
| `hw:0,1` | TDM_Capture | Microphones (2 ch, S32_LE, 48 kHz) |
| `hw:0,2` | MultiMedia1_Capture | DMIC path — not connected |
| `hw:0,3`–`6` | AWB / DL1 / BT / DL2 | Echo reference, alternative playback, Bluetooth |

## Partitions (relevant ones)

| Partition | Size | Use in this project |
|---|---|---|
| `misc` | 1 MiB | A/B control block at `0x800` ([05](05-usb-and-alpine.md)) |
| `boot_a` / `boot_b` | 32 MiB | Stock Android / our kernel; `boot_b`+16 MiB = debug log area |
| `system_a` / `system_b` | 512 MiB | Stock Android / Alpine Linux |
| `vendor_a` | 64 MiB | Source of Wi-Fi/GPU firmware and stock audio configs |
| `oem_a` | 500 MiB | Board app (`sparrow.oem`) with the audio setup; `kernel.dtb` |
| `oem_bootloader_a/b` | 4 MiB | Android DT table: the board's device tree |
| `userdata` | ~5 GiB | Unused by Alpine so far |

## Display engine (MT8167)

| Block | Address | Notes |
|---|---|---|
| OVL0 | `0x14007000` | Overlay/compose |
| RDMA0 | `0x14009000` | Scan-out read DMA |
| WDMA0 | `0x1400b000` | Decouple write-back |
| COLOR / CCORR / AAL / GAMMA / DITHER | `0x1400c000`–`0x14010000` | Picture-quality chain |
| DSI0 | `0x14012000` | |
| MUTEX | `0x14015000` | |
| SMI larb0 | `0x14016000` | Display's memory port (via M4U `0x10203000`) |
