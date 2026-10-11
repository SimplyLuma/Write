#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
: "${DEV_ARTIFACT_DIR:?Run with devctl run so outputs belong to the task}"
meson setup "$DEV_ARTIFACT_DIR/core" -Ddesktop=disabled -Dlibreofficekit=disabled
meson compile -C "$DEV_ARTIFACT_DIR/core" -j2
meson test -C "$DEV_ARTIFACT_DIR/core" --suite unit --print-errorlogs
# This exercises Android's discovery branch on the host; it is not an NDK build.
clang++ -std=c++20 -Wall -Wextra -Werror -D__ANDROID__ -Isrc tests/office_locations_test.cpp -o "$DEV_ARTIFACT_DIR/android-discovery"
"$DEV_ARTIFACT_DIR/android-discovery"
