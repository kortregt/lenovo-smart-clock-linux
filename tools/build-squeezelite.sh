#!/bin/sh
# Build squeezelite (the Music Assistant / Lyrion player) for the clock. Alpine 3.24 doesn't
# package it, so it's compiled in an Alpine aarch64 chroot (qemu-user binfmt), linking the
# same shared libraries the clock installs with apk (see PACKAGES in mkrootfs.sh).
# Patched so Music Assistant's volume drives the amp (patches/squeezelite-volume-hook.patch).
# Run in the Linux build environment:
#   BUILD=/path/to/build sh tools/build-squeezelite.sh   -> $BUILD/squeezelite
set -eu
BUILD=${BUILD:?set BUILD to the build directory}
COMMIT=c7c4248			# ralph-irving/squeezelite, revision 1595
D=$BUILD/alpine-dev
TARBALL=$BUILD/alpine/alpine-minirootfs-3.24.2-aarch64.tar.gz	# fetched by mkrootfs.sh

if [ ! -d "$D" ]; then
	sudo mkdir -p "$D"
	sudo tar -xzf "$TARBALL" -C "$D"
	sudo mknod -m 666 "$D/dev/null" c 1 3
	sudo mknod -m 666 "$D/dev/urandom" c 1 9
fi
sudo cp /etc/resolv.conf "$D/etc/resolv.conf"
sudo cp "$(dirname "$0")/patches/squeezelite-volume-hook.patch" "$D/tmp/volume-hook.patch"
sudo chroot "$D" /bin/sh -eu -c "
	apk add -q build-base git alsa-lib-dev flac-dev mpg123-dev libvorbis-dev faad2-dev \
		soxr-dev opusfile-dev libmad-dev
	cd /root
	[ -d squeezelite ] || git clone -q https://github.com/ralph-irving/squeezelite.git
	cd squeezelite
	git checkout -q $COMMIT
	git checkout -q .
	patch -p1 < /tmp/volume-hook.patch
	make -s clean
	make -s -j8 OPTS='-DLINKALL -DRESAMPLE -DOPUS -I/usr/include/opus'
"
cp "$D/root/squeezelite/squeezelite" "$BUILD/squeezelite"
echo "$BUILD/squeezelite"
