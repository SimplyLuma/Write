# Write Android candidate

The target is the released C++/GTK Write application with its existing
LibreOfficeKit engine, carried by the existing native `luma-port` Android
backend. This task adds `luma-app.toml`, locked baseline inputs, an admitted
build entry point and native engine discovery. It preserves the newer released
application's Linux document fixes rather than replacing it with the older
`ProjectLuma/Write` `luma-port/write` branch.

`office_locations.hpp` adapts discovery from that branch at
`c042f5e3666af230e9051ecbcd03fdce3af06199` (MPL-2.0). Android uses the launcher's
`LUMA_LIBREOFFICE_PROGRAM` and checks `sofficerc`, because the engine code is
linked through Android's `liblo-native-code.so`; Linux still discovers its
`libsofficeapp.so`/`libmergedlo.so`. macOS path discovery alone is not a macOS
engine qualification; the engine still enforces the existing 26.2 series.

The carrier baseline and TDF APK hashes are in `packaging/android/inputs.toml`.
The Android toolkit uses the existing port branch's portability changes and
requires its own rendering/behavior qualification against Write's vendored
Linux toolkit. Do not silently replace the Linux toolkit lock. The APK's app,
carrier, native toolkit and dependency revisions must accompany its evidence.

Build only in an assigned Android lane with its admitted caches and persistent
signing key:

```sh
# Set the LUMA_PORT_* variables named by scripts/build-android.sh on the build host.
devctl run write-android-candidate --path "$PWD" --disk-gib 8 --wait -- \
  bash scripts/build-android.sh
```

The wrapper fails on inaccessible caches, missing signing configuration or
uncommitted sources. It records source revisions and gives this app a private
working directory under the job artifacts; it never adopts another app's task.
The existing builder also requires its reviewed build container and a writable
shared runtime-cache lane. Do not bypass root-owned cache permissions or the
server's disk reserve. If the cache belongs to another lane, its owner must
build the committed candidate or provide a dedicated shared lane.

The candidate reserves Android version code 1025. Confirm the currently
installed package and signing certificate before a phone install, and raise
the code if needed. Public download notices use the common carrier's stable
feed; see `tools/luma-port/RELEASES.md` in the carrier repository. No APK or
feed has been published by this source-preparation task.

Run host source checks with:

```sh
devctl run write-android-source --path "$PWD" --disk-gib 1 --wait -- \
  bash scripts/check-android-source.sh
```

These cover the ten existing/core discovery tests and the Android discovery
branch compiled with the host compiler. They do not exercise LibreOfficeKit or
GTK on Android. Open gates: actual ARM64/x86_64 APK builds, exact bundled source/
licence closure, runtime and page alignment, document-provider import and save/
export (content URIs are not local paths), touch/IME, printing, recovery,
accessibility, rotation, real document round trips and N-to-N+1 data-preserving
upgrade. The released application's provider paths currently still need an
Android adapter; do not call a working private document copy a successful
provider overwrite. Independent updater review is required before publication.
