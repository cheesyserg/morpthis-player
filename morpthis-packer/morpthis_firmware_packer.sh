#!/bin/bash
#
# Builds a TempoTec V1 firmware image that boots Morpthis instead of the
# stock music player. This tree builds one model: the V1.
#
# Everything it needs sits next to it:
#
#   morpthis_firmware_packer.sh   this script
#   v1_original.upt            the stock firmware of the TempoTec V1
#   morpthis_player               the binary to install
#   morpthis_launch               waits for the player and reboots when it exits
#                              (optional: without it the launcher's shell does)
#   assets/
#       V1/                    the whole overlay, kernel-facing files included
#       |
#       v
#   v1.upt                     community firmware; see README before flashing
#
# No questions asked: run it and it does the lot.

set -euo pipefail
export COLUMNS=1

# --- terminal colours ---
RED='\033[1;31m'
GREEN='\033[1;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Homebrew, for macOS.
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

say()  { echo -e "$@"; }
step() { echo -e "${BLUE}==>${NC} $*"; }
warn() { echo -e "${YELLOW}!!${NC}  $*"; }
die()  { echo -e "${RED}Error:${NC} $*" >&2; exit 1; }

# ==========================================================================
# Where everything lives
# ==========================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

PLAYER_BIN="$SCRIPT_DIR/morpthis_player"
LAUNCH_BIN="$SCRIPT_DIR/morpthis_launch"

# The model this tree builds: the assets folder, the stock firmware to start
# from, the image to write, and the device-name the player reads from
# system-info.json.
#
# The device-name is not decoration -- it is checked against the file the
# overlay actually landed, so an image cannot offer another model's unsafe
# update file.
MODELS=(
	"V1|v1_original.upt|v1.upt|TempoTec V1"
)

WORK_DIR="$SCRIPT_DIR/temp"
OTA_DIR="$WORK_DIR/ota_v0"
SQUASH_DIR="$SCRIPT_DIR/squashfs-root"
MERGED_SQUASHFS="$SCRIPT_DIR/rootfs_original.squashfs"

# Paths inside the rootfs.
STOCK_PLAYER="usr/bin/hiby_player"
STOCK_LAUNCHER="usr/bin/hiby_player.sh"
NEW_LAUNCHER="usr/bin/morpthis_player.sh"
INIT_SCRIPT="etc/init.d/S92_03_start_music_player"
SYSTEM_INFO="usr/resource/morpthis/components/system-info.json"

# The stock interface's own resources. Nothing on the device reads them once
# the player is replaced, and they are several megabytes of an image that has
# to fit in flash.
#
# Note what is NOT in this list: usr/resource/morpthis, which is where the assets
# folder lands. The stock fonts and strings live one level above it and go;
# ours stay.
STOCK_UI_DIRS=(usr/resource/litegui usr/resource/layout usr/resource/str usr/resource/fonts)

# The working folders always go, whether the run succeeded or not.
cleanup() { rm -rf "$WORK_DIR" "$SQUASH_DIR" "$MERGED_SQUASHFS" 2>/dev/null || true; }
trap cleanup EXIT

say "${BLUE}###############################################${NC}"
say "${BLUE}###   MORPTHIS PLAYER -- FIRMWARE PACKER       ###${NC}"
say "${BLUE}###############################################${NC}"
say ""

# ==========================================================================
# Portability: md5, file size, build stamp
# ==========================================================================
if command -v md5sum >/dev/null 2>&1; then
	get_md5() { md5sum "$1" | awk '{print $1}'; }
else
	get_md5() { md5 -q "$1"; }
fi

if stat -c%s . >/dev/null 2>&1; then
	get_size()  { stat -c%s "$1"; }
	# When the file was last written, as DDMMYYYYHHMM. GNU date takes the file
	# itself here; the BSD one takes seconds, which is why this is not one line
	# for both.
	get_stamp() { date -r "$1" +%d%m%Y%H%M; }
else
	get_size()  { stat -f%z "$1"; }
	get_stamp() { stat -f%Sm -t %d%m%Y%H%M "$1"; }
fi

# sed -i is spelled differently on the two systems, so nothing here uses it:
# the file is rewritten through a temporary and moved into place.
sed_file() {
	local expression="$1" file="$2"
	sed -E "$expression" "$file" > "$file.packer.tmp"
	mv "$file.packer.tmp" "$file"
}

# How many times a string appears in a file -- every occurrence, not the lines
# that hold one. A single line can kill both hiby_player.sh and hiby_player,
# and counting lines would report that as one.
count_in() { tr -c '[:alnum:]_./' '\n' < "$2" | grep -c -- "$1" || true; }

