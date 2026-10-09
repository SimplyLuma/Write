# Third-party notices

## Luma Kit

The qualified shared toolkit snapshot under `vendor/luma-platform` comes from
Luma’s `src/luma-platform` at revision
`5f4854e6547e65f30cf78ba1c53fab8f78088fc0`. Its original software is Apache-2.0.
The snapshot is retained here to make this application build independently.
Internal Markdown notes are excluded and demo account names are anonymized;
toolkit runtime implementation and build inputs are preserved.
The current shared project is https://github.com/SimplyLuma/luma.

The toolkit uses Lucide icons, licensed under ISC, with retained MIT notices for icons derived from Feather. The full notice is retained
in `LICENSES/Lucide-ISC.txt`. Bundled Newsreader fonts carry their upstream
SIL Open Font License and copyright in
`vendor/luma-platform/appkit/fonts/Newsreader-OFL.txt`. Neither is relicensed
under Luma’s default artwork license.

## Document engine and Linux runtime

Write uses unmodified LibreOfficeKit / Writer 26.2.6.3. LibreOffice’s licensing
and component notices are retained in the runtime; see
https://www.libreoffice.org/licenses/. The distribution includes its program
files, Writer filters, UNO services, registry and shared data, rather than
assuming a system LibreOffice installation. Depot installs the versioned
`org.projectluma.Platform.Office` runtime automatically.

GTK, GLib, libadwaita, Cairo, Fontconfig, libxml2, compiler runtimes, fonts and
other engine dependencies retain their controlling licenses. Each assembled
binary contains a package inventory, original license files and source-package
provenance. Distributors must preserve those files with the application and
provide corresponding source for components whose licenses require it.

The original launch artwork and application artwork are by Luma contributors,
licensed under CC-BY-SA-4.0. Runtime Figtree, Liberation and OpenSymbol fonts
retain their packaged licenses. No Microsoft fonts or private documents are
included in the source or distribution.
