# 10 — Bluetooth

**Status: works as a Bluetooth speaker (A2DP sink).** Phones pair with "Smart Clock" without a
PIN and play through the clock's speaker.

## The controller

The MT7668's Bluetooth is the chip's second SDIO function (`037a:7668`; Wi-Fi is the first).
MediaTek's driver, `btmtksdio.ko` (built with the kernel, loaded by `rcS`), uploads the
firmware patch (`mt7668_patch_e2_hdr.bin`, the same file Wi-Fi uses) and exposes the
controller as **`/dev/stpbt`**: plain H4 HCI packets, meant for Android's Bluetooth stack.
It never registers a Linux HCI device, even with Bluetooth enabled in the kernel.

The controller answers normally (HCI Reset works; version 4.2, manufacturer 0x0046
MediaTek) but has **no address** (`00:00:00:00:00:00`): Android set one at boot from a factory
partition that's blank on this unit. MediaTek's vendor command `0xfc1a` sets one.

## Relaying it to BlueZ

[`userspace/btrelay.c`](../userspace/btrelay.c) resets the controller, gives it the Wi-Fi
address + 1 (`-a` to choose one), creates a virtual controller through `/dev/vhci`, and relays
packets both ways. `/dev/vhci` takes one packet per write, while the driver can hand over
several in one read, so the relay splits them by their H4 headers. BlueZ then sees an
ordinary adapter, `hci0`.

Kernel options added for this ([`smartclock.config`](../kernel/smartclock.config)):
`CONFIG_BT`, `BT_BREDR`, `BT_LE`, `BT_RFCOMM`, `BT_BNEP`, `BT_HIDP` and `BT_HCIVHCI`. Changing
the kernel config means rebuilding the Wi-Fi and Bluetooth modules too; `rcS` loads each from
`/lib/modules`, `next/` or `prev/`, whichever matches the running kernel, so a new kernel can
be tried from RAM (`fastboot boot`) before it's flashed.

## The speaker

From `/etc/inittab`: `dbus-daemon`, `bluetoothd`, `btrelay`, **bluez-alsa** (`bluealsa -p
a2dp-sink`, receiving audio) with `bluealsa-aplay` playing it into the shared `dmix` speaker,
and `bt-agent -c NoInputNoOutput` (bluez-tools), which accepts pairing without a PIN.
[`/etc/bluetooth/main.conf`](../rootfs-overlay/etc/bluetooth/main.conf) names it "Smart Clock",
makes it a loudspeaker, always discoverable and pairable. About 15 MB of RAM, no CPU while idle.

- **The speaker freeze.** When a phone's Bluetooth audio first starts after boot, the
  speaker's playback pointer stops moving while ALSA still reports the stream running, and
  every client of the shared `dmix` blocks. Caught with a boot-time logger: it happens the
  moment `bluealsa-aplay` opens the speaker; a second 44.1 or 48 kHz client, a clock step or
  an HCI reset don't cause it, and a fresh stream always works. Root cause unknown (a lead:
  the driver's `set_i2s_slave` / pin-mux writes to the chip's PCM pins). Two services work
  around it, for every player: an `aplay` of silence keeps the stream fed, and
  [`clock-audio-watchdog`](../rootfs-overlay/usr/local/bin/clock-audio-watchdog) kills
  whatever holds the speaker when its pointer hasn't moved for 2 s (they're respawned), so
  a freeze costs a few seconds.
- Codec: SBC only (`-c -aac`); the first attempt, with AAC, hit the freeze above, so AAC may
  well work.
- **Reconnecting after a reboot** needs the device trusted: BlueZ otherwise asks the agent
  to authorize each profile connection, and `bt-agent` refuses.
  [`clock-bt-trust`](../rootfs-overlay/usr/local/bin/clock-bt-trust) trusts paired devices.
- Anyone in range can pair and play. That's the point of a speaker, but `bluetoothctl
  discoverable off` (or `pairable off`) closes it once your devices are paired.
