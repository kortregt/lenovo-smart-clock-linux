# 03 — Building the kernel

## Which source

Google published the kernel source. deadman96385's
[android_kernel_lenovo_mt8167s](https://github.com/deadman96385/android_kernel_lenovo_mt8167s)
mirror has three branches; the one that matches the clock is **`ivy-smart-display`**, despite
the name. It also contains the clock's panel driver and `mt8167s_som_defconfig`.

How we know: the stock kernel on `boot_a` reports

```
Linux version 4.4.95+ (android-build@abfarm-east4-003) (gcc version 4.9.x 20150123
(prerelease) (GCC) ) #1 SMP PREEMPT Tue Oct 5 18:35:18 UTC 2021
```

and the head of `ivy-smart-display` (a9e681a8, "Snap for 7792311 … oc-mr1-1.19-iot-release")
was committed at 18:13 UTC that same day. The `smart-clock` branch and Lenovo's own GPL
tarball (firmware 0.114.10, files dated March 2019) are older.

Clone it on a **case-sensitive** filesystem: the tree has files differing only in case
(`xt_TCPMSS.c` / `xt_tcpmss.c`), which collide on a default macOS volume.

## Build environment

The stock kernel was built with Google's **GCC 4.9.x 20150123**, which only exists for
x86-64 Linux. On an Apple Silicon Mac we used an OrbStack VM:

```sh
orb create --arch amd64 ubuntu:jammy clockbuild    # runs under Rosetta
# inside it:
apt-get install build-essential bc bison flex libssl-dev git python2 python3 cpio kmod \
                device-tree-compiler lz4 e2fsprogs
ln -sf /usr/bin/python2 /usr/bin/python             # the GCC wrapper script needs it
git clone --depth 1 https://github.com/LineageOS/android_prebuilts_gcc_linux-x86_aarch64_aarch64-linux-android-4.9 gcc49
git clone --depth 1 -b ivy-smart-display https://github.com/deadman96385/android_kernel_lenovo_mt8167s kernel
```

(`android.googlesource.com` was returning 503s at the time, hence the LineageOS mirror of
the toolchain.) A cold build takes about 20 minutes under Rosetta; incremental ones, a few.

## Config: use the stock one

The stock kernel embeds its own config (`CONFIG_IKCONFIG`), which is better than guessing
from a defconfig:

```sh
# extract the kernel from the stock boot image, gunzip it, then:
kernel/scripts/extract-ikconfig Image > stock.config
```

It's a **64-bit** kernel (`ARCH=arm64`), even though Lenovo's `build-kernel.sh` builds
32-bit with `mt8167s_ref_debug_defconfig`. Our additions are in
[`kernel/smartclock.config`](../kernel/smartclock.config).

```sh
cd kernel
export ARCH=arm64 CROSS_COMPILE=$HOME/build/gcc49/bin/aarch64-linux-android-
mkdir -p out && cp ../stock.config out/.config
scripts/config --file out/.config --enable DEVTMPFS ...   # see smartclock.config
make O=out olddefconfig
make O=out -j8
```

### Patches

Apply [`kernel/patches/`](../kernel/patches/) with `git apply`:

| Patch | Why |
|---|---|
| 0001 dtc `yylloc` | The bundled `dtc` fails to link with host GCC ≥ 10 (`multiple definition of yylloc`) |
| 0002 initramfs | Lets a built-in initramfs run on this bootloader ([04](04-booting-a-custom-kernel.md)) |
| 0003 dispsys debug | `dsi_bist` / `pq_bypass` debug commands used for display bring-up ([08](08-display.md)) |

## Device tree

The build appends three DTBs to `Image.gz-dtb`; the stock kernel has only one,
**`mt8167s_ref.dtb`** (byte-identical to ours). Use `Image.gz` + `mt8167s_ref.dtb`, not
`Image.gz-dtb`.

In practice the appended DTB barely matters: the bootloader loads the board's real device
tree (`mt8167s_smini`, from the `oem_bootloader` / `oem` partitions) and passes that to the
kernel.

## Making a boot image

[`tools/repack_boot.py`](../tools/repack_boot.py) takes the stock `boot_a.bin` and swaps in
a new kernel (and optionally ramdisk and command line), keeping every other header field.
Repacking the stock kernel with it reproduces `boot_a` byte for byte (apart from the AVB
footer, which isn't needed while AVB is unlocked).

```sh
python3 tools/repack_boot.py backup/parts/boot_a.bin out/arch/arm64/boot/Image.gz \
    out/arch/arm64/boot/dts/mediatek/mt8167s_ref.dtb build/boot_linux.img \
    --cmdline "bootopt=64S3,32N2,64N2 buildvariant=user keep_initramfs"
```

## Testing without flashing

```sh
fastboot boot build/boot_linux.img
```

runs the image from RAM once; nothing is written, and pulling the power returns to the
normal boot. First check: a rebuild with the **stock** config boots Android to the Google
Home setup screen exactly like stock. That proves the toolchain, config, DTB and repacking
before changing anything else.
