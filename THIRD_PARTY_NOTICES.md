# Third-party notices

GitMycelium's own source code is licensed under the Apache License, Version 2.0
(see [LICENSE](LICENSE)). It relies on the components listed below. Each of them
keeps its own license, and nothing in GitMycelium's license changes that.

## Included in this repository

### Lucide icons — ISC License

The SVG icons in `src/theme/icons/` come from [Lucide](https://github.com/lucide-icons/lucide).
They are renamed in the `gk::Icon` enum, and the SVG paths are unchanged.
The full license text is in [third_party/lucide/LICENSE](third_party/lucide/LICENSE).

## Linked at build time (not included in this repository)

### Qt 6 — GNU LGPL v3

[Qt](https://www.qt.io) (Core, Gui, Widgets, Svg; Test for the test suite only) is
used under the [GNU Lesser General Public License v3](https://www.gnu.org/licenses/lgpl-3.0.html).
GitMycelium links Qt **dynamically** and does not modify it. If you distribute a
binary that contains Qt libraries (for example a macOS `.dmg` produced by
`macdeployqt`, or an AppImage), you must:

- ship the LGPLv3 and GPLv3 license texts with it (they are in `third_party/qt/`, and
  the release builds copy them into the package);
- make the source code of the Qt version you used available, or a written offer
  for it (pointing to the unmodified upstream release at
  https://download.qt.io/archive/qt/ satisfies this for an unmodified Qt);
- leave the user able to replace the Qt libraries with their own build (dynamic
  linking already satisfies this).

### libgit2 — GNU GPL v2 with linking exception

[libgit2](https://libgit2.org) is used under the
[GPLv2 with a linking exception](https://github.com/libgit2/libgit2/blob/main/COPYING).
The exception allows linking libgit2 into programs under any license, as long as
libgit2 itself is distributed under its own terms. If you redistribute a binary
that bundles libgit2, include its `COPYING` file. libgit2 can in turn bundle or
link third-party code (for example zlib, PCRE2, llhttp, libssh2, OpenSSL, or the
platform's TLS library), depending on how it was built. Those components' notices
are listed in libgit2's `COPYING` and must travel with a bundled copy.

## Used at run time as separate programs

GitMycelium starts these programs as separate processes. They are neither
linked nor distributed with GitMycelium:

- **git**, GNU GPL v2 (https://git-scm.com);
- **git-lfs**, MIT (https://git-lfs.com), optional;
- the external diff and merge tools, terminal emulator, GPG or SSH that the user
  configures.

## Fonts

GitMycelium does not ship any fonts. It uses a monospace font that is already
installed on the system (JetBrains Mono, Iosevka, Cascadia Code, Fira Code, Hack,
Meslo or DejaVu Sans Mono, whichever is found first, otherwise the system's fixed
font).

## Trademarks

"Git" and the Git logo are trademarks of Software Freedom Conservancy. Qt is a
trademark of The Qt Company Ltd. All other trademarks belong to their respective
owners. GitMycelium is an independent project, and none of these owners endorse
or are affiliated with it.
