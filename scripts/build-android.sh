#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
: "${DEV_ARTIFACT_DIR:?Run this build through devctl run}"
: "${LUMA_PORT_ROOT:?Set the exact reviewed ProjectLuma carrier checkout}"
: "${LUMA_PORT_ANDROID_SHARED:?Set the admitted Android cache owned by this build lane}"
: "${LUMA_PORT_ANDROID_DATA:?Set a writable admitted download/source cache}"
: "${LUMA_PORT_ANDROID_KEYSTORE:?Set the persistent app signing keystore on this build host}"
for directory in "$LUMA_PORT_ANDROID_SHARED" "$LUMA_PORT_ANDROID_DATA"; do
  if [[ ! -r "$directory" || ! -w "$directory" || ! -x "$directory" ]]; then
    printf 'Android cache is not accessible to this build lane: %s\n' "$directory" >&2
    exit 1
  fi
done
for key in LUMA_PORT_ANDROID_KEY_ALIAS LUMA_PORT_ANDROID_STORE_PASSWORD LUMA_PORT_ANDROID_KEY_PASSWORD; do
  [[ -n "${!key:-}" ]] || { printf 'Missing release signing variable: %s\n' "$key" >&2; exit 1; }
done
[[ -r "$LUMA_PORT_ANDROID_KEYSTORE" ]] || { echo 'Signing keystore is unreadable' >&2; exit 1; }
# Source and build provenance, without passwords or private keystore contents.
python3 - "$LUMA_PORT_ROOT" "$PWD" "$DEV_ARTIFACT_DIR/source-revisions.json" <<'PY'
import json,subprocess,sys
from pathlib import Path
records={}
for name,root in zip(('carrier','write'),sys.argv[1:3]):
 records[name]={'commit':subprocess.check_output(['git','-C',root,'rev-parse','HEAD'],text=True).strip(),
                'dirty':bool(subprocess.check_output(['git','-C',root,'status','--porcelain'],text=True))}
 if records[name]['dirty']: raise SystemExit('Commit the candidate source before building: '+name)
Path(sys.argv[3]).write_text(json.dumps(records,indent=2)+'\n')
PY
export LUMA_PORT_ANDROID_HOST=local
export LUMA_PORT_ANDROID_WORK="$DEV_ARTIFACT_DIR/android-work"
export LUMA_PORT_ANDROID_ARCH=${LUMA_PORT_ANDROID_ARCH:-aarch64,x86_64}
"$LUMA_PORT_ROOT/tools/luma-port/luma-port" android "$PWD" --out "$DEV_ARTIFACT_DIR/apks"
