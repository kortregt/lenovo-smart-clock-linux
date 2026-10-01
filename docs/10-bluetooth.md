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

- **The speaker froze at the first Bluetooth playback after each boot** (solved). The
  speaker's playback pointer stopped at the end of its buffer while ALSA still reported the
  stream running, so every client of the shared `dmix` blocked. Tracing showed the audio
  interrupts stopping with no stop from the audio driver; a register monitor
  ([`kernel/debug/afewatch`](../kernel/debug/afewatch/afewatch.c), polling the AFE every
  2 ms) then caught another memif (DL1) being switched on and the whole front-end
  (`AFE_DAC_CON0` bit 0) switched off, twice, with no ALSA trigger. The culprit is the
  MT8167 codec driver's headphone DC calibration, which runs (once per channel) the first
  time anything reads its **"HP DC Offsets"** mixer control: `bluealsa-aplay` opens the ALSA
  mixer when a stream starts, and loading a mixer reads every control. The calibration plays
  through DL1 and its cleanup clears the global AFE enable regardless of what else is
  playing. Any program opening the mixer would have done it. `rcS` now reads that control
  at boot, before any audio, as Android presumably did.
  On the way, these were ruled out: a second 44.1/48 kHz client, a clock step, HCI resets,
  the driver's PCM/I2S pin setup (`skip_i2s` experiment), dmix period size, and interrupt
  latency on the single online core.
- Codecs: SBC and AAC; iPhones pick AAC (decoding it takes ~8% of a core while playing).
  Compared from an iPhone by recording the decoded stream (`bluealsa-cli open`) of a test
  signal: both flat to ~19 kHz; SBC tracks the waveform more closely (1 kHz THD+N -70 vs
  -64 dB) but adds a broadband noise floor rising in the treble, AAC keeps the space
  between tones far cleaner (multitone 63 vs 45 dB) by hiding its error under the music.
  In a blind ABX of the same music clip through the clock's speaker, 3/8: no audible
  difference here.
  `dmix` uses 100 ms periods (bluez-alsa's advice for resampled 44.1 kHz Bluetooth audio).
- **Reconnecting after a reboot** needs the device trusted: BlueZ otherwise asks the agent
  to authorize each profile connection, and `bt-agent` refuses.
  [`clock-bt-trust`](../rootfs-overlay/usr/local/bin/clock-bt-trust) trusts paired devices.
- Anyone in range can pair and play. That's the point of a speaker, but `bluetoothctl
  discoverable off` (or `pairable off`) closes it once your devices are paired.
