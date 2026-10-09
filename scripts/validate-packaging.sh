#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_directory="${LUMA_BUILD_DIRECTORY:-${project_root}/_build/write}"

while IFS= read -r script; do
  bash -n "${script}"
done < <(find "${project_root}/scripts" -maxdepth 1 -type f -name '*.sh' -print | sort)

desktop_file="${build_directory}/data/org.projectluma.Write.desktop"
if [[ -f "${desktop_file}" ]]; then
  desktop-file-validate "${desktop_file}"
else
  echo "Configured desktop file not present; build before metadata validation." >&2
fi

metainfo_file="${build_directory}/data/org.projectluma.Write.metainfo.xml"
if [[ -f "${metainfo_file}" ]]; then
  appstreamcli validate --pedantic --override=url-homepage-missing=info \
    "${metainfo_file}"
else
  echo "Configured AppStream metadata not present; build before validation." >&2
fi

if [[ -f "${build_directory}/data/org.projectluma.Write.gschema.xml" ]] &&
   command -v glib-compile-schemas >/dev/null 2>&1; then
  glib-compile-schemas --strict --dry-run "${build_directory}/data"
fi

echo "Available documentation/script/package syntax checks passed."
