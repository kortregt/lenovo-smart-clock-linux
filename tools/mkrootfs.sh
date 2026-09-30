#!/bin/sh
# Build the Alpine root filesystem image for the clock's system_b partition (512 MiB).
# Run as root in the Linux build environment (needs qemu-aarch64 binfmt for the chroot,
# aarch64-linux-gnu-gcc and -g++, and LVGL v9.6 in $BUILD/lvgl for the clock face):
#   sudo BUILD=/path/to/build SSH_PUBKEY=/path/to/id_ed25519.pub sh tools/mkrootfs.sh
# Flash from the Mac with the clock in fastboot mode:
#   fastboot flash system_b build/system_b-alpine.img
set -eu

PROJ=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:?set BUILD to the build directory holding kernel/ and gcc49/}
SSH_PUBKEY=${SSH_PUBKEY:?set SSH_PUBKEY to the public key to authorize for root}
WORK=$BUILD/alpine
KOUT=$BUILD/kernel/out
STRIP=$BUILD/gcc49/bin/aarch64-linux-android-strip
VENDOR=$PROJ/backup/parts/vendor_a.bin
ALPINE=3.24.2
TARBALL=alpine-minirootfs-$ALPINE-aarch64.tar.gz
URL=https://dl-cdn.alpinelinux.org/alpine/v${ALPINE%.*}/releases/aarch64/$TARBALL
PACKAGES="dropbear openssh-sftp-server wpa_supplicant iw alsa-utils i2c-tools ca-certificates tzdata font-inter jq flac-libs libvorbis opusfile soxr faad2-libs libmad mpg123-libs"
TZ_NAME=${TZ_NAME:-America/New_York}
SIZE=512M
OUT=$PROJ/build/system_b-alpine.img

mkdir -p "$WORK"
cd "$WORK"
if [ ! -f "$TARBALL" ]; then
	curl -sfO "$URL"
	curl -sf "$URL.sha256" | sha256sum -c
fi
rm -rf rootfs
mkdir rootfs
tar -xzf "$TARBALL" -C rootfs

cp /etc/resolv.conf rootfs/etc/resolv.conf
chroot rootfs /bin/sh -c "apk update >/dev/null && apk add --no-progress $PACKAGES >/dev/null"
rm -f rootfs/etc/resolv.conf

cp -a "$PROJ/rootfs-overlay/." rootfs/
chown -R root:root rootfs/etc rootfs/usr/local

# Wi-Fi/BT modules from our kernel build, stripped of debug info (41 MB -> 3 MB).
mkdir -p rootfs/lib/modules rootfs/lib/firmware
for ko in misc/mediatek/connectivity/wlan/gen4-mt7668/wlan_drv_gen4_mt7668.ko \
	  misc/mediatek/connectivity/bt/mt76xx/sdio/btmtksdio.ko; do
	"$STRIP" --strip-debug -o "rootfs/lib/modules/$(basename $ko)" "$KOUT/drivers/$ko"
done

# Firmware from the stock vendor partition. FT6336U_*.bin (touch) is deliberately left out:
# the touch driver reflashes the controller when it finds a newer file.
for fw in EEPROM_MT7668.bin TxPwrLimit_MT76x8.dat WIFI_RAM_CODE2_SDIO_MT7668.bin \
	  WIFI_RAM_CODE_MT7668.bin mt7668_patch_e2_hdr.bin wifi.cfg rgx.fw.signed.22.40.54.30; do
	debugfs -R "dump /firmware/$fw rootfs/lib/firmware/$fw" "$VENDOR" 2>/dev/null
	[ -s "rootfs/lib/firmware/$fw" ] || { echo "missing firmware $fw" >&2; exit 1; }
done

# The clock face (userspace/clockface), started from inittab.
make -s -C "$PROJ/userspace/clockface" LVGL="$BUILD/lvgl" OUT="$BUILD/clockface-out"
install -m 755 "$BUILD/clockface-out/clockface" rootfs/usr/local/bin/clockface
# Backlight from the ambient light sensor, also started from inittab.
aarch64-linux-gnu-gcc -O2 -static -Wall -o rootfs/usr/local/bin/autobright \
	"$PROJ/userspace/autobright.c" -lm
# Music Assistant player (built by tools/build-squeezelite.sh).
install -m 755 "$BUILD/squeezelite" rootfs/usr/local/bin/squeezelite
# Buttons and mic switch.
aarch64-linux-gnu-gcc -O2 -static -Wall -o rootfs/usr/local/bin/clockkeys \
	"$PROJ/userspace/clockkeys.c"

mkdir -p -m 700 rootfs/root/.ssh
cp "$SSH_PUBKEY" rootfs/root/.ssh/authorized_keys
chmod 600 rootfs/root/.ssh/authorized_keys
cp "rootfs/usr/share/zoneinfo/$TZ_NAME" rootfs/etc/localtime
echo "$TZ_NAME" > rootfs/etc/timezone
mkdir -p rootfs/etc/dropbear rootfs/var/log

# ext4 without metadata_csum_seed/orphan_file-era features, for the 4.4 kernel.
rm -f "$OUT"
mke2fs -q -t ext4 -O ^metadata_csum,^metadata_csum_seed -L clockroot -d rootfs "$OUT" "$SIZE"
[ -z "${SUDO_UID:-}" ] || chown "$SUDO_UID" "$OUT"
echo "$OUT: $(du -h --apparent-size "$OUT" | cut -f1) ($(du -sh rootfs | cut -f1) used)"
