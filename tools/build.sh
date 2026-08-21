#!/usr/bin/env bash
#
# build.sh - build the gpu64/RAD firmware (kernel8.img) from Source/Firmware.
#
# What it does:
#   1. Clones the Circle bare-metal framework (pinned tag) into external/Circle
#      if it isn't there yet.
#   2. Overlays this repo's Rules.mk / sysconfig.h onto that checkout, and
#      applies the couple of Circle config tweaks gpu64 needs (see below).
#   3. Builds the Circle libraries gpu64 depends on (only once, or after a
#      toolchain/Circle upgrade -- use --libs to force a rebuild).
#   4. Copies Source/Firmware into external/Circle/app/Firmware (Circle's
#      Makefiles resolve paths like "../Rules.mk" via the *physical* cwd,
#      so a symlink there breaks the build -- rsync is used instead of cp so
#      repeat builds stay incremental) and builds kernel8.img there.
#   5. If SDCARD is set (a mounted RAD SD card's root), copies kernel8.img
#      onto it -- see docs/hw_testing.md for how to make that a one-liner.
#
# Usage:
#   tools/build.sh                      # build kernel8.img
#   tools/build.sh --libs                # also (re)build the Circle libs
#   SDCARD=/media/you/RAD tools/build.sh # build + deploy to a mounted SD card
#
# Requires a bare-metal aarch64-none-elf toolchain on PATH (NOT
# aarch64-linux-gnu-*, which pulls in glibc headers that clash with
# Circle's freestanding ones). See docs/hw_testing.md for where to get one.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CIRCLE_DIR="$REPO_ROOT/external/Circle"
CIRCLE_TAG="Step44.3"
FIRMWARE_SRC="$REPO_ROOT/Source/Firmware"
FIRMWARE_APP="$CIRCLE_DIR/app/Firmware"

PREFIX64="${PREFIX64:-aarch64-none-elf-}"
RASPPI="${RASPPI:-3}"
AARCH="${AARCH:-64}"
JOBS="${JOBS:-$(nproc)}"

BUILD_LIBS=0
for arg in "$@"; do
	case "$arg" in
		--libs) BUILD_LIBS=1 ;;
		*) echo "unknown argument: $arg" >&2; exit 1 ;;
	esac
done

MAKE_ARGS=(PREFIX64="$PREFIX64" RASPPI="$RASPPI" AARCH="$AARCH")

if ! command -v "${PREFIX64}gcc" >/dev/null 2>&1; then
	echo "error: ${PREFIX64}gcc not found on PATH." >&2
	echo "This needs a bare-metal aarch64-none-elf toolchain -- see docs/hw_testing.md." >&2
	exit 1
fi

# --- 1. fetch Circle -------------------------------------------------------
if [ ! -d "$CIRCLE_DIR" ]; then
	echo "==> cloning Circle ($CIRCLE_TAG) into external/Circle"
	git clone --branch "$CIRCLE_TAG" --depth 1 https://github.com/rsta2/circle "$CIRCLE_DIR"
	BUILD_LIBS=1
fi

# --- 2. overlay gpu64's Circle config --------------------------------------
# cmp -s first: touching these every run bumps their mtime, which would
# force a full library+firmware rebuild on every invocation even when
# nothing changed.
copy_if_changed() {
	cmp -s "$1" "$2" || cp "$1" "$2"
}
copy_if_changed "$FIRMWARE_SRC/Circle/Rules.mk" "$CIRCLE_DIR/Rules.mk"
copy_if_changed "$FIRMWARE_SRC/Circle/sysconfig.h" "$CIRCLE_DIR/include/circle/sysconfig.h"
# gpu64's helpers.cpp calls f_chdir(), which FatFs only builds with FF_FS_RPATH on.
if grep -q '^#define FF_FS_RPATH[[:space:]]\+0' "$CIRCLE_DIR/addon/fatfs/ffconf.h"; then
	sed -i 's/^#define FF_FS_RPATH\s\+0/#define FF_FS_RPATH\t\t1/' "$CIRCLE_DIR/addon/fatfs/ffconf.h"
fi

# --- 3. build the Circle libs gpu64 links against --------------------------
CIRCLE_LIBS=(lib lib/fs lib/sched lib/usb lib/input addon/SDCard addon/fatfs addon/linux)
if [ "$BUILD_LIBS" = "1" ]; then
	echo "==> building Circle libraries"
	for d in "${CIRCLE_LIBS[@]}"; do
		make -C "$CIRCLE_DIR/$d" "${MAKE_ARGS[@]}" -j"$JOBS"
	done
else
	for d in "${CIRCLE_LIBS[@]}"; do
		if ! compgen -G "$CIRCLE_DIR/$d"/*.a >/dev/null; then
			echo "==> $d has no built library yet, building Circle libraries"
			for d2 in "${CIRCLE_LIBS[@]}"; do
				make -C "$CIRCLE_DIR/$d2" "${MAKE_ARGS[@]}" -j"$JOBS"
			done
			break
		fi
	done
fi

# --- 4. build the firmware --------------------------------------------------
echo "==> syncing Source/Firmware -> external/Circle/app/Firmware"
mkdir -p "$CIRCLE_DIR/app"
# --delete keeps the two trees in sync, but must not sweep away build
# artifacts that only ever exist on the app/Firmware side (Circle's build
# rules put .o/.d/.elf/... next to the sources) -- that would force a full
# rebuild on every single invocation.
rsync -a --delete \
	--exclude 'Circle/' \
	--exclude '*.o' --exclude '*.d' --exclude '*.a' --exclude '*.elf' \
	--exclude '*.lst' --exclude '*.img' --exclude '*.hex' --exclude '*.map' \
	"$FIRMWARE_SRC/" "$FIRMWARE_APP/"

echo "==> building kernel8.img"
make -C "$FIRMWARE_APP" "${MAKE_ARGS[@]}" -j"$JOBS"

KERNEL_IMG="$FIRMWARE_APP/kernel8.img"
echo "==> built $KERNEL_IMG"

# --- 5. optional deploy ------------------------------------------------------
if [ -n "${SDCARD:-}" ]; then
	if [ ! -d "$SDCARD" ]; then
		echo "error: SDCARD=$SDCARD is not a directory" >&2
		exit 1
	fi
	cp "$KERNEL_IMG" "$SDCARD/kernel8.img"
	sync
	echo "==> deployed to $SDCARD/kernel8.img"
fi
