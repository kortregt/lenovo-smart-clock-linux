# 06 — Wi-Fi

The Wi-Fi/Bluetooth chip is a **MediaTek MT7668** on the SDIO bus `mmc1`: SDIO IDs
`037a:7668` (Wi-Fi) and `037a:7608` (Bluetooth). On boot the chip is detected, but no driver
claims it and there's no `wlan0`.

## Driver

The drivers are **loadable modules**, and building the kernel (with the stock config)
already produces them:

```
out/drivers/misc/mediatek/connectivity/wlan/gen4-mt7668/wlan_drv_gen4_mt7668.ko
out/drivers/misc/mediatek/connectivity/bt/mt76xx/sdio/btmtksdio.ko
```

The Wi-Fi module is 41 MB with debug info; `aarch64-linux-android-strip --strip-debug`
takes it to 3 MB.

## Firmware

The module requests its firmware from `/lib/firmware` (this kernel's only search path).
The files are on the stock `vendor` partition under `/firmware`, and
[`tools/mkrootfs.sh`](../tools/mkrootfs.sh) copies them out of your `vendor_a` backup with
`debugfs`:

```
WIFI_RAM_CODE_MT7668.bin        WIFI_RAM_CODE2_SDIO_MT7668.bin
mt7668_patch_e2_hdr.bin         EEPROM_MT7668.bin
TxPwrLimit_MT76x8.dat           wifi.cfg
```

The same directory also has the GPU firmware (`rgx.fw.signed.*`, included too) and
touchscreen firmware (`FT6336U_*.bin`, **deliberately left out**: if the touch driver finds
it, it tries to reflash the touch controller).

## Bringing it up

`insmod wlan_drv_gen4_mt7668.ko` produces `wlan0` immediately. After that it's ordinary
`wpa_supplicant` and `udhcpc`; `rcS` does this in the background at boot (so a slow
firmware load can't block the console):

```sh
wpa_passphrase 'SSID' 'password' >> /etc/wpa_supplicant/wpa_supplicant.conf
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
udhcpc -b -i wlan0
```

(`wpa_passphrase` also writes the plain-text password as a comment; delete that line.)

`rfkill: Cannot open RFKILL control device` from `wpa_supplicant` is harmless: this kernel
has no rfkill support. The radio worked on 2.4 GHz here; 5 GHz wasn't tested.
