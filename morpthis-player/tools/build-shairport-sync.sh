#!/bin/sh
#
# Builds shairport-sync for the player's MIPS toolchain, classic AirPlay only,
# with the pipe output back end.
#
#   ./tools/build-shairport-sync.sh            build into airplay-target/
#   ./tools/build-shairport-sync.sh --clean    remove the build tree first
#
# The result is a stripped binary at
#
#   morpthis-packer/shairport-sync
#
# next to morpthis_player, where morpthis_firmware_packer.sh installs it as
# /usr/bin/shairport-sync. Without that binary the packer keeps the stock
# shairport and the patched shairport_on.sh falls back to it.
#
# What is built, and why:
#
#   popt, libconfig   required by shairport-sync's configuration handling.
#                     Static, because neither is on the device.
#   OpenSSL 1.1.1     the RAOP handshake. Static, because the device's
#                     libcrypto is the same 1.1.1 series but this avoids
#                     depending on its exact patch level.
#   shairport-sync    --with-pipe, S16_LE 44100 stereo into a FIFO; the
#                     metadata pipe; tinysvcmdns so no Avahi daemon is
#                     needed. No FFmpeg, no soxr, no ALSA, no AirPlay 2:
#                     classic AirPlay is ALAC/S16/44100 and the player owns
#                     the DAC.
#
# The ALAC decoder is the bundled Hammerton one (--with-hammerton). It is
# deprecated upstream for security reasons, but this device only decodes
# streams from a phone on the local network, and it is the only decoder that
# needs no extra library. Swap to --with-apple-alac and a libalac prefix if
# that trade ever changes.
#
# Network access is needed the first time, to fetch the three tarballs. They
# are cached under airplay-src/.

set -eu

HERE=$(cd "$(dirname "$0")" >/dev/null 2>&1 && pwd)
PLAYER_ROOT=$(cd "$HERE/.." >/dev/null 2>&1 && pwd)
PACKER_DIR=${PACKER_DIR:-$PLAYER_ROOT/../morpthis-packer}
SHAIRPORT_SRC=${SHAIRPORT_SRC:-$PLAYER_ROOT/../../shairport-sync}

BUILD_DIR=${BUILD_DIR:-$HERE/airplay-target}
SRC_DIR=${SRC_DIR:-$HERE/airplay-src}
PREFIX=$BUILD_DIR/prefix

CROSS_CC=${CROSS_CC:-$PLAYER_ROOT/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc}
CROSS_STRIP=${CROSS_STRIP:-$PLAYER_ROOT/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-strip}
CROSS_AR=${CROSS_AR:-$PLAYER_ROOT/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-ar}
CROSS_RANLIB=${CROSS_RANLIB:-$PLAYER_ROOT/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-ranlib}

POPT_VER=1.19
LIBCONFIG_VER=1.7.3
OPENSSL_VER=1.1.1w

die() { echo "shairport-sync: $*" >&2; exit 1; }
say() { echo "==> $*"; }

if [ "${1:-}" = "--clean" ]; then
	rm -rf "$BUILD_DIR" "$SRC_DIR"
	say "cleaned"
	exit 0
fi

[ -x "$CROSS_CC" ] || die "no cross compiler at $CROSS_CC (set CROSS_CC)"
[ -d "$SHAIRPORT_SRC" ] || die "shairport-sync source not found at $SHAIRPORT_SRC (set SHAIRPORT_SRC)"

TRIPLET=$(basename "$CROSS_CC")
TRIPLET=${TRIPLET%-gcc}
[ -n "$TRIPLET" ] || die "cannot derive the toolchain triplet from $CROSS_CC"

mkdir -p "$BUILD_DIR" "$SRC_DIR" "$PREFIX"

fetch() {
	url=$1
	file=$2
	if [ ! -f "$SRC_DIR/$file" ]; then
		say "fetching $file"
		if command -v curl >/dev/null 2>&1; then
			curl -fL "$url" -o "$SRC_DIR/$file"
		elif command -v wget >/dev/null 2>&1; then
			wget -O "$SRC_DIR/$file" "$url"
		else
			die "need curl or wget to fetch $url"
		fi
	fi
}

untar() {
	file=$1
	dir=$2
	if [ ! -d "$BUILD_DIR/$dir" ]; then
		say "unpacking $file"
		tar -C "$BUILD_DIR" -xf "$SRC_DIR/$file"
	fi
}

build_popt() {
	untar "popt-$POPT_VER.tar.gz" "popt-$POPT_VER"
	say "building popt $POPT_VER"
	(
		cd "$BUILD_DIR/popt-$POPT_VER"
		./configure --host="$TRIPLET" --prefix="$PREFIX" \
			--disable-shared --enable-static --disable-nls \
			CC="$CROSS_CC" AR="$CROSS_AR" RANLIB="$CROSS_RANLIB"
		make -j"${JOBS:-2}"
		make install
	)
}

