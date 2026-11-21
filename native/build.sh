#!/usr/bin/env bash
# language: bash, file: native/build.sh
# Builds libvanta.so for arm64 Android without the NDK.
#
# Requires clang with an aarch64-linux-android target (Termux clang, or NDK clang with
# --target). No STL, no liblog link — see src/vanta.cpp.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-$here/../patches/src/main/resources/vanta/libvanta.so}"
api="${API:-26}"

mkdir -p "$(dirname "$out")"

clang++ \
  --target="aarch64-linux-android${api}" \
  -std=c++17 \
  -shared -fPIC \
  -Os \
  -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -nostdlib++ \
  -fvisibility=hidden -fvisibility-inlines-hidden \
  -Wall -Wextra -Wno-unused-parameter \
  -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,--build-id=none \
  -o "$out" \
  "$here/src/vanta.cpp" \
  -ldl

echo "built $out"
llvm-size "$out" 2>/dev/null || size "$out" 2>/dev/null || true
