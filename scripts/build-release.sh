#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build_root=${BUILD_ROOT:-"$root/_build"}
kit_prefix="$build_root/platform"
jobs=${BUILD_JOBS:-2}
kit_setup=()
[[ ! -f "$build_root/kit/build.ninja" ]] || kit_setup+=(--reconfigure)
meson setup "${kit_setup[@]}" "$build_root/kit" "$root/vendor/luma-platform" --prefix="$kit_prefix" --libdir=lib --buildtype=release
ninja -C "$build_root/kit" -j"$jobs"
meson install -C "$build_root/kit"
export PKG_CONFIG_PATH="$kit_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$kit_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
write_setup=()
[[ ! -f "$build_root/write/build.ninja" ]] || write_setup+=(--reconfigure)
meson setup "${write_setup[@]}" "$build_root/write" "$root" --prefix=/usr --buildtype=release -Db_pch=false -Ddesktop=enabled -Dlibreofficekit=enabled -Ddeveloper_tools=false
ninja -C "$build_root/write" -j"$jobs"
