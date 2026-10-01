# 07 — Speaker and microphones

The sound card (`mt-snd-card`, MediaTek's `mt8516-mt7668-ref` machine driver) registers
fine, and playback runs without errors, yet nothing comes out. The reason: **neither audio
chip has a kernel driver.** On Android Things, Lenovo's app configured both over I2C from
userspace. We found this by decompiling that app, and now replay its register sequences.

## Summary

| | Speaker | Microphones |
|---|---|---|
| ALSA device | `hw:0,0` ("I2S8CH Playback") | `hw:0,1` ("TDM_Capture") |
| SoC pins | `I2S_8CH_MCK/DO1/LRCK/BCK` | `TDM_RX_MCK/BCK/LRCK/DI` |
| Chip | TI **TAS5805M** class-D amp | TI **TLV320ADC3101** stereo ADC |
| Control | i2c-2, address `0x2c` | i2c-1, address `0x1b` |
| Enable | — | "GPIO24" = Linux GPIO 411 (reset pulse) |
| Format | 48 kHz stereo (mono speaker, bridged) | `S32_LE`, 2 ch, 48 kHz |

Setup: [`rootfs-overlay/etc/clock/audio-init.sh`](../rootfs-overlay/etc/clock/audio-init.sh),
run once per boot from `rcS`, takes about 3 seconds.

## How it was found

1. **Which PCM?** The stock mixer file (`vendor/etc/mixer_paths_0.xml`) lists several
   routes. The live pin mux settled it: the sound node claims pins whose functions
   (`mt8167-pinfunc.h`) are `I2S_8CH_*` (playback, device 0) and `TDM_RX_*` (capture,
   device 1). The DMIC path (`hw:0,2`) just reads a constant rail, since nothing is
   connected there.
2. **Why silent?** `i2cdetect` shows an unclaimed chip at `2-002c`. Its registers matched
   the TAS5805M's power-on defaults (`0x03 = 0x10`: deep sleep, DSP in reset;
   `0x4c = 0x30`: 0 dB). Waking it by hand gave a pop, and it dropped back to sleep
   with a channel-1 overcurrent fault. The speaker is **bridged (PBTL)**, and in the
   default BTL mode the two halves fight. Setting `DEVICE_CTRL_1` (`0x02`) bit 2 fixed it.
3. **The full recipe.** The `oem` partition holds the board app,
   `com.google.android.things.sparrow.oem`. Decompiled with [jadx](https://github.com/skylot/jadx),
   its `Lenovo` class has `enableAmplifier()` and `enableMicrophone()`, with the complete
   register tables.
4. **The microphones.** The ADC doesn't even answer on I2C until `GPIO24` is pulsed:
   out-high, input, 20 ms, out-high. Android Things' `GPIO24` is SoC GPIO 24, i.e. Linux
   GPIO `387 + 24 = 411`. Then 46 register writes (PLL, I2S format, mic bias, filters,
   ADC power-on).

## The sequence

**Amplifier** (`enableAmplifier`):

1. Start playing silence, so the I2S clocks run (the TAS5805M needs them to leave sleep).
2. Write `AMPLIFIER_HIZ_STATE` (11 writes), wait 500 ms.
3. Write `AMP_INIT`: 1,666 writes of Lenovo's DSP tuning (EQ and limiter), PBTL mode, 0 dB,
   Play.
4. Stop the silence.

Once in Play, the amp idles in Hi-Z whenever no I2S clock is running and switches to Play by
itself when a stream starts, so this only runs once per power-on.

**Microphones** (`enableMicrophone`): the GPIO pulse, then `MIC_ADC_INIT`.

The tables live in `rootfs-overlay/etc/clock/*.regs` (one `reg value` per line) and are
written with `i2ctransfer`, 40 writes per call.

## Checks

- Amplifier state: `i2cget -y 2 0x2c 0x68` → `0x02` idle (Hi-Z), `0x03` playing.
  Faults: `0x70`–`0x72` (`0x71 = 0x04` just means "clock stopped").
- Loopback: record `hw:0,1` while playing a tone on `hw:0,0`. The level rose from about −66
  to −33 dBFS here.
- Each recording starts with a short click while the ADC settles.
- Volume is register `0x4c`: `0x30` is 0 dB, each step up is −0.5 dB.

## Volume and buttons

[`clock-volume`](../rootfs-overlay/usr/local/bin/clock-volume) sets the amp's own digital
volume (`0x4c`) as 0-100, Music Assistant's scale: 100 is 0 dB, each step down is 0.5 dB
(one amp step), 0 is mute; the buttons move 5. The level is
saved across reboots (restored by `rcS` after `audio-init`) and published in
`/run/clock/volume`; the clock face shows a volume bar for 2 seconds whenever that changes. `clock-volume up`, `down`, `set N` or `get`.

The buttons aren't input devices on this kernel; Android Things read them as GPIOs (the pins
are in the stock app's resources: `VolumeUpGpio` GPIO42, `VolumeDownGpio` GPIO576,
`MicGpio` GPIO23). [`userspace/clockkeys.c`](../userspace/clockkeys.c) watches them with
GPIO edge interrupts and runs a command per gesture from
[`/etc/clock/keys.conf`](../rootfs-overlay/etc/clock/keys.conf): `VOLUP`/`VOLDOWN` (press,
repeating while held), `BOTH`, `BOTH_LONG` (2 s), and `MIC_OFF`/`MIC_ON` for the switch on
the back (also in `/run/clock/mic`). A press waits 150 ms for the other button, so pressing
both doesn't change the volume. Volume up and down are the only actions set so far.

## A trap: the codec's headphone calibration

Reading the MT8167 codec's **"HP DC Offsets"** mixer control the first time after boot runs
a headphone DC-offset calibration that plays through the DL1 memif and then switches the
whole audio front-end off, silently stopping the speaker if it's playing (any program that
opens the ALSA mixer reads every control). `rcS` reads it at boot, before any audio. Details
in [10](10-bluetooth.md).

## Other chips the app drives

The same app also talks to a **Bosch BMA253 accelerometer** (`0-0018`, chip ID `0xfa`;
probably tap detection), an **LTR-578/LTR-390 light/proximity sensor** (`0-0053`), and GPIOs
15/16 for what look like the mic-mute and camera-switch inputs. Not wired up yet.
