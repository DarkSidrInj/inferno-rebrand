#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK_HOME:-$ROOT/../../android-ndk-r30}"
NDK_BUILD="$NDK/build/ndk-build"
[[ -x "$NDK_BUILD" ]] || { echo "NDK build tool not found: $NDK_BUILD" >&2; exit 1; }

"$NDK_BUILD" \
  NDK_PROJECT_PATH="$ROOT" \
  APP_BUILD_SCRIPT="$ROOT/jni/Android.mk" \
  NDK_APPLICATION_MK="$ROOT/jni/Application.mk" \
  NDK_OUT="$ROOT/build/obj" \
  NDK_LIBS_OUT="$ROOT/build/libs" \
  -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

mkdir -p "$ROOT/out/arm64-v8a"
cp -f "$ROOT/build/libs/arm64-v8a/bycmd" "$ROOT/out/arm64-v8a/bycmd-1.0.0"
echo "Built: $ROOT/out/arm64-v8a/bycmd-1.0.0"
