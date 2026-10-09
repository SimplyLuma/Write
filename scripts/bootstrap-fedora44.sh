#!/usr/bin/env bash
set -euo pipefail

script_path="$(realpath "${BASH_SOURCE[0]}")"

if [[ "$(id -u)" -ne 0 ]]; then
  if ! command -v sudo >/dev/null 2>&1; then
    echo "bootstrap-fedora44.sh needs root privileges and sudo is unavailable." >&2
    exit 1
  fi
  exec sudo -- "${script_path}" "$@"
fi

if [[ ! -r /etc/os-release ]]; then
  echo "Cannot verify Fedora 44: /etc/os-release is unavailable." >&2
  exit 2
fi

# shellcheck disable=SC1091
source /etc/os-release
if [[ "${ID:-}" != "fedora" || "${VERSION_ID:-}" != "44" ]]; then
  echo "This bootstrap is intentionally pinned to Fedora 44; found ${PRETTY_NAME:-unknown}." >&2
  echo "Build on a native Fedora 44 builder." >&2
  exit 2
fi

dnf -y --refresh --setopt=install_weak_deps=False install \
  meson \
  ninja-build \
  gcc-c++ \
  pkgconf-pkg-config \
  gtk4-devel \
  glib2-devel \
  libadwaita-devel \
  wayland-devel \
  json-glib-devel \
  libxml2-devel \
  fontconfig-devel \
  cairo-devel \
  libreofficekit-devel \
  libreoffice-core \
  libreoffice-writer \
  dbus-daemon \
  xorg-x11-server-Xvfb \
  xdotool \
  localsearch \
  python3-gobject \
  at-spi2-core \
  desktop-file-utils \
  appstream \
  rpm-build \
  rpmdevtools \
  rpmlint \
  git-core \
  which

echo "Fedora 44 dependencies installed. Installed engine/UI/smoke versions:"
rpm -q \
  gtk4-devel glib2-devel \
  libreofficekit-devel libreoffice-core libreoffice-writer \
  dbus-daemon xorg-x11-server-Xvfb xdotool localsearch \
  python3-gobject at-spi2-core