build_libconfig() {
	untar "libconfig-$LIBCONFIG_VER.tar.gz" "libconfig-$LIBCONFIG_VER"
	say "building libconfig $LIBCONFIG_VER"
	(
		cd "$BUILD_DIR/libconfig-$LIBCONFIG_VER"
		./configure --host="$TRIPLET" --prefix="$PREFIX" \
			--disable-shared --enable-static --disable-examples --disable-tests \
			CC="$CROSS_CC" AR="$CROSS_AR" RANLIB="$CROSS_RANLIB"
		make -j"${JOBS:-2}"
		make install
	)
}

build_openssl() {
	untar "openssl-$OPENSSL_VER.tar.gz" "openssl-$OPENSSL_VER"
	say "building OpenSSL $OPENSSL_VER"
	(
		cd "$BUILD_DIR/openssl-$OPENSSL_VER"
		# no-asm: the target's assembly is not worth the toolchain risk, and
		# this is a handshake and an AES stream at 44.1 kHz, not a benchmark.
		# -DL_ENDIAN: the device is mipsel, and the generic linux-mips32
		# target otherwise leaves the endianness to the compiler defaults.
		./Configure linux-mips32 -DL_ENDIAN no-shared no-tests no-asm \
			--prefix="$PREFIX" --openssldir="$PREFIX/ssl" \
			CC="$CROSS_CC" AR="$CROSS_AR" RANLIB="$CROSS_RANLIB"
		make -j"${JOBS:-2}"
		make install_sw
	)
}

fetch "https://ftp.rpm.org/popt/releases/popt-1.x/popt-$POPT_VER.tar.gz" "popt-$POPT_VER.tar.gz"
fetch "https://github.com/hyperrealm/libconfig/releases/download/v$LIBCONFIG_VER/libconfig-$LIBCONFIG_VER.tar.gz" "libconfig-$LIBCONFIG_VER.tar.gz"
fetch "https://www.openssl.org/source/old/1.1.1/openssl-$OPENSSL_VER.tar.gz" "openssl-$OPENSSL_VER.tar.gz"

[ -f "$PREFIX/lib/libpopt.a" ] || build_popt
[ -f "$PREFIX/lib/libconfig.a" ] || build_libconfig
[ -f "$PREFIX/lib/libssl.a" ] || build_openssl

say "building shairport-sync (classic AirPlay, pipe backend)"
# Built from a copy: autoreconf and make would otherwise leave generated files
# and objects all over the checkout.
SHAIRPORT_BUILD=$BUILD_DIR/shairport-sync-src
if [ ! -d "$SHAIRPORT_BUILD" ]; then
	say "copying the source into $SHAIRPORT_BUILD"
	cp -a "$SHAIRPORT_SRC" "$SHAIRPORT_BUILD"
fi

# Static by default: popt and libconfig are not on the device, and a static
# link also removes any question of which OpenSSL patch level is present.
# STATIC=0 links them dynamically instead, which needs the .so files shipped.
LINK_FLAGS="-L$PREFIX/lib"
if [ "${STATIC:-1}" = "1" ]; then
	LINK_FLAGS="-static $LINK_FLAGS"
fi
(
	cd "$SHAIRPORT_BUILD"
	[ -f configure ] || autoreconf -fi
	PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
	PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
	./configure --host="$TRIPLET" --prefix="$PREFIX" \
		--with-pipe \
		--with-tinysvcmdns \
		--with-ssl=openssl \
		--with-metadata \
		--with-metadata-pipe \
		--with-hammerton \
		--without-ffmpeg \
		--without-soxr \
		--without-avahi \
		--without-alsa \
		--without-pipewire \
		--without-jack \
		--without-pa \
		--without-dbus-interface \
		--without-mpris-interface \
		--without-mqtt-client \
		--without-systemd-startup \
		--without-systemv-startup \
		--without-configfiles \
		--without-create-user-group \
		CC="$CROSS_CC" AR="$CROSS_AR" RANLIB="$CROSS_RANLIB" \
		CFLAGS="-O2 -I$PREFIX/include" \
		LDFLAGS="$LINK_FLAGS"
	make -j"${JOBS:-2}"
)

[ -x "$SHAIRPORT_BUILD/shairport-sync" ] || die "shairport-sync did not build"

mkdir -p "$PACKER_DIR"
"$CROSS_STRIP" -o "$PACKER_DIR/shairport-sync" "$SHAIRPORT_BUILD/shairport-sync"
chmod 755 "$PACKER_DIR/shairport-sync"

say "installed $PACKER_DIR/shairport-sync"
"${CROSS_OBJDUMP:-$PLAYER_ROOT/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-objdump}" \
	-f "$PACKER_DIR/shairport-sync" 2>/dev/null | sed -n '1,8p' || true
