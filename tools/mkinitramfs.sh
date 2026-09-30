#!/bin/sh
# Pack initramfs/init + a static arm64 busybox into a gzipped newc cpio.
# Also writes the gen_init_cpio list to build/initramfs.list, which the kernel embeds
# via CONFIG_INITRAMFS_SOURCE (the lk may not hand over the boot image ramdisk).
# Run in the Linux build environment (needs the kernel's usr/gen_init_cpio from a build):
#   sh tools/mkinitramfs.sh            (BUILD defaults to ~/build; see docs/03-kernel.md)
set -eu

PROJ=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$HOME/build}
BUSYBOX=${BUSYBOX:-$BUILD/initramfs-src/bb/bin/busybox}   # static arm64 busybox
GEN=${GEN:-$BUILD/kernel/out/usr/gen_init_cpio}
OUT=${OUT:-$PROJ/build/initramfs.cpio.gz}
# Optional: an extra static binary from $BUILD to install as /bin/$EXTRA (debug probes).
EXTRA=${EXTRA:-}

list=${LIST:-$PROJ/build/initramfs.list}
cat > "$list" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
dir /bin 0755 0 0
file /bin/busybox $BUSYBOX 0755 0 0
slink /bin/sh busybox 0777 0 0
file /init $PROJ/${INIT:-initramfs/init} 0755 0 0
dir /sbin 0755 0 0
slink /sbin/multi_init /init 0777 0 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /tmp 1777 0 0
dir /run 0755 0 0
EOF
[ -z "$EXTRA" ] || echo "file /bin/$EXTRA $BUILD/$EXTRA 0755 0 0" >> "$list"

"$GEN" "$list" | gzip -9n > "$OUT"
echo "$OUT: $(wc -c < "$OUT") bytes"
