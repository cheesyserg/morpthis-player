#!/bin/sh
# Downloads the pinned TempoTec V1 kernel (v1-zram-xImage) next to the packer
# and checks it against kernel/tempotec_v1.json.
#
#   fetch_v1_kernel.sh [DEST_DIR]      (default: this folder)
#
# The kernel is Compás's V1 refresh: Linux 4.4.94+ rebuilt from the Ingenic
# X1600 SDK with the hiby-custom-kernel patches, compressed RAM swap (zram,
# LZ4) added and the stock device tree kept byte for byte. It is published as
# the v1-zram-xImage asset of the build-inputs release of
# Starnished66/compas-firmware; the pin records its source commit, its
# configuration and every hash. V1_KERNEL_URL overrides where it comes from;
# the hash check applies whatever the source.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${1:-$HERE}
URL=${V1_KERNEL_URL:-https://github.com/Starnished66/compas-firmware/releases/download/build-inputs/v1-zram-xImage}
PIN="$HERE/kernel/tempotec_v1.json"
WANT=$(sed -n 's/.*"sha256": *"\([0-9a-f]\{64\}\)".*/\1/p' "$PIN" | head -1)
[ -n "$WANT" ] || { echo "fetch_v1_kernel: no sha256 in $PIN" >&2; exit 1; }

mkdir -p "$DEST"
OUT="$DEST/v1-zram-xImage"
if [ -f "$OUT" ]; then
	HAVE=$( (sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT") | awk '{print $1}')
	if [ "$HAVE" = "$WANT" ]; then
		echo "v1-zram-xImage already here and verified ($WANT)"
		exit 0
	fi
fi

curl -fsSL --retry 4 -o "$OUT.part" "$URL"
HAVE=$( (sha256sum "$OUT.part" 2>/dev/null || shasum -a 256 "$OUT.part") | awk '{print $1}')
if [ "$HAVE" != "$WANT" ]; then
	rm -f "$OUT.part"
	echo "fetch_v1_kernel: downloaded kernel has SHA-256 $HAVE, the pin says $WANT" >&2
	exit 1
fi
mv "$OUT.part" "$OUT"
echo "v1-zram-xImage downloaded and verified ($WANT)"
