# 01 — Unlocking the bootloader and AVB

Two separate locks stand between you and running your own code:

1. **The bootloader lock** (`fastboot flashing unlock`): lets fastboot write partitions.
2. **AVB / verified boot** (Android Things "at-vboot"): without unlocking it, the bootloader
   refuses anything not signed by Google. Android Things devices normally need a
   per-product private key to unlock this; the Smart Clock's leaked, which is the only
   reason any of this is possible.

This follows [deadman96385's XDA guide](https://xdaforums.com/t/lenovo-smart-clock-bootloader-avb-unlock-firmware-region-changer-kernel-source.4130295/),
with notes for doing it from macOS instead of Windows.

## Getting into fastboot

You need a **USB-A to USB-A** cable (the clock's port is USB-A). Then:

1. Unplug the clock's power. Keep the USB cable connected to the computer.
2. Hold **volume up** and plug the power back in.
3. Keep holding for **20–30 seconds**. The screen stays **dark** in fastboot mode, so check
   with `fastboot devices`.

A handy trick: start the fastboot command first (`fastboot devices`, `fastboot boot …`);
it waits with `< waiting for any device >` and runs as soon as the clock appears.

## 1. Bootloader unlock

```sh
fastboot flashing unlock
```

The bootloader prints `Start unlock flow`. Afterwards `fastboot getvar unlocked` should say
`yes` (and `getvar secure` `no`).

> **Don't be misled by `getvar at-vboot-state`**: its `bootloader-locked: 1` line is an
> Android Things field and stays `1` after a successful `flashing unlock`. Use
> `getvar unlocked`.

## 2. AVB unlock

The XDA download (`Cube-AVB-factory-Unlock.zip`) contains `cube_unlock_credentials_v2.zip`
and a Windows `at_auth_unlock.exe`. That exe is a packaged copy of AOSP's
[`external/avb/tools/at_auth_unlock.py`](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/tools/at_auth_unlock.py),
so on macOS or Linux just run the script. The copy in this repo
([`at_auth_unlock.py`](../at_auth_unlock.py)) fixes a Python 3 bug in the last step.

```sh
uv run --no-project --with pycryptodome python at_auth_unlock.py -v cube_unlock_credentials_v2.zip
```

It needs `fastboot` on your PATH. `pycryptodome` stands in for the long-dead PyCrypto (same
`Crypto.*` imports). What it does:

```
fastboot oem at-get-vboot-unlock-challenge
fastboot get_staged challenge          # device's random challenge
  ... sign it with the product unlock key from the credentials zip ...
fastboot stage credential
fastboot oem at-unlock-vboot           # -> avb-locked: 0
```

then clears the factory partition's persistent digest (otherwise later changes to
`factory` would be rejected once AVB is relocked).

### The Python 3 crash

The stock script crashes in that final step with
`TypeError: a bytes-like object is required, not 'str'` (it writes a `str` into a binary
file). The unlock itself has already succeeded at that point. The fixed script encodes the
name; if you hit it with the original, run just the last step:

```sh
uv run --no-project --with pycryptodome python -c "import at_auth_unlock as a; a.ClearFactoryPersistentDigest(verbose=True)"
```

Re-running the whole script after a successful unlock fails with
`only allow when locked`, because there's no challenge to issue any more; that's expected.

## Things that don't work

- `fastboot oem append-cmdline …` is refused on production units
  (`not support on security`), so you can't add kernel parameters from fastboot. Put them in
  the boot image header instead ([04](04-booting-a-custom-kernel.md)).
- `fastboot oem device-info` is unsupported.

## Going back

- Relock AVB: `fastboot oem at-lock-vboot`
- Stock Android Things stays on slot A throughout this project; see
  [05](05-usb-and-alpine.md#going-back-to-android) for switching back.
