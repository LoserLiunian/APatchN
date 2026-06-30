#!/usr/bin/env bash
# Standalone build for apd (arm64-v8a) via the Android NDK + Ninja.
# Gradle normally drives this; this script is for local/dev iteration.
#
#   ANDROID_NDK_HOME=/path/to/ndk NINJA=/path/to/ninja ./build.sh [Debug|Release]
#
# Produces build-cmake/apd (an arm64 ELF). Copy to app/libs/arm64-v8a/libapd.so.
set -euo pipefail
cd "$(dirname "$0")"

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:-D:/Android/Sdk/ndk/29.0.14206865}}"
NINJA="${NINJA:-ninja}"
BUILD_TYPE="${1:-Release}"

cmake -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -S . -B build-cmake

cmake --build build-cmake
echo "==> build-cmake/apd"
