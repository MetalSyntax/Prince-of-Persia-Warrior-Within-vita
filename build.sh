#!/bin/bash
# Builds build/princeofpersiawarriorwithin.vpk on macOS or Linux/WSL.
#   ./build.sh            Release
#   ./build.sh Debug      adds -DDEBUG_LOADER
#   ./build.sh clean      wipes the build dir (and vitaGL objects) first
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="${TMPDIR:-/tmp}"
TMP="${TMP%/}"
# The repo path has spaces ("TOSHIBA EXT/PSVITA Develop") that vita.cmake/make choke on, and lives on
# exFAT (slow, AppleDouble ._* files): build through a symlink, into a dir on the local disk.
SRC_LINK="$TMP/pop-src"
BUILD_DIR="$TMP/pop-build"

export VITASDK="${VITASDK:-$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"
command -v arm-vita-eabi-gcc >/dev/null || { echo "arm-vita-eabi-gcc not found: set VITASDK (now $VITASDK)" >&2; exit 1; }

BUILD_TYPE=Release
case "$1" in
  clean) rm -rf "$BUILD_DIR" ;;
  Debug|Release) BUILD_TYPE="$1" ;;
  "") ;;
  *) echo "usage: $0 [Release|Debug|clean]" >&2; exit 1 ;;
esac

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"

ln -sfn "$PROJECT_DIR" "$SRC_LINK"
mkdir -p "$BUILD_DIR"
cmake -S "$SRC_LINK" -B "$BUILD_DIR" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j"$JOBS"

mkdir -p "$PROJECT_DIR/build"
for f in princeofpersiawarriorwithin.vpk eboot.bin princeofpersiawarriorwithin.velf princeofpersiawarriorwithin; do
  cp -f "$BUILD_DIR/$f" "$PROJECT_DIR/build/"
done

echo "===> Build completed: build/princeofpersiawarriorwithin.vpk ($(date '+%F %T'))"
