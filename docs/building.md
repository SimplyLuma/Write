# Building and testing

Install Meson, Ninja, a C++20/C17 compiler, GTK 4.22 development files,
libadwaita, GLib/GIO, Wayland, Fontconfig, Cairo, json-glib, libxml2 and
LibreOfficeKit headers. Runtime tests also need Writer/Core 26.2 and Xvfb.

`./scripts/build-release.sh` builds the included Luma Kit and then Write.
Set `BUILD_ROOT` or `BUILD_JOBS` to adjust output location and parallelism.

```sh
meson test -C _build/write --suite unit --print-errorlogs
SAL_USE_VCLPLUGIN=svp LUMA_LIBREOFFICE_PROGRAM=/usr/lib64/libreoffice/program \
  _build/write/tests/libreoffice-engine-integration images
```

The `images` test exercises insertion, click-away editing, single-click
reselection, drag movement, resize, Escape and a DOCX round trip against Writer.
Other engine scenarios qualify DOCX/ODT saving, PDF export, header/footer
changes and formatting. `ui-responsive` checks phone, tablet and desktop
controls in an isolated X server; `native-ui-smoke` drives the production app.

The engine adapter remains separate from the UI. Each future platform must
supply a matching native LibreOfficeKit implementation and package its native
filters, services, data and licenses, then rerun the round-trip tests.