# Replaces every hiby_player with morpthis_player, and proves it: the count before,
# every resulting line printed, and a hard stop if anything survived.
#
# One substitution covers both spellings -- a reference to `hiby_player` becomes
# `morpthis_player`, and one to `hiby_player.sh` becomes `morpthis_player.sh`, which
# is the name the launcher has just been given.
rename_player_in() {
	local file="$1" label="$2"
	local before
	before="$(count_in "hiby_player" "$file")"

	sed_file "s/hiby_player/morpthis_player/g" "$file"

	if grep -q "hiby_player" "$file"; then
		die "$label still mentions hiby_player after the rewrite."
	fi
	say "    $label: $before occurrence(s) of hiby_player replaced"
	grep -n "morpthis_player" "$file" | sed 's/^/        /'
}

# ==========================================================================
# What has to be there before anything is touched
# ==========================================================================
step "Checking what is needed"

# A string and not an array: bash 3.2, which is the one macOS ships, treats
# ${#array[@]} on an empty array as an unbound variable under `set -u`.
MISSING=""
for tool in 7z unsquashfs mksquashfs split tar; do
	command -v "$tool" >/dev/null 2>&1 || MISSING="$MISSING $tool"
done
# The ISO writer answers to more than one name depending on the system.
ISO_TOOL=""
for tool in mkisofs genisoimage xorrisofs; do
	if command -v "$tool" >/dev/null 2>&1; then
		ISO_TOOL="$tool"
		break
	fi
done
[ -n "$ISO_TOOL" ] || MISSING="$MISSING mkisofs"

if [ -n "$MISSING" ]; then
	die "missing:$MISSING
  Debian/Ubuntu:  sudo apt install p7zip-full squashfs-tools genisoimage
  macOS:          brew install p7zip squashfs cdrtools"
fi

[ -f "$PLAYER_BIN" ] || die "morpthis_player is not next to this script."

ASSETS_DIR="$SCRIPT_DIR/assets"
[ -d "$ASSETS_DIR" ] || die "there is no assets folder next to this script."

# Which models can be built at all. A stock firmware that is not here is not an
# error -- it is a model this person does not own.
BUILDABLE=""
for entry in "${MODELS[@]}"; do
	IFS='|' read -r m_dir m_src m_out m_name <<< "$entry"
	if [ ! -d "$ASSETS_DIR/$m_dir" ]; then
		warn "assets/$m_dir is missing: $m_name will be skipped."
		continue
	fi
	if [ ! -f "$SCRIPT_DIR/$m_src" ]; then
		warn "$m_src is not next to this script: $m_name will be skipped."
		continue
	fi
	BUILDABLE="$BUILDABLE$entry
"
	say "    $m_name: $m_src -> $m_out"
done

[ -n "$BUILDABLE" ] || die "no stock firmware to start from. Put v1_original.upt
  next to this script."

BUILD_STAMP="$(get_stamp "$PLAYER_BIN")"
TODAY="$(date +%d%m%Y)"
say "    binary:  morpthis_player, $(get_size "$PLAYER_BIN") bytes, built $BUILD_STAMP"
if [ "${BUILD_STAMP:0:8}" != "$TODAY" ]; then
	warn "morpthis_player was not built today; build_version will still say when it was."
fi

