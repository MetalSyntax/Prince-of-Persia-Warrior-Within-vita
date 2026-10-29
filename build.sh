#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="/tmp/pop-build"
SRC_LINK="/tmp/pop-src"

export VITASDK="${VITASDK:-/home/metalsyntax/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

ln -sfn "$PROJECT_DIR" "$SRC_LINK"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SRC_LINK" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"

mkdir -p "$PROJECT_DIR/build"
cp -f "$BUILD_DIR/princeofpersiawarriorwithin.vpk" "$PROJECT_DIR/build/"
cp -f "$BUILD_DIR/eboot.bin" "$PROJECT_DIR/build/"
cp -f "$BUILD_DIR/princeofpersiawarriorwithin.velf" "$PROJECT_DIR/build/"
cp -f "$BUILD_DIR/princeofpersiawarriorwithin" "$PROJECT_DIR/build/"

echo "===> Build completed successfully: build/princeofpersiawarriorwithin.vpk"
