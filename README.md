# Write

Write is Luma’s native word processor, part of the Creative Collection.
It provides a quiet document canvas, contextual formatting controls and
interoperable documents through LibreOfficeKit.

## Features

- Paragraph styles, fonts, colour, lists and alignment.
- Images with selection handles, drag positioning, size and wrapping controls.
- Tables, comments, document outline, search and replace.
- Page sizes, margins, headers, footers and page numbers.
- Validated, atomic DOCX and ODT saves, recovery and PDF export.
- Adaptive desktop and handheld layouts using Luma Kit.

DOCX and ODT are the editable formats. Legacy DOC and text files can be
imported and saved as DOCX or ODT. Complex legacy layouts, floating frames and
font substitutions can change during conversion; inspect the result before
replacing an original. Macro-enabled and encrypted documents are not supported.

## Build

The Linux build targets x86_64 Fedora 44 / Luma with GTK 4.22 and
LibreOfficeKit 26.2. The qualified Luma Kit source is included under
`vendor/luma-platform`, so building does not require a private repository.

```sh
./scripts/build-release.sh
```

See [building](docs/building.md) and [distribution](packaging/README.md).
macOS, Windows and Android require their own native engine builds; Linux
binaries cannot serve as those platforms’ dependencies.

## Licenses

Original software is Apache-2.0, documentation CC-BY-4.0, and original artwork
CC-BY-SA-4.0. Third-party components retain their own licenses. See
[LICENSE.md](LICENSE.md) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
