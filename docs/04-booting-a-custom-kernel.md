# 04 — Booting a custom kernel and initramfs

Getting our own `/init` to run took several rounds, with no serial console and a screen that
stays dark. This page covers the two bootloader quirks we hit and the tricks that found
them, which are useful for any blind bring-up.

## Debugging with no console

**Timing is a signal.** From `fastboot boot` finishing to the Lenovo logo:

| Time | Meaning |
|---|---|
| ~17 s | A normal boot (stock or ours): Android's userspace draws the logo |
| ~18 s | Our `/init` ran and immediately rebooted (a deliberate probe, below) |
| ~60–75 s | **Something hung and the hardware watchdog reset the clock.** Then the next, normal boot from `boot_a` shows the logo |

The bootloader arms the SoC watchdog; the kernel's `mtk_wdt` driver stops it when it
probes. So a ~60 s reset means the kernel died *before* that driver loaded. Pull-the-plug
tests like this cost one boot each, and the answer comes from a stopwatch.

**A reboot probe.** The smallest possible `/init` just calls `sync` and `reboot`:
[`initramfs/probe/reboot-probe.c`](../initramfs/probe/reboot-probe.c), 1 KB, no libc. If
the clock comes back in ~18 s instead of ~70 s, `/init` ran.

**A log that survives the reset.** The initramfs `/init` writes its progress plus `dmesg`
into an unused gap of `boot_b` (from 16 MiB in: the boot image ends around 12–14 MiB and
the AVB footer sits in the last MiB) after every step. After a failed boot, read it back
over the BootROM (`mtk r boot_b boot_b.bin`) and decode it with
[`tools/read_diag.py`](../tools/read_diag.py). An empty gap means `/init` never ran.

**The backlight as a status LED.** `/init` blinks `/sys/class/leds/lcd-backlight`: 2 slow
blinks when it starts, 4 fast ones when the USB gadget is up.

## Quirk 1: the bootloader always adds `skip_initramfs`

This is a system-as-root A/B build. The bootloader (`lk`) appends `skip_initramfs` (and
`root=PARTUUID=…`) to the command line on every normal boot, and the kernel then throws the
ramdisk away and boots Android straight from `system_a`. Symptom: our image "boots", but
Android's spinner appears, because our kernel ran Android.

The boot image header's own command line *is* passed through (visible in
`/proc/cmdline`), but `fastboot oem append-cmdline` is refused on production units. So the
fix is in the kernel ([patch 0002](../kernel/patches/)): a `keep_initramfs` parameter
overrides `skip_initramfs`, and a kernel with a built-in initramfs always keeps it.

## Quirk 2: touching the bootloader's ramdisk hangs the kernel

With `skip_initramfs` overridden, the kernel unpacks the ramdisk lk loaded at `0x55000000`,
then frees that memory. **That hangs the kernel early** (~70 s watchdog reset), even with a
ramdisk containing only the 1 KB probe. Kernel size isn't the cause: the same kernel with
the stock Android ramdisk boots fine when the ramdisk is skipped, and later a 20.7 MB
kernel booted without trouble.

The fix, also in patch 0002: **build the initramfs into the kernel**
(`CONFIG_INITRAMFS_SOURCE`) and **ignore the bootloader's initrd completely** (neither
unpack nor free it) whenever a built-in initramfs exists. The boot image can still carry
the stock ramdisk; it's never touched. With that, the reboot probe came back in 18 s.

## The initramfs

[`initramfs/init`](../initramfs/init) is a busybox shell script (static arm64 busybox).
[`tools/mkinitramfs.sh`](../tools/mkinitramfs.sh) writes the `gen_init_cpio` list that the
kernel build embeds.

1. Mounts `/proc`, `/sys`, devtmpfs, configfs and pstore.
2. Sets up the diagnostic log in `boot_b` (above).
3. Forces the USB controller into device mode and creates a USB gadget: serial console
   plus network ([05](05-usb-and-alpine.md)).
4. Unless the command line contains `clock.shell`, mounts `system_b` and `switch_root`s
   into the Alpine system there.
5. Otherwise, or if that fails, stays as a rescue shell: shell on the USB serial port, and
   telnet on the USB network.

`build/boot_rescue.img` is the same kernel with `clock.shell` in its header command line.
Boot it with `fastboot boot` to get the rescue shell whatever is on `system_b`.

## Other notes

- Ubuntu's static busybox is built against glibc 2.35, which calls `rseq` (syscall 293).
  This 4.4 kernel doesn't have it and dumps registers to the kernel log for each process
  (`exe[pid]: syscall 293`). It's harmless; `echo 0 > /proc/sys/debug/exception-trace`
  silences it.
- `/init` also appears as `/sbin/multi_init` (a symlink), because lk's code contains
  `rdinit=sbin/multi_init` for some boot modes.
