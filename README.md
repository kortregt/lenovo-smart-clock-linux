# Lenovo Smart Clock (gen 1) — running plain Linux

The Lenovo Smart Clock (CD-24501F, codename "sparrow") was a Google Assistant alarm clock
running **Android Things**. Google and Lenovo have ended support, and the Google Home app
no longer manages it. This project replaces Android Things with a small **Alpine Linux**
system, built on the clock's own 4.4 kernel, so it can be reused (the goal here is Home
Assistant) without any Google services.

Everything below was worked out on one unit, from a Mac, without opening the case or
soldering anything. A USB-A-to-USB-A cable is the only extra hardware.

## Status

| Area | Status | Notes |
|---|---|---|
| Bootloader + AVB unlock | ✅ | Public leaked AVB unlock key; see [01](docs/01-unlocking.md) |
| Full eMMC backup / restore | ✅ | mtkclient over the BootROM; see [02](docs/02-backup.md) |
| Kernel built from source | ✅ | Google's release, GCC 4.9, stock config; see [03](docs/03-kernel.md) |
| Custom kernel + initramfs boots | ✅ | Needs two kernel patches; see [04](docs/04-booting-a-custom-kernel.md) |
| Root shell over USB (no UART) | ✅ | USB gadget serial + network; see [05](docs/05-usb-and-alpine.md) |
| Alpine Linux as the default OS | ✅ | Slot B; stock Android stays on slot A; see [05](docs/05-usb-and-alpine.md) |
| Wi-Fi (MT7668) | ✅ | Vendor module + firmware; see [06](docs/06-wifi.md) |
| Speaker + microphones | ✅ | Amp and ADC replayed from the stock app (the mics record, but nothing uses them yet); see [07](docs/07-audio.md) |
| Display | ✅ | Plain `/dev/fb0` (kernel patch 0004); panel mounted sideways; see [08](docs/08-display.md) |
| Clock face | ✅ | LVGL: time (12 or 24 h), date, weather, next alarm; see [08](docs/08-display.md#the-clock-face) |
| Automatic brightness | ✅ | Light sensor → backlight, smooth fades, dim at night; see [08](docs/08-display.md#automatic-brightness) |
| Touchscreen | ✅ | Tap for the forecast, long-press for the alarms, now-playing controls; see [08](docs/08-display.md#touch) |
| Buttons + mic switch | ✅ | Volume up/down (hardware amp volume), snooze; combos configurable; see [07](docs/07-audio.md#volume-and-buttons) |
| Accelerometer | ✅ | Taps on the case: a double tap snoozes or lights the screen; see [07](docs/07-audio.md#volume-and-buttons) |
| Bluetooth | ✅ | Bluetooth speaker (A2DP sink, SBC/AAC) through a relay to BlueZ; hands-free calls don't work (the call audio never reaches Linux); see [10](docs/10-bluetooth.md) |
| Home Assistant | ✅ | Weather and forecast, music, alarms (helpers, a dashboard card, a next-alarm sensor); see [09](docs/09-home-assistant.md) |
| Music | ✅ | Music Assistant player (squeezelite, Spotify Connect via MA); one volume shared with the buttons; see [09](docs/09-home-assistant.md#music-music-assistant) |
| Now playing | ✅ | Cover, title, progress and touch controls; see [09](docs/09-home-assistant.md#now-playing) |
| Alarms | ✅ | Set in Home Assistant or on the clock, ring offline too; music or a built-in sound, fade-in, snooze; see [09](docs/09-home-assistant.md#alarms) |

A reference of every chip, bus address, GPIO and partition found along the way is in
[docs/hardware.md](docs/hardware.md).

## How it fits together

```
 lk (stock, unlocked)                      -> always boots slot B now
   └─ boot_b: our 4.4 kernel (+ built-in initramfs)
        └─ initramfs /init: USB gadget (serial + network), then switch_root
             └─ system_b: Alpine Linux 3.24 (busybox init)
                  ├─ Wi-Fi (MT7668 module + vendor firmware), SSH (dropbear)
                  ├─ audio-init: TAS5805M amp + TLV320ADC3101 mics over I2C
                  ├─ clockface: LVGL clock on /dev/fb0
                  ├─ ha-poll: weather from Home Assistant for the clock face
                  ├─ ha-media: now playing from Home Assistant, and its controls
                  ├─ clock-alarm: alarms (Home Assistant helpers, or set on the clock)
                  ├─ autobright: backlight from the light sensor
                  ├─ clockkeys: buttons (volume), the mic switch, taps
                  ├─ squeezelite: Music Assistant player, volume -> amp
                  └─ BlueZ + btrelay + bluez-alsa: Bluetooth speaker
 slot A: untouched stock Android Things    -> `fastboot set_active a` to go back
```

## Repository layout

| Path | What |
|---|---|
| [docs/](docs/) | The write-up, in the order things were done |
| [kernel/patches/](kernel/patches/) | Our changes to Google's kernel source |
| [kernel/smartclock.config](kernel/smartclock.config) | Config options changed from stock |
| [initramfs/](initramfs/) | The initramfs `/init` (and a tiny reboot probe used for debugging) |
| [userspace/](userspace/) | Programs for the clock: the LVGL clock face, automatic brightness, buttons and taps (`clockkeys`), the Bluetooth relay (`btrelay`), a framebuffer test |
| [homeassistant/](homeassistant/) | Home Assistant package (the alarm helpers and next-alarm sensor), a dashboard card and dashboard |
| [rootfs-overlay/](rootfs-overlay/) | Files layered onto Alpine: boot script, inittab, configs, alarm sounds, and the scripts for alarms, volume and Home Assistant |
| [tools/](tools/) | Build and helper scripts (boot image repack, initramfs, rootfs, squeezelite, diagnostics) |
| [at_auth_unlock.py](at_auth_unlock.py) | AOSP's Android Things AVB unlock tool, fixed for Python 3 |

Not in the repo (you need your own): your device backup, the AVB unlock credentials, the
vendor firmware blobs (extracted from your own `vendor_a` partition by
`tools/mkrootfs.sh`).

## Warnings

- This erases the device's user data and replaces its OS on slot B. **Make the full backup
  first** ([02](docs/02-backup.md)); it's the only way back if something goes wrong badly.
- The unlock uses a leaked Google/Lenovo key; this only works because it leaked.
- Nothing here is supported by Lenovo or Google. It worked on one unit; yours may differ
  (in particular, the second hardware revision is not covered).

## Credits

- deadman96385 on XDA for the unlock guide, kernel source mirror and firmware:
  [Lenovo Smart Clock (Bootloader/AVB unlock, Firmware, Region Changer, Kernel Source)](https://xdaforums.com/t/lenovo-smart-clock-bootloader-avb-unlock-firmware-region-changer-kernel-source.4130295/)
  and [android_kernel_lenovo_mt8167s](https://github.com/deadman96385/android_kernel_lenovo_mt8167s)
- deletescape for the leaked AVB unlock key and stock firmware
- [untocodes/lenovo-cube-hacking](https://github.com/untocodes/lenovo-cube-hacking) for
  collected community notes, including the UART pinout
- [bkerler/mtkclient](https://github.com/bkerler/mtkclient) for BootROM access
- [LVGL](https://lvgl.io), [Inter](https://rsms.me/inter/), Home Assistant's weather icons
  and [Meteocons](https://meteocons.com) (Bas Milius) for the clock face
- [Music Assistant](https://music-assistant.io), [squeezelite](https://github.com/ralph-irving/squeezelite),
  [BlueZ](https://www.bluez.org) and [bluez-alsa](https://github.com/arkq/bluez-alsa) for the audio
- [skylot/jadx](https://github.com/skylot/jadx), used to read the stock app's hardware setup
