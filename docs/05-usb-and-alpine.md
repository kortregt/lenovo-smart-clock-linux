# 05 — USB console and Alpine Linux

## A shell over USB, no UART needed

The clock's USB-A port can be a USB *device*, which is how fastboot works. The kernel can do
the same with a configfs USB gadget, giving a root shell over the same cable used for
fastboot. No serial adapter and no soldering.

Two things are needed:

1. **Force device mode.** The port's OTG ID pin is pulled low internally, which means
   "host". `CONFIG_MTK_MUSB_SW_WITCH_MODE` adds a sysfs switch:
   `echo device > /sys/devices/platform/soc/mt_usb/swmode`.
2. **A gadget** with CDC-ACM (serial) and CDC-ECM (network) functions, bound to the
   `musb-hdrc` controller. See [`initramfs/init`](../initramfs/init).

On macOS it shows up as `/dev/cu.usbmodem*` (macOS asks once whether to allow the
accessory) and a new network interface. The clock is `192.168.7.2`; give the Mac
`192.168.7.1`:

```sh
screen /dev/cu.usbmodem* 115200                       # serial shell
sudo ifconfig enX 192.168.7.1 netmask 255.255.255.0   # then telnet/ssh 192.168.7.2
```

Known problem: the USB network link handles pings and interactive use, but **bulk
transfers stall** after roughly 30–230 KB in either direction. So we move files with
`fastboot flash` or over Wi-Fi instead.

The community UART (USB-A pins 5/6, **1.8 V**, 921600 baud) is documented in
[lenovo-cube-hacking](https://github.com/untocodes/lenovo-cube-hacking), but none of this
work needed it.

## The Alpine root filesystem

[`tools/mkrootfs.sh`](../tools/mkrootfs.sh) builds a 512 MiB ext4 image for `system_b` (the
unused slot-B copy of Android's system partition):

- Alpine 3.24 aarch64 minirootfs, plus `dropbear`, `wpa_supplicant`, `iw`, `alsa-utils`,
  `i2c-tools` and a few others, installed with `apk` in a chroot (qemu binfmt emulation in
  the build VM).
- [`rootfs-overlay/`](../rootfs-overlay/): busybox init (no OpenRC), the boot script
  `/etc/clock/rcS`, audio setup and the register tables.
- The Wi-Fi kernel module (stripped) and firmware, extracted from your own `vendor_a`
  backup.
- Your SSH public key for `root` (dropbear runs key-only).
- ext4 made without `metadata_csum` features, for the 4.4 kernel.

About 40 MB is used.

```sh
fastboot flash system_b build/system_b-alpine.img   # raw image; fastboot sparses it
fastboot boot build/boot_linux.img                   # try it without changing the boot slot
```

Busybox `init` runs `/etc/clock/rcS` (mounts, audio, USB network, Wi-Fi, time), then a
serial login on the USB port, dropbear, and a watchdog daemon.

### Time

The clock has no working RTC and starts in 2009 on every boot. chrony 4.8 refused every
server here (`No suitable source for synchronisation`), while busybox `ntpd` works. So
`rcS` waits for DHCP and DNS, steps the time once with `ntpd -q`, then keeps `ntpd`
running.

## Making Alpine the default boot

With `boot_linux.img` in `boot_b`, the remaining step is telling the bootloader to prefer
slot B. It keeps A/B state in the `misc` partition at offset `0x800`, in the old libavb
**`\0AB0`** format (32 bytes):

| Offset | Field |
|---|---|
| 0 | magic `00 41 42 30` |
| 4 | version major 1, minor 0, 2 reserved |
| 8 | slot A: priority, tries remaining, successful, reserved |
| 12 | slot B: same |
| 16 | 12 reserved bytes |
| 28 | CRC32 (big-endian) of bytes 0–27 |

We set **A = priority 14, successful** and **B = priority 15, successful**. Marking B
"successful" matters: otherwise the bootloader counts down retries and falls back to A, and
nothing on Alpine tells it the boot succeeded. [`tools/ab_control.py`](../tools/ab_control.py)
decodes a `misc` dump and builds the block (`build --prefer b ab_slotb.bin`). Then, from the
running clock:

```sh
dd if=build/boot_linux.img of=/dev/mmcblk0p16 bs=1M conv=fsync     # boot_b
dd if=ab_slotb.bin of=/dev/mmcblk0p3 bs=1 seek=2048 count=32 conv=notrunc,fsync  # misc
```

(Partition numbers are from this unit's GPT; check yours via `PARTNAME` in
`/sys/class/block/*/uevent`.)

Note that `fastboot flash system_b` sets slot B's priority to 0 (unbootable), so write the
A/B block after flashing.

The downside of "successful": a broken kernel in `boot_b` won't fall back to Android by
itself. So test new kernels with `fastboot boot` first, and keep the last good image.

### Going back to Android

Hold volume up while powering on (fastboot), then:

```sh
fastboot set_active a
```

Slot A (`boot_a`, `system_a`, `vendor_a`) is never modified by any of this.

To come back, `fastboot set_active b` from fastboot mode. That leaves slot B with retries
counting down and not "successful", so once Alpine is up, write the A/B block above again
(or the 32 bytes saved from `misc` before switching), or the bootloader returns to Android
after a few boots.

On this unit, stock Android no longer gets past its boot animation, neither from storage nor
with `fastboot boot` of the original `boot_a` (which reached the "set up with Google Home"
screen once, right after the AVB unlock). Slot A is unchanged, so it's probably Android's own
state in `userdata`; with no adb or serial console on this build, it wasn't pursued.
