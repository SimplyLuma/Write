#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build_root=${BUILD_ROOT:-"$root/_build"}
output=${1:?Usage: package-flatpak.sh OUTPUT_DIRECTORY}
stage=${STAGED_INSTALL:-"$output/stage"}
app="$output/app"
kit="$build_root/platform"
mkdir -p "$output"
if [[ -z ${STAGED_INSTALL:-} ]]; then
  meson install -C "$build_root/write" --destdir "$stage"
fi
test -x "$stage/usr/bin/luma-write"
flatpak build-init --arch=x86_64 "$app" org.projectluma.Write org.projectluma.Sdk org.projectluma.Platform 44
cp -a "$stage/usr/." "$app/files/"
mkdir -p "$app/files/lib" "$app/files/lib/office" "$app/files/share/licenses/org.projectluma.Write"
cp -a "$kit/share/." "$app/files/share/"
for directory in lib lib64; do
  if [[ -d "$kit/$directory" ]]; then
    find "$kit/$directory" -maxdepth 1 -name '*.so*' -exec cp -a -t "$app/files/lib" {} +
  fi
done
cp -a "$root/LICENSES" "$app/files/share/licenses/org.projectluma.Write/"
cp "$root/LICENSE.md" "$root/THIRD_PARTY_NOTICES.md" "$app/files/share/licenses/org.projectluma.Write/"
glib-compile-schemas --strict "$app/files/share/glib-2.0/schemas"
flatpak build-finish \
  --command=luma-write --no-inherit-permissions \
  --share=ipc --socket=wayland --socket=fallback-x11 --device=dri \
  --env=LUMA_LIBREOFFICE_PROGRAM=/app/lib/office/lib64/libreoffice/program \
  --env=LD_LIBRARY_PATH=/app/lib/office/lib64/libreoffice/program:/app/lib:/app/lib/office/lib64 \
  --extension=org.projectluma.Platform.Office=directory=lib/office \
  --extension=org.projectluma.Platform.Office=version=44 \
  --extension=org.projectluma.Platform.Office=add-ld-path=lib64 \
  --extension=org.projectluma.Platform.Office=no-autodownload=false \
  --extension=org.projectluma.Platform.Office=autodelete=false \
  "$app"
printf 'Prepared Flatpak build directory: %s\n' "$app"
