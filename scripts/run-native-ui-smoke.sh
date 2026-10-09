#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${project_root}/_build/write/src/luma-write}"
artifact_root="${2:-$(dirname "$(dirname "${binary}")")/meson-logs/native-ui-smoke}"
driver="${project_root}/tests/native_ui_smoke.py"

if [[ ! -x "${binary}" ]]; then
  echo "Native UI smoke binary is not executable: ${binary}" >&2
  exit 2
fi

for required_command in dbus-run-session Xvfb xdotool python3 gsettings; do
  if ! command -v "${required_command}" >/dev/null 2>&1; then
    echo "Native UI smoke requires '${required_command}'." >&2
    exit 77
  fi
done

if ! python3 -c 'import gi; gi.require_version("Atspi", "2.0"); from gi.repository import Atspi' \
    >/dev/null 2>&1; then
  echo "Native UI smoke requires Python GObject bindings and the Atspi 2 typelib." >&2
  exit 77
fi

mkdir -p "${artifact_root}"
run_artifacts="${artifact_root}/run-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "${run_artifacts}"
session_root="$(mktemp -d "${TMPDIR:-/tmp}/luma-write-native-ui.XXXXXX")"
runtime_dir="${session_root}/runtime"
mkdir -p "${runtime_dir}"
chmod 700 "${runtime_dir}"
mkdir -p /tmp/.X11-unix
if [[ $(stat -c '%a' /tmp/.X11-unix) != 1777 ]]; then
  chmod 1777 /tmp/.X11-unix
fi

xvfb_pid=""
cleanup() {
  if [[ -n "${xvfb_pid}" ]] && kill -0 "${xvfb_pid}" 2>/dev/null; then
    kill "${xvfb_pid}" 2>/dev/null || true
    wait "${xvfb_pid}" 2>/dev/null || true
  fi
  if [[ -d "${session_root}" ]]; then
    rm -r -- "${session_root}"
  fi
}
trap cleanup EXIT INT TERM

export XDG_RUNTIME_DIR="${runtime_dir}"
export GTK_A11Y=atspi
# GtkFileDialog always delegates to the desktop portal when GTK detects this
# nested rootless build container. The portal's own SVG sandbox cannot nest in
# that environment, so use Luma Write's explicitly test-only in-process dialog
# while retaining the same user-driven destination and production save path.
export LUMA_UI_TEST_DIRECT_FILE_CHOOSER=1
# GTK_USE_PORTAL is presence-based, so the string "0" still enables it. The
# RPM build container has no document-portal FUSE mount; leave the variable
# absent to exercise GTK's in-process native chooser in this headless lane.
unset GTK_USE_PORTAL
export GDK_BACKEND=x11
# Xvfb exposes no DRM device. Keep GTK's headless acceptance lane on the
# deterministic software renderer so probing Vulkan is neither useful nor a
# fatal warning under G_DEBUG=fatal-warnings.
export GDK_DISABLE=vulkan
export GSK_RENDERER=cairo
export LIBGL_ALWAYS_SOFTWARE=1

xvfb_log="${run_artifacts}/xvfb.log"
display_file="${session_root}/display-number"
Xvfb -displayfd 3 -screen 0 1440x900x24 -nolisten tcp -noreset \
  3>"${display_file}" 2>"${xvfb_log}" &
xvfb_pid=$!

deadline=$((SECONDS + 20))
while [[ ! -s "${display_file}" ]]; do
  if ! kill -0 "${xvfb_pid}" 2>/dev/null; then
    echo "Xvfb exited before allocating a display; see ${xvfb_log}." >&2
    exit 1
  fi
  if (( SECONDS >= deadline )); then
    echo "Timed out waiting for Xvfb; see ${xvfb_log}." >&2
    exit 1
  fi
  sleep 0.1
done
display_number="$(tr -d '[:space:]' < "${display_file}")"
export DISPLAY=":${display_number}"

driver_command=(
  python3 "${driver}"
  --binary "${binary}"
  --artifacts "${run_artifacts}"
)
if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
  # Start the session bus only after Xvfb has exported DISPLAY. Services that
  # D-Bus activates (including portal and accessibility helpers) otherwise
  # inherit a pre-display environment and can block the application before it
  # publishes its AT-SPI tree.
  dbus-run-session -- "${driver_command[@]}"
else
  "${driver_command[@]}"
fi

echo "Native GTK/AT-SPI smoke passed; artifacts: ${run_artifacts}"