# morpthis_launch has to be a 32-bit little-endian MIPS executable: the launcher
# execs it, and a busybox shell that cannot exec it exits without starting the
# player. The first 20 bytes of the ELF header say so.
if [ -f "$LAUNCH_BIN" ]; then
	ELF_HEAD="$(od -An -tx1 -N20 "$LAUNCH_BIN" | tr -d ' \n')"
	[ "${ELF_HEAD:0:12}" = "7f454c460101" ] && [ "${ELF_HEAD:32:8}" = "02000800" ] ||
		die "morpthis_launch is not a 32-bit little-endian MIPS executable.
	  Build it with \`make target\` (or \`make morpthis_launch\`) in sonix-player."
	say "    launch:  morpthis_launch, $(get_size "$LAUNCH_BIN") bytes"
else
	warn "morpthis_launch is not next to this script: the launcher keeps its shell for the whole session."
fi
say ""

# ==========================================================================
# One image, start to finish
# ==========================================================================
# Called once per model. Everything it touches -- the work folder, the unpacked
# rootfs -- is torn down by cleanup() before the next model starts, so the two
# builds cannot see each other's files.
build_one() {
	local MODEL_DIR="$1" MODEL_NAME="$2"
	local SOURCE_UPT="$SCRIPT_DIR/$3" OUTPUT_UPT="$SCRIPT_DIR/$4"

	say "${YELLOW}###############################################${NC}"
	say "${YELLOW}###   $MODEL_NAME${NC}"
	say "${YELLOW}###############################################${NC}"
	say ""
	warn "TempoTec V1 firmware is an unofficial community build."
	warn "Keep the official stock firmware and matching checksum available for recovery before installing it."
	say ""

	# ==========================================================================
	# 1. Unpack the stock firmware
	# ==========================================================================
	step "[$MODEL_NAME] unpacking $(basename "$SOURCE_UPT")"

	rm -rf "$WORK_DIR" "$SQUASH_DIR" "$MERGED_SQUASHFS"
	mkdir -p "$WORK_DIR"

	7z x "$SOURCE_UPT" -o"$WORK_DIR" -y > /dev/null

	[ -d "$OTA_DIR" ] || die "there is no ota_v0 folder inside the .upt: not a firmware of this kind."

	say "    Joining the rootfs chunks..."
	( cd "$OTA_DIR" && cat rootfs.squashfs.* ) > "$MERGED_SQUASHFS"

	say "    Extracting the filesystem..."
	unsquashfs -f -d "$SQUASH_DIR" "$MERGED_SQUASHFS" > /dev/null
	rm -f "$MERGED_SQUASHFS"

	# The old chunks go now: what gets written back must not sit next to what it
	# replaces, or the image ends up carrying both.
	rm -f "$OTA_DIR/ota_md5_rootfs.squashfs."* "$OTA_DIR/rootfs."*

	[ -f "$SQUASH_DIR/$STOCK_LAUNCHER" ] || die "the rootfs has no $STOCK_LAUNCHER: unexpected firmware."
	[ -f "$SQUASH_DIR/$INIT_SCRIPT" ]    || die "the rootfs has no $INIT_SCRIPT: unexpected firmware."
	say ""

	# ==========================================================================
	# 2. The binary
	# ==========================================================================
	step "[$MODEL_NAME] installing morpthis_player"

	chmod 777 "$PLAYER_BIN"

	if [ -e "$SQUASH_DIR/$STOCK_PLAYER" ]; then
		rm -f "$SQUASH_DIR/$STOCK_PLAYER"
		say "    $STOCK_PLAYER removed"
	else
		warn "$STOCK_PLAYER was not there to begin with"
	fi

	cp -f "$PLAYER_BIN" "$SQUASH_DIR/usr/bin/morpthis_player"
	chmod 777 "$SQUASH_DIR/usr/bin/morpthis_player"
	say "    usr/bin/morpthis_player installed (777)"
	say ""

	# ==========================================================================
	# 3. The launcher script
	# ==========================================================================
	step "[$MODEL_NAME] renaming the launcher"

	mv "$SQUASH_DIR/$STOCK_LAUNCHER" "$SQUASH_DIR/$NEW_LAUNCHER"
	say "    $STOCK_LAUNCHER -> $NEW_LAUNCHER"
	rename_player_in "$SQUASH_DIR/$NEW_LAUNCHER" "$NEW_LAUNCHER"
	# The mode is set after the overlay, not here: the overlay may carry its own
	# copy of this file and would bring its own mode with it. See step 6.
	say ""

	# ==========================================================================
	# 4. The init script
	# ==========================================================================
	step "[$MODEL_NAME] updating $INIT_SCRIPT"

	rename_player_in "$SQUASH_DIR/$INIT_SCRIPT" "$INIT_SCRIPT"
	say ""

	# ==========================================================================
	# 5. The overlay
	# ==========================================================================
	step "[$MODEL_NAME] copying the overlay onto the root of the rootfs"

	# The V1's assets are the whole overlay: its own kernel-facing files sit in
	# it alongside the resources the player reads.
	#

	# tar rather than cp: it merges into directories that already exist,
	# overwrites the files that clash, carries the hidden ones, and behaves the
	# same way on macOS and on Linux -- none of which is true of `cp -a` on both.
	( cd "$ASSETS_DIR/$MODEL_DIR" && tar cf - . ) | ( cd "$SQUASH_DIR" && tar xf - )

	MODEL_N="$(cd "$ASSETS_DIR/$MODEL_DIR" && find . -type f | wc -l | tr -d ' ')"
	say "    $MODEL_DIR/: $MODEL_N model-specific files"

	# The streaming keys go in sealed, as streaming-keys.bin. One left in the
	# clear in the assets is not shipped: an image is unpacked by anyone who
	# downloads it. See tools/seal_streamkeys.py in sonix-player.
	PLAIN_KEYS="usr/resource/morpthis/components/streaming-keys.ini"
	if [ -f "$SQUASH_DIR/$PLAIN_KEYS" ]; then
		rm -f "$SQUASH_DIR/$PLAIN_KEYS"
		warn "$MODEL_DIR/$PLAIN_KEYS is in the clear and was left out; seal it with tools/seal_streamkeys.py"
	fi
	if [ ! -f "$SQUASH_DIR/usr/resource/morpthis/components/streaming-keys.bin" ]; then
		warn "no streaming-keys.bin in $MODEL_DIR: Tidal, Qobuz, podcasts and Last.fm stay off"
		warn "whoever installs it can still turn them on: streaming-keys.ini at the card's root is read at boot"
	fi

	# The streaming keys go in sealed, as streaming-keys.bin. One left in the
	# ==========================================================================
	# 5a. The boot script records the boot screen choice
	# ==========================================================================
	#
	# The script that draws the logo picks one of three files from one field of
	# the flash marker. Stock follows the theme in that field, and Retrospace is
	# theme:3, the value that falls through to /etc/logo.jpeg, so the stock
	# chain already draws both. What the script cannot do is say which one it
	# drew: a block of our own goes in just before the vendor's display logic,
	# reads the marker's second field, at byte 8 of the same block, and leaves
	# the trace the Developer options page shows after a boot. Inserted rather
	# than shipped whole because the script is the vendor's and stays the
	# vendor's.
	#
	# Nothing about that script is assumed -- not its name, not its display
	# command, not the shape of its branches. The script is whichever under
	# etc/init.d mentions a logo jpeg; the display command is the words the
	# script itself puts in front of a logo path; the block goes in before the
	# first line that mentions one. And everything found is written to
	# boot-script-report.txt, because a patch that silently does not apply is
	# worse than one that shouts.
	step "[$MODEL_NAME] recording the boot screen choice"

	REPORT="$SCRIPT_DIR/boot-script-report.txt"
	{
		echo "== $MODEL_NAME"
		echo "-- init scripts mentioning a logo jpeg or the marker:"
		grep -lE 'logo[0-9a-z_]*\.jpeg|0x20000' "$SQUASH_DIR"/etc/init.d/* 2>/dev/null |
			sed 's|.*/|   |' || echo "   (none)"
	} >> "$REPORT"

	BOOT_SCRIPT=""
	for cand in "$SQUASH_DIR"/etc/init.d/*; do
		[ -f "$cand" ] || continue
		if grep -qE 'logo[0-9a-z_]*\.jpeg' "$cand"; then
			BOOT_SCRIPT="$cand"
			break
		fi
	done
	if [ -z "$BOOT_SCRIPT" ]; then
		for cand in "$SQUASH_DIR"/etc/init.d/*; do
			[ -f "$cand" ] || continue
			if grep -q "0x20000" "$cand"; then
				BOOT_SCRIPT="$cand"
				break
			fi
		done
	fi

	if [ -z "$BOOT_SCRIPT" ]; then
		warn "no init script mentions a logo jpeg or the marker here."
		warn "the boot screen setting will only offer what the stock script can draw."
		echo "-- no boot script found; nothing patched." >> "$REPORT"
	elif grep -q "morpthis boot screen choice" "$BOOT_SCRIPT"; then
		say "    $(basename "$BOOT_SCRIPT") already carries the choice block"
		echo "-- $(basename "$BOOT_SCRIPT"): already carried the choice block." >> "$REPORT"
	else
		# The display command with its arguments: everything on the first line
		# carrying a logo path, from just after the last shell keyword up to
		# just before the path. "then cmd_jpeg_display /etc/logo1.jpeg" gives
		# cmd_jpeg_display; a vendor that passes flags keeps them.
		DISPLAY_CMD="$(awk '
			taken == 0 {
				for (i = 1; i <= NF; i++) {
					f = $i; gsub(/["\047]/, "", f)
					if (f ~ /logo[0-9a-z_]*\.jpeg$/) {
						cmd = ""
						for (j = i - 1; j >= 1; j--) {
							t = $j
							if (t == "then" || t == "else" || t == "do" || t == "if" ||
							    t == "elif" || t == ";" || t == "&&" || t == "||" ||
							    t ~ /=$/ || t ~ /\)$/ || t ~ /^\(/) break
							cmd = (cmd == "" ? t : t " " cmd)
						}
						if (cmd != "" && cmd != "[" && cmd != "[[" && cmd != "test") { print cmd; taken = 1; exit }
					}
				}
			}
		' "$BOOT_SCRIPT")"
		# A script that keeps the path in a variable and displays that: the same
		# walk, ending at the variable instead of at a path.
		if [ -z "$DISPLAY_CMD" ]; then
			DISPLAY_CMD="$(awk '
				taken == 0 {
					for (i = 1; i <= NF; i++) {
						f = $i; gsub(/["\047]/, "", f)
						if (f ~ /^\$[A-Za-z_]/ || f ~ /^\{?[A-Za-z_][A-Za-z0-9_]*\}$/ && seen[f]) {
							cmd = ""
							for (j = i - 1; j >= 1; j--) {
								t = $j
								if (t == "then" || t == "else" || t == "do" || t == "if" ||
								    t == "elif" || t == ";" || t == "&&" || t == "||" ||
								    t ~ /=$/ || t ~ /\)$/ || t ~ /^\(/) break
								cmd = (cmd == "" ? t : t " " cmd)
							}
							if (cmd != "" && cmd != "[" && cmd != "[[" && cmd != "test") { print cmd; taken = 1; exit }
						}
						if ($i ~ /^[A-Za-z_][A-Za-z0-9_]*=/) { v = $i; sub(/=.*/, "", v); seen[v] = 1 }
					}
				}
			' "$BOOT_SCRIPT")"
		fi
		[ -n "$DISPLAY_CMD" ] || DISPLAY_CMD="cmd_jpeg_display"

		# How the block leaves the vendor script running. The V1 display logic
		# lives in a function that, after drawing, raises the panel backlight;
		# an exit there would draw our picture onto a dark panel. So a display
		# command that is a plain name is shadowed with a no-op function once
		# ours has drawn: the vendor's own branches then run to the end,
		# backlight and all, and draw nothing over ours. A command that is a
		# path, or carries arguments, cannot be shadowed that way and exits
		# instead, keeping the picture at the cost of the vendor's tail.
		case "$DISPLAY_CMD" in
			*" "*) LEAVE="exit 0" ;;
			/*) LEAVE="exit 0" ;;
			*) LEAVE="$DISPLAY_CMD() { :; }" ;;
		esac

		# The marker is read as one aligned 64-byte read, the same call shape
		# the vendor's own read uses, and split in the shell: an unaligned -s
		# is one more thing a vendor nanddump could stumble over, and this has
		# been wrong often enough to owe us a trace. The trace file is what the
		# player's Developer options page shows after a boot.
		#
		# A custom picture is not a file the boot script can read: the rootfs
		# is read-only and /usr/data is not mounted yet. The player writes it
		# into the same erase block as the marker, one page in, and the marker
		# carries its length. The block copies it out to /tmp and hands that to
		# the vendor's own display command. A missing picture, a bad length or
		# a failed display falls through to the stock chain, so what a device
		# can lose is the choice, not the boot.
		BOOT_BLOCK="# --- morpthis boot screen choice (added by the firmware packer) ---
boot_raw=\$(nanddump -q -s 0x20000 -l 64 /dev/mtd5 -a 2>/dev/null)
boot_rest=\${boot_raw#* }
boot_sel=\${boot_rest%% *}
echo \"sel=\$boot_sel raw=\$boot_raw\" > /tmp/.bootlogo_trace 2>/dev/null
if [ \"\$boot_sel\" = \"logo:2\" ]; then
    boot_size=\$(echo \"\$boot_raw\" | sed -n 's/.*size:\\([0-9][0-9]*\\).*/\\1/p')
    if [ -n \"\$boot_size\" ] && [ \"\$boot_size\" -gt 0 ] && [ \"\$boot_size\" -le 57344 ]; then
        nanddump -q -s 0x21000 -l \"\$boot_size\" /dev/mtd5 -a > /tmp/.bootscreen.jpg 2>/dev/null
        if [ -s /tmp/.bootscreen.jpg ]; then
            $DISPLAY_CMD /tmp/.bootscreen.jpg && { echo \"drawn=custom\" >> /tmp/.bootlogo_trace; boot_drawn=1; $LEAVE; }
        fi
    fi
fi
[ -n \"\$boot_drawn\" ] || echo \"drawn=stock-chain\" >> /tmp/.bootlogo_trace 2>/dev/null
# --- end morpthis boot screen choice ---
"
		# The anchor: the line that reads the marker, a plain assignment at the
		# top of the script and a statement boundary in every shape a shell
		# script takes -- inside a case arm, which the first logo path can be,
		# an inserted if would be a syntax error that costs the whole script.
		# Failing that the first logo path; failing that the top.
		ANCHOR="$(grep -n "nanddump" "$BOOT_SCRIPT" | head -1 | cut -d: -f1)"
		[ -n "$ANCHOR" ] || ANCHOR="$(grep -nE 'logo[0-9a-z_]*\.jpeg' "$BOOT_SCRIPT" | head -1 | cut -d: -f1)"
		[ -n "$ANCHOR" ] || ANCHOR=1

		awk -v block="$BOOT_BLOCK" -v at="$ANCHOR" '
			NR == at { printf "%s", block }
			{ print }
		' "$BOOT_SCRIPT" > "$BOOT_SCRIPT.packer.tmp"

		grep -q "morpthis boot screen choice" "$BOOT_SCRIPT.packer.tmp" ||
			die "the choice block was not inserted into $(basename "$BOOT_SCRIPT")."

		# cat onto the file rather than mv over it: the init script's mode and
		# owner are the stock ones and the image boots through them.
		cat "$BOOT_SCRIPT.packer.tmp" > "$BOOT_SCRIPT"
		rm -f "$BOOT_SCRIPT.packer.tmp"
		chmod 755 "$BOOT_SCRIPT"
		say "    $(basename "$BOOT_SCRIPT"): choice block at line $ANCHOR, display command \"$DISPLAY_CMD\""

		{
			echo "-- patched $(basename "$BOOT_SCRIPT") at line $ANCHOR; display command: $DISPLAY_CMD"
			echo "-- the logo files in the image:"
			ls -l "$SQUASH_DIR"/etc/logo*.jpeg 2>/dev/null | sed 's|.*/||; s/^/   /' || echo "   (none)"
			echo "-- the script as it goes into the image:"
			sed -n '1,70p' "$BOOT_SCRIPT" | sed 's/^/   /'
		} >> "$REPORT"
	fi
	echo "" >> "$REPORT"
	say ""

	# ==========================================================================
	# 5b. The modes that decide whether any of this runs
	# ==========================================================================
	#
	# After the overlay, because the overlay wins: it may carry its own copy of
	# the launcher, and tar brings the mode along with the file.
	#
	# This is not housekeeping. The init script is
	#
	#     PL01=/usr/bin/morpthis_player.sh
	#     [ -f $PL01 ] && ( $PL01 & ) || echo "file <$PL01> don't exist."
	#
	# and `-f` asks whether the file EXISTS, not whether it can be run. A
	# launcher that arrives without its execute bit passes that test, fails to
	# start, takes the `&&` branch so the `||` message is never printed, and
	# leaves a device sitting on its boot logo with nothing in any log and no
	# reboot to hint at it. An assets folder that has been through a zip is
	# enough to cause it, which is why this is checked rather than assumed.
	step "[$MODEL_NAME] making the boot path executable"

	for rel in "$NEW_LAUNCHER" "$INIT_SCRIPT" usr/bin/bluealsa usr/bin/shairport_on.sh etc/init.d/S80_bt_init; do
		if [ -f "$SQUASH_DIR/$rel" ]; then
			chmod 755 "$SQUASH_DIR/$rel"
			say "    $rel is 755"
		fi
	done

	# Anything else the overlay put in a directory of executables, and the
	# kernel-module scripts with it.
	for rel in $(cd "$ASSETS_DIR/$MODEL_DIR" && find usr/bin module_driver -type f 2>/dev/null); do
		[ -f "$SQUASH_DIR/$rel" ] || continue
		chmod 755 "$SQUASH_DIR/$rel"
	done

	# The two that decide whether the player is ever started at all.
	for rel in "$NEW_LAUNCHER" "$INIT_SCRIPT"; do
		[ -x "$SQUASH_DIR/$rel" ] || die "$rel is not executable in the image.
	  Nothing would start the player and the device would sit on its boot logo."
	done
	say ""

	# ==========================================================================
	# 5c. morpthis_launch
	# ==========================================================================
	#
	# The launcher execs morpthis_launch just before the line that starts the
	# player, so the shell is replaced by a process of a few kB that does what
	# the rest of the script does: runs the player, then `sleep 1; reboot`. The
	# original lines stay under it and still run on a rootfs without the binary.
	if [ -f "$LAUNCH_BIN" ]; then
		step "[$MODEL_NAME] starting the player through morpthis_launch"

		cp -f "$LAUNCH_BIN" "$SQUASH_DIR/usr/bin/morpthis_launch"
		chmod 755 "$SQUASH_DIR/usr/bin/morpthis_launch"
		say "    usr/bin/morpthis_launch installed (755)"

		LAUNCHER_FILE="$SQUASH_DIR/$NEW_LAUNCHER"
		if grep -q "morpthis_launch" "$LAUNCHER_FILE"; then
			say "    $NEW_LAUNCHER already calls it"
		else
			PLAYER_LINES="$(grep -c '^/usr/bin/morpthis_player$' "$LAUNCHER_FILE" || true)"
			if [ "$PLAYER_LINES" = "1" ]; then
				awk '$0 == "/usr/bin/morpthis_player" { print "[ -x /usr/bin/morpthis_launch ] && exec /usr/bin/morpthis_launch /usr/bin/morpthis_player" } { print }' \
					"$LAUNCHER_FILE" > "$LAUNCHER_FILE.new"
				mv -f "$LAUNCHER_FILE.new" "$LAUNCHER_FILE"
				chmod 755 "$LAUNCHER_FILE"
				grep -q "exec /usr/bin/morpthis_launch" "$LAUNCHER_FILE" || die "$NEW_LAUNCHER was not rewritten."
				say "    $NEW_LAUNCHER execs morpthis_launch"
			else
				rm -f "$SQUASH_DIR/usr/bin/morpthis_launch"
				warn "$NEW_LAUNCHER has $PLAYER_LINES lines that are exactly /usr/bin/morpthis_player, not one:
	  left as it is, and morpthis_launch not installed."
			fi
		fi
		say ""
	fi

	# ==========================================================================
	# 6. The stock interface's resources
	# ==========================================================================
	step "[$MODEL_NAME] removing the stock interface's folders"

	for dir in "${STOCK_UI_DIRS[@]}"; do
		if [ -e "$SQUASH_DIR/$dir" ]; then
			rm -rf "$SQUASH_DIR/$dir"
			[ -e "$SQUASH_DIR/$dir" ] && die "could not remove $dir."
			say "    $dir removed"
		else
			say "    $dir was not there"
		fi
	done
	say ""

	# ==========================================================================
	# 6b. The stock status-LED driver
	# ==========================================================================
	#
	# The player drives the SGM31324 RGB controller directly over I2C (see
	# sonix-player/src/system/device/led_i2c.c), which needs the controller free.
	# The firmware's leds_sgm31324_add.ko holds it and on this kernel will not
	# unload, so it is never loaded at all: the module script is replaced with a
	# no-op and the .ko removed, whichever way the stock init brings it up.
	step "[$MODEL_NAME] disabling the stock LED driver"

	LED_SCRIPT="$SQUASH_DIR/module_driver/leds_sgm31324_add.sh"
	if [ -e "$LED_SCRIPT" ]; then
		printf '#!/bin/sh\n# Disabled by the firmware packer: the player drives the SGM31324 directly.\nexit 0\n' > "$LED_SCRIPT"
		chmod 755 "$LED_SCRIPT"
		say "    module_driver/leds_sgm31324_add.sh replaced with a no-op"
	else
		say "    module_driver/leds_sgm31324_add.sh was not there"
	fi

	LED_MODULE="$SQUASH_DIR/module_driver/leds_sgm31324_add.ko"
	if [ -e "$LED_MODULE" ]; then
		rm -f "$LED_MODULE"
		say "    module_driver/leds_sgm31324_add.ko removed"
	else
		say "    module_driver/leds_sgm31324_add.ko was not there"
	fi
	say ""

	# ==========================================================================
	# 7. The build stamp
	# ==========================================================================
	step "[$MODEL_NAME] system-info.json"

	JSON="$SQUASH_DIR/$SYSTEM_INFO"
	[ -f "$JSON" ] || die "$SYSTEM_INFO is not in the rootfs.
	  It should come from assets/$MODEL_DIR/$SYSTEM_INFO"

	# The name in the file against the model being built. Everything the player
	# decides by model -- the panel it opens the simulator at, the .upt it will
	# accept, the converter and the serial it prints -- hangs off this one string,
	# and an image carrying the other model's name would offer that model's update
	# to a device that cannot survive it.
	FOUND_NAME="$(tr -d '\r' < "$JSON" | tr ',' '\n' | grep -i '"device-name"' | head -1 | sed -E 's/.*"device-name"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/')"
	[ -n "$FOUND_NAME" ] || die "$SYSTEM_INFO has no device-name."
	[ "$FOUND_NAME" = "$MODEL_NAME" ] || die "$SYSTEM_INFO says device-name \"$FOUND_NAME\",
	  but this is the $MODEL_NAME image. Check assets/$MODEL_DIR/$SYSTEM_INFO."
	say "    device-name = $FOUND_NAME"

	# Whatever case the key was written in. The player reads it case-insensitively
	# (find_nocase in src/system/device/sysinfo.c), so "Build_version" and
	# "build_version" are equally correct in the file and this has to accept the
	# same. The spelling is read out first and then used literally in the
	# substitution, because sed's case-insensitive flag is a GNU extension and this
	# has to run on macOS too.
	KEY="$(grep -oE '"[Bb][Uu][Ii][Ll][Dd]_[Vv][Ee][Rr][Ss][Ii][Oo][Nn]"' "$JSON" | head -1 || true)"
	[ -n "$KEY" ] || die "$SYSTEM_INFO has no build_version key, in any case."

	sed_file "s/($KEY[[:space:]]*:[[:space:]]*\")[^\"]*(\")/\\1$BUILD_STAMP\\2/" "$JSON"

	grep -q "\"$BUILD_STAMP\"" "$JSON" || die "$KEY was not rewritten."
	say "    $KEY = $BUILD_STAMP"
	say ""

	# ==========================================================================
	# 8. Repack
	# ==========================================================================
	say "${YELLOW}###############################${NC}"
	say "${YELLOW}###   NEW FILESYSTEM        ###${NC}"
	say "${YELLOW}###############################${NC}"
	say ""

	step "[$MODEL_NAME] clearing macOS clutter"
	find "$SQUASH_DIR" -name '.DS_Store' -type f -delete 2>/dev/null || true
	find "$SQUASH_DIR" -name '._*' -type f -delete 2>/dev/null || true

	# Nothing this script writes belongs in the image.
	find "$SQUASH_DIR" -name '*.packer.tmp' -type f -delete 2>/dev/null || true

	ROOTFS_NEW="$OTA_DIR/rootfs.squashfs"

	step "[$MODEL_NAME] building the filesystem"
	mksquashfs "$SQUASH_DIR" "$ROOTFS_NEW" -comp lzo -all-root > /dev/null

	ORIGINAL_SUM="$(get_md5 "$ROOTFS_NEW")"
	SIZE="$(get_size "$ROOTFS_NEW")"
	say "    rootfs.squashfs: $SIZE bytes, md5 $ORIGINAL_SUM"

	step "[$MODEL_NAME] updating ota_update.in"
	# The kernel is not touched, so its two lines are carried over exactly as they
	# were: recomputing them from a file this script never writes would be a way of
	# getting them wrong.
	X_SIZE="$(grep -A 3 'img_name=xImage' "$OTA_DIR/ota_update.in" | grep 'img_size' | cut -d= -f2 | tr -d '\r ')"
	X_MD5="$(grep -A 3 'img_name=xImage' "$OTA_DIR/ota_update.in" | grep 'img_md5' | cut -d= -f2 | tr -d '\r ')"

	[ -n "$X_SIZE" ] && [ -n "$X_MD5" ] || die "cannot read the kernel entry from ota_update.in."

	cat > "$OTA_DIR/ota_update.in" <<-EOF
	ota_version=0

	img_type=kernel
	img_name=xImage
	img_size=$X_SIZE
	img_md5=$X_MD5

	img_type=rootfs
	img_name=rootfs.squashfs
	img_size=$SIZE
	img_md5=$ORIGINAL_SUM
	EOF

	step "[$MODEL_NAME] splitting into chunks and building the md5 chain"
	split -b 524288 -a 4 "$ROOTFS_NEW" "$OTA_DIR/temp_chunk_"
	rm -f "$ROOTFS_NEW"

	MD5_FILE="$OTA_DIR/ota_md5_rootfs.squashfs.$ORIGINAL_SUM"
	: > "$MD5_FILE"

	count=0
	CURRENT_SUM="$ORIGINAL_SUM"
	for f in "$OTA_DIR/temp_chunk_"*; do
		[ -e "$f" ] || continue
		suffix="$(printf "%04d" $count)"
		NEW_FILENAME="$OTA_DIR/rootfs.squashfs.$suffix.$CURRENT_SUM"
		mv "$f" "$NEW_FILENAME"
		CURRENT_SUM="$(get_md5 "$NEW_FILENAME")"
		echo "$CURRENT_SUM" >> "$MD5_FILE"
		count=$((count + 1))
	done
	say "    $count chunks"
	say ""

	# ==========================================================================
	# 9. The image
	# ==========================================================================
	say "${YELLOW}###############################${NC}"
	say "${YELLOW}###   FIRMWARE IMAGE        ###${NC}"
	say "${YELLOW}###############################${NC}"
	say ""

	step "[$MODEL_NAME] writing $(basename "$OUTPUT_UPT")"
	rm -f "$OUTPUT_UPT"
	"$ISO_TOOL" -o "$OUTPUT_UPT" -J -r "$WORK_DIR" > /dev/null 2>&1

	[ -f "$OUTPUT_UPT" ] || die "the image was not written."

	# TempoTec distributes a checksum text file beside v1.upt. If the workflow
	# retained the stock template, preserve its exact wording and replace every
	# MD5 in it; otherwise write the standard md5sum form. The firmware image is
	# useful on its own, but shipping the companion avoids making an official
	# update procedure silently incomplete.
	V1_SUM="$(get_md5 "$OUTPUT_UPT")"
	V1_MD5_OUT="$SCRIPT_DIR/v1_md5.txt"
	V1_MD5_TEMPLATE="$SCRIPT_DIR/v1_md5_original.txt"
	if [ -f "$V1_MD5_TEMPLATE" ] && grep -Eq '[[:xdigit:]]{32}' "$V1_MD5_TEMPLATE"; then
		sed -E "s/[[:xdigit:]]{32}/$V1_SUM/g; s/v1_original\\.upt/v1.upt/g" \
			"$V1_MD5_TEMPLATE" > "$V1_MD5_OUT"
		grep -qi "$V1_SUM" "$V1_MD5_OUT" || die "could not update the V1 checksum template."
	else
		[ ! -f "$V1_MD5_TEMPLATE" ] || warn "V1 checksum template has no MD5; writing standard syntax."
		printf '%s  v1.upt\n' "$V1_SUM" > "$V1_MD5_OUT"
	fi
	say "    v1_md5.txt: $V1_SUM"

	cleanup

	say "  $OUTPUT_UPT"
	say "  $(get_size "$OUTPUT_UPT") bytes, build_version $BUILD_STAMP"
	say ""
}

# ==========================================================================
# The model
# ==========================================================================
while IFS='|' read -r m_dir m_src m_out m_name; do
	[ -n "$m_dir" ] || continue
	build_one "$m_dir" "$m_name" "$m_src" "$m_out"
done <<< "$BUILDABLE"

say "${GREEN}#############################${NC}"
say "${GREEN}###   DONE                ###${NC}"
say "${GREEN}#############################${NC}"
say ""
say "${GREEN}  Ready to be copied to the SD card.${NC}"
say ""
