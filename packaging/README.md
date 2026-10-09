# Distribution

Write’s Depot identity is `org.projectluma.Write`, with launcher
`org.projectluma.Write.desktop`. Keep this identity stable across packages.

Depot uses the Luma Platform 44 runtime and its automatically installed
Office extension. `packaging/flatpak/org.projectluma.Write.json` builds the
qualified Luma Kit snapshot and Write without a private source dependency.
No separate LibreOffice installation is required of the user.

`scripts/assemble-linux-bundle.py` assembles an x86_64 Linux payload containing
the application, toolkit, LibreOffice program/filter/data tree, shared-library
closure, fonts, licenses and package/source inventory. It does not copy user
profiles, documents or credentials. Its launcher uses the included engine.

Release artifacts belong outside Git. Retain checksums, source commit, build
provenance, test results and corresponding-source material with every release.
The repository manager signs the store release and publishes its Depot listing.
