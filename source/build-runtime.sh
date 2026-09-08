#!/bin/bash
set -euo pipefail

tdlib_source=${TDLIB_SOURCE:?Set TDLIB_SOURCE to the pinned TDLib checkout}
tdlib_build=${TDLIB_BUILD:?Set TDLIB_BUILD to its ARM64 musl build directory}
sysroot=${AERA_SYSROOT:-/tmp/aera-webkit-sysroot}
toolchain=${AERA_TOOLCHAIN:-/tmp/aera-webkit/toolchain.cmake}
strip=${AERA_STRIP:?Set AERA_STRIP to llvm-strip}
client_config=${AERA_TELEGRAM_CLIENT_CONFIG:?Set AERA_TELEGRAM_CLIENT_CONFIG to the private two-line API ID/hash file}
output=${1:?Pass the staged runtime output directory}
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
build_dir=${AERA_TELEGRAM_BUILD:-/tmp/aera-telegram-build}

test "$(git -C "$tdlib_source" rev-parse HEAD)" = \
  d1085f9cebc5a62379991ae1652673954f229c1f
test -f "$tdlib_build/libtdclient.a"
test -f "$tdlib_build/libtdcore.a"

CCACHE_DISABLE=1 cmake -S "$script_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DTDLIB_SOURCE="$tdlib_source" -DTDLIB_BUILD="$tdlib_build"
CCACHE_DISABLE=1 cmake --build "$build_dir" -j"$(nproc)"

rm -rf "$output"
mkdir -p "$output/usr/bin" "$output/lib" "$output/usr/lib" \
  "$output/etc/aera-telegram" \
  "$output/usr/share/licenses/tdlib" "$output/state"
cp "$build_dir/aera-telegram" "$output/usr/bin/aera-telegram"
cp "$sysroot/lib/ld-musl-aarch64.so.1" "$output/lib/"
cp "$sysroot/usr/lib/libssl.so.3" "$output/usr/lib/"
cp "$sysroot/usr/lib/libcrypto.so.3" "$output/usr/lib/"
cp "$sysroot/usr/lib/libstdc++.so.6.0.34" "$output/usr/lib/"
cp "$sysroot/usr/lib/libgcc_s.so.1" "$output/usr/lib/"
cp "$sysroot/usr/lib/libz.so.1.3.2" "$output/usr/lib/"
ln -s libstdc++.so.6.0.34 "$output/usr/lib/libstdc++.so.6"
ln -s libz.so.1.3.2 "$output/usr/lib/libz.so.1"
ln -s ld-musl-aarch64.so.1 "$output/lib/libc.musl-aarch64.so.1"
cp "$tdlib_source/LICENSE_1_0.txt" "$output/usr/share/licenses/tdlib/"
api_id=$(sed -n '1p' "$client_config")
api_hash=$(sed -n '2p' "$client_config")
[[ "$api_id" =~ ^[0-9]{4,10}$ ]]
[[ "$api_hash" =~ ^[0-9a-fA-F]{32}$ ]]
printf '%s\n%s\n' "$api_id" "$api_hash" > \
  "$output/etc/aera-telegram/client.conf"
chmod 0444 "$output/etc/aera-telegram/client.conf"
touch "$output/state/.keep"
"$strip" --strip-unneeded "$output/usr/bin/aera-telegram"
"$strip" --strip-unneeded "$output/usr/lib/libssl.so.3" \
  "$output/usr/lib/libcrypto.so.3" "$output/usr/lib/libstdc++.so.6.0.34" \
  "$output/usr/lib/libgcc_s.so.1" "$output/usr/lib/libz.so.1.3.2"
