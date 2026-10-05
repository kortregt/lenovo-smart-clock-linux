# 02 — Full backup over the BootROM (mtkclient)

Before changing anything, take a full image of the eMMC. MediaTek SoCs have a BootROM
download mode that works even if everything on the eMMC is broken, so with a backup the
clock is very hard to brick permanently.

[mtkclient](https://github.com/bkerler/mtkclient) talks to that mode. On the MT8167 the
BootROM exploit ("kamakiri") works; the SoC has secure boot and DAA enabled but no SLA.

## Entering BootROM mode

<!-- TODO: exact button combination used on this unit -->
Power the clock off completely (unplug power **and** USB, wait a few seconds), then connect
it in BootROM mode. mtkclient waits for the device and runs the exploit automatically.

## macOS setup notes

mtkclient is installed in a `uv` project pinned to Python 3.12 (see
[`pyproject.toml`](../pyproject.toml)). Things that needed fixing on an Apple Silicon Mac:

- `keystone-engine`, `capstone` and `unicorn` aren't declared by mtkclient; add them. Don't
  install the PyPI package called `keystone` (that's OpenStack).
- `keystone-engine` can't find its own library: `brew install keystone` and set
  `LIBKEYSTONE_PATH=/opt/homebrew/lib`.
- mtkclient imports `mfusepy`, which needs a libfuse: with fuse-t installed, set
  `FUSE_LIBRARY_PATH=/usr/local/lib/libfuse-t.dylib` (`libfuse3.dylib` crashes).
- `USB CORE ERROR [Errno 13] Access denied` means the macOS serial driver grabbed the
  device: run mtkclient as root, preserving the environment:

```sh
set -a; source .env; set +a      # LIBKEYSTONE_PATH, FUSE_LIBRARY_PATH
sudo --preserve-env=LIBKEYSTONE_PATH,FUSE_LIBRARY_PATH .venv/bin/mtk printgpt
```

## What to save

```sh
mtk printgpt > gpt.txt                     # partition table
mtk rf full_user.bin                       # whole user area (~7.3 GiB, ~40 min)
mtk r preloader boot1.bin --parttype boot1   # preloader (eMMC boot area 1)
mtk r preloader boot2.bin --parttype boot2   # eMMC boot area 2 (empty on this unit)
```

The image is mostly empty space and compresses to under a tenth of its size (7.3 GiB to
about 0.7 GiB here). To keep it compressed, check it against the original's checksum, then
remove the original:

```sh
zstd -T0 -10 --long=27 full_user.bin -o full_user.bin.zst
zstd -dc --long=27 full_user.bin.zst | shasum -a 256     # must match full_user.bin's
zstd -d --long=27 full_user.bin.zst                      # back to full_user.bin, to restore
```

Then split the full image into partitions and sanity-check each one:

```sh
uv run python tools/split_dump.py backup/full_user.bin backup/gpt.txt backup/parts
```

[`tools/split_dump.py`](../tools/split_dump.py) writes one `<name>.bin` per partition plus
`SHA256SUMS`, and checks the formats it knows (GPT, Android boot images, vbmeta, lk, ext4).

The irreplaceable pieces are the full image, `boot1`/`boot2` (preloader) and `seccfg`.
Everything else can be rebuilt or re-downloaded, but keep them anyway: later steps read
`boot_a`, `vendor_a` and `oem_a` from the backup.

## Findings

- A/B layout: `lk`, `tee`, `boot`, `system`, `vbmeta`, `vendor`, `oem` and
  `oem_bootloader` all have `_a` and `_b` copies. There's no recovery partition.
- `nvram` is all zeros **on the device** (the neighbouring partitions parse fine). The
  Android Things build simply doesn't use MediaTek's nvram, so don't chase it.
- `factory` is a nearly empty ext4 (label `factory_iot`). Device config lives there.
- `seccfg` v4 is locked (`lock_state=1`). `mtk da seccfg unlock` can rewrite it, but it
  isn't needed: the fastboot unlock in [01](01-unlocking.md) is what the bootloader honours.
- Reading a single partition later is quick, e.g. `mtk r boot_b boot_b.bin`; this was used
  as a debug channel in [04](04-booting-a-custom-kernel.md).
