# Write Android port

- Owner: Codex / Creative Collection Android ports, `luma-dev-2`.
- Status: source candidate prepared; actual APK and phone delivery blocked by build admission/cache access. Application/provider integration remains incomplete.
- Branch: `agent/1003/codex-write-android`, GitHub `SimplyLuma/Write`.
- Worktree: `/srv/dev/worktrees/write/codex-write-android`.
- Base: public release head `e0a42d93b5ede129198f6703864051358de70bbe`.
- Shared carrier: `nmcmil/ProjectLuma`, branch `agent/1003/codex-mobile-release`, head `de74685`; `/srv/dev/worktrees/projectluma/codex-mobile-release`.

## Scope/result

Nick requested Android Write then Focus, macOS subsequently, and common build/
release/update practices for 12+ apps. Reused the existing native `luma-port`
scaffolding at `5dc0c658...`; Photos' recent work was on another machine and Nick
said that scaffolding is sufficient. Write's released application is C++ with
GTK/Luma UI and LibreOfficeKit. It is not a new Python/mobile editor.

Added native manifest (package `org.projectluma.write`, candidate version code
1025, stable GitHub update feed), baseline dependency hashes, build wrapper,
source checks and Android plan. Android engine discovery now accepts the
launcher's program-data directory with `sofficerc`, matching Android's linked
`liblo-native-code.so`; Linux's search order remains the same. Discovery was
adapted from the older private Write port at `c042f5e3666af230e9051ecbcd03fdce3af06199`
with MPL-2.0 credit. The newer released document fixes were preserved; the old
port app was not substituted. See `docs/android.md` and carrier `RELEASES.md`.

## Evidence

- `dev-write-android-source-ba13bfcd.service`, exit 0: `bash scripts/check-android-source.sh` via devctl, ten core/unit tests and host-compiled Android discovery branch. Outputs `/srv/dev/accounts/second/artifacts/dev-write-android-source-ba13bfcd`.
- `dev-write-android-adapter-syntax-v2-827657b8.service`, exit 0: actual engine adapter syntax-checked against the carrier's exact LibreOffice 26.2 headers, Linux and Android conditional branches. Host compiler only, no Android link/runtime. First attempt omitted the project's existing `-Wno-missing-field-initializers` and failed on established aggregate initialization warnings; corrected to project flags, no source warnings disabled beyond its existing configuration.
- `dev-write-discovery-strict-b5c8d285.service`: Linux/Android/macOS conditional discovery compiled and run on host (macOS means discovery simulation, not a macOS build). See result.json for exit evidence.
- Shared carrier: seven Python tests; API-35 Java compilation; twenty negative/positive update policy checks, all passed at `dev-mobile-release-final-01b6fc2d`.
- Shell parsing and `git diff --check` passed. No GTK/LOK integration, NDK build, emulator, phone install or GitHub APK publication occurred. No service was left running.

## Gates/next action

APK job was refused before starting: `devctl run write-android-candidate --path
"$PWD" --disk-gib 8 --wait -- bash scripts/build-android.sh`, exit 75,
free=25.7 GiB and required reserve=20 GiB. Existing runtime/toolchain cache
`/root/luma-port-20261001/android` is inaccessible to this identity; data cache
`/mnt/luma-secondary/luma-port-20261001/android` is readable/root-owned and full.
No keys were copied and no other task/artifacts were pruned.

The existing build owner must run the committed app/carrier in its admitted
lane, or an administrator must provide this account a dedicated writable shared
Android cache with sufficient admitted storage. Persistent app signing identity
also needs selection in that lane. Build wrapper explicitly requires these
inputs and committed source; no automatic root fallback.

Then build ARM64/x86_64, verify ELF/page alignment and complete the provider
adapter. Current application open/save/export paths still assume local files;
content:// provider requests require real import/export and truthful copy
semantics. Also qualify printing, touch/IME, accessibility, rotation/recreation,
recovery, actual DOCX/ODT/PDF/picture/header-footer round trips and same-key
N-to-N+1 data preservation. Inspect existing phone package/code/certificate
before install; raise 1025 if already exceeded, preserve installed data. Obtain
independent updater review per the platform Class D contract before public
release. Focus publication stays with Write Guy; its Android work is next after
Write has a working candidate. macOS path discovery is preparation only.

Rollback: revert source branch or remove the update manifest stanza; no deployed
state changed. Released-device fixes require a higher-code repair APK, not an
uninstall/downgrade. Git records the final online source/handoff revision.
