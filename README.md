# Zen Viewers

Small native Linux viewers with rounded, borderless windows and controls that appear when the pointer moves.

| App | Current functionality |
| --- | --- |
| **zen-pdf** | Fit-width PDF reading, page navigation, zoom, text selection and Unicode copying |
| **zen-image** | GPU-texture display, fit/actual-size modes, pan, zoom and view rotation |
| **zen-markdown** | Native CommonMark reading, selectable text, links, font sizing and source view |

The three apps share the same GTK 4 window shell. Each renderer is a separate app with its own dependencies. The shared C code is linked into each executable as a small static library; there is no additional shared-library package or background service.

## Build

On Ubuntu 22.04 or later, build all three with:

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libpoppler-glib-dev libcmark-dev
make
```

The executables are in `build/`:

```sh
build/zen-pdf examples/field-notes.pdf
build/zen-image examples/basin.png
build/zen-markdown examples/welcome.md
```

Running an app without a filename opens its start screen. All apps support **Ctrl+O**, file drag-and-drop, **Ctrl+W**, and **F11**. Drag an unclaimed part of the top area to move a window; drag an edge or corner to resize it. Each app contributes its own floating reading/view controls.

You can build apps independently:

```sh
make zen-image       # GTK only, including its image loaders
make zen-pdf         # GTK + Poppler
make zen-markdown    # GTK + libcmark
```

Only the chosen target's development dependencies are needed. PNG and JPEG are handled by the system loaders; install `webp-pixbuf-loader` on Ubuntu for WebP support. Other distributions need the corresponding GTK 4, Poppler GLib and cmark development packages. Fedora uses `gtk4-devel poppler-glib-devel cmark-devel gcc make pkgconf-pkg-config`.

## Install and packages

```sh
make install PREFIX="$HOME/.local"
```

Make sure `$HOME/.local/bin` is on the desktop session's `PATH`. For a system installation use `sudo make install`. To install just one app, add `APPS=zen-image` (or another app name). Use the same `PREFIX` and `APPS` values with `make uninstall` to remove it.

Debian/Ubuntu package builds require `dpkg-dev`:

```sh
sudo apt install dpkg-dev
./scripts/build-deb.sh
sudo apt install ./dist/*.deb
```

The default build creates separate `zen-pdf`, `zen-image`, and `zen-markdown` packages, plus a small `zen-viewers` metapackage that installs all three. To package only one viewer, use `./scripts/build-deb.sh --app zen-image`. Runtime dependencies are detected from the linked executable, so an image-only install does not require Poppler or cmark. Build on the target distribution/architecture or an appropriate older compatible baseline; binaries are not universal across Linux library versions.

See [packaging notes](packaging/README.md) for RPM builds and release details.

## PDF selection and copy

Drag starting on text to select across lines. Double-click a word or triple-click a line. **Ctrl+A** selects the current page's text, **Ctrl+C** or **Ctrl+Insert** copies it, and **Esc** clears the selection. Completing a selection also supplies the Linux PRIMARY clipboard for middle-paste in another app.

Drag blank page space to pan; **Ctrl+drag** or middle-drag pans over text. Zoom, scrolling and window resizing preserve selection alignment. Image-only scans require an existing text layer; OCR is not included. Text extraction stays on a worker, and selection changes do not rerender the PDF.

See the [PDF controls](apps/zen-pdf/README.md), [image controls](apps/zen-image/README.md), and [Markdown controls](apps/zen-markdown/README.md).

## Structure

```text
common/                 Window, hover overlays, open/drop, fullscreen, feedback
apps/zen-pdf/           Poppler rendering, page navigation, text selection
apps/zen-image/         Image decoding, retained texture, pan/zoom/rotation
apps/zen-markdown/      CommonMark parsing, native text styles and links
examples/               Small original files to try
previews/               Actual app screenshots, with and without controls
scripts/                Package builders
packaging/              RPM spec and package documentation
```

[Architecture and adding a viewer](common/README.md)

## Verification

```sh
make check
ZEN_PDF_BIN="$PWD/build/zen-pdf" apps/zen-pdf/tests/smoke.sh
```

The optional native-window tests use `xvfb xauth xdotool xclip dbus-daemon python3-pil` and a WebP loader:

```sh
make check-ui
```

They verify actual rendered pixels, PDF selection and both clipboards, image transforms and decode-error recovery, Markdown links, native text copying, and source view. Xvfb tests validate functionality; they do not benchmark hardware GPU performance.

## Prototype boundaries

- PDF reading and selection are implemented. Search, OCR, annotations and editing are not included. Its production page presenter currently uses cached Cairo surfaces; the retained-texture PDF experiment remains a separate performance prototype.
- Images are still frames. Large inputs request a preview of at most 32 megapixels/16,384 pixels per axis, and compressed file input is limited to 128 MiB. Loader-specific memory use can exceed the preview size. Rotation affects the view, not the saved file.
- Markdown supports CommonMark rather than full GitHub-flavored Markdown. Images appear as alt-text links; raw HTML is shown as text. It reads UTF-8 files up to 8 MiB and bounds nesting, node count and formatting spans. Reading/source views are both read-only.
- Rounded transparency depends on the compositor. A noncomposited X11 desktop can show opaque corner backgrounds.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Linked libraries retain their own licenses.
