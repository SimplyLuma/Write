#!/usr/bin/env bash
set -euo pipefail

if [[ ! -r /etc/os-release ]]; then
  echo "This report is intended for Fedora 44 and /etc/os-release is unavailable." >&2
  exit 2
fi

# shellcheck disable=SC1091
source /etc/os-release

echo "Recorded (UTC): $(date --utc --iso-8601=seconds)"
echo "Operating system: ${PRETTY_NAME:-unknown}"
echo "Kernel: $(uname -srmo)"
echo "Desktop: ${XDG_CURRENT_DESKTOP:-unknown}"
echo "Session type: ${XDG_SESSION_TYPE:-unknown}"
echo "Display: ${WAYLAND_DISPLAY:-${DISPLAY:-unknown}}"
echo

echo "Toolchain"
meson --version
ninja --version
c++ --version
pkg-config --version
echo

echo "Pinned dependency NVRAs"
rpm -q \
  gtk4 gtk4-devel \
  glib2 glib2-devel \
  fontconfig fontconfig-devel \
  cairo cairo-devel \
  libreofficekit libreofficekit-devel \
  libreoffice-core libreoffice-writer \
  dbus-daemon xorg-x11-server-Xvfb xdotool localsearch \
  python3-gobject at-spi2-core
echo

echo "CPU"
lscpu
echo

echo "Memory"
if command -v free >/dev/null 2>&1; then
  free --bytes
else
  sed -n '/^MemTotal:/p; /^MemAvailable:/p; /^SwapTotal:/p' /proc/meminfo
fi
