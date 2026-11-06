# Zen Viewers

**Minimalist, modern viewers for Linux.**

Zen Viewers brings the quiet, focused feel of tablet apps to the Linux desktop. Open a PDF, image, Markdown document, 3D model, video or book and let the content fill the window, with rounded corners and controls that fade in at the top or bottom edge.

Built in C with a shared GTK 4 shell, each viewer stays small and focused. Separate renderer packages let you install just the viewers you need.

| App | Current functionality |
| --- | --- |
| **zen-pdf** | Fit-width pages, smooth vertical scrolling, animated page turns, an optional page sidebar, zoom and text selection |
| **zen-image** | Folder navigation, optional image details, fit and actual size, pan, zoom and view rotation |
| **zen-markdown** | Serif reading view, formatted tables, selectable text, links, font sizing and source view |
| **zen-3d** | GPU mesh and point-cloud viewing, orbit, pan, zoom, view presets and optional model details |
| **zen-video** | Local playback, hover timeline, seeking, frame stepping, volume and subtitles |
| **zen-books** | Reflowable EPUB, CBZ comic pages, contents, night mode and saved reading position |

All six apps share the same GTK 4 window shell. Each renderer is a separate app with its own dependencies. The shared C code is linked into each executable as a small static library; there is no additional shared-library package or background service.

## Screenshots

Actual app windows with their edge controls visible. Click an image for the full-size screenshot.

| Zen PDF | Zen Image |
| --- | --- |
| [![Zen PDF with a fit-width page and floating controls](previews/pdf-hover.png)](previews/pdf-hover.png) | [![Zen Image with a landscape and floating view controls](previews/image-hover.png)](previews/image-hover.png) |
| **Zen Markdown** | **Zen 3D** |
| [![Zen Markdown with serif text and a formatted table](previews/markdown-hover.png)](previews/markdown-hover.png) | [![Zen 3D with a jade-colored trefoil model and hover controls](previews/3d-hover.png)](previews/3d-hover.png) |
| **Zen Video** | **Zen Books** |
| [![Zen Video with its playback timeline](previews/video-hover.png)](previews/video-hover.png) | [![Zen Books with a chapter from an illustrated EPUB](previews/books-hover.png)](previews/books-hover.png) |

With the controls hidden. [PDF](previews/pdf-quiet.png) · [Image](previews/image-quiet.png) · [Markdown](previews/markdown-quiet.png) · [3D](previews/3d-quiet.png) · [Video](previews/video-quiet.png) · [Books](previews/books-quiet.png).

## Build

These commands build all six apps on Ubuntu 22.04 or later.

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libpoppler-glib-dev libcmark-gfm-dev libcmark-gfm-extensions-dev libexif-dev libepoxy-dev libassimp-dev libmpv-dev libarchive-dev libxml2-dev
make
```

Run the executables from `build/`.

```sh
build/zen-pdf examples/field-notes.pdf
build/zen-image examples/basin.png
build/zen-markdown examples/welcome.md
build/zen-3d examples/trefoil.glb
build/zen-video examples/basin.webm
build/zen-books examples/above-the-fog.epub
```

Running an app without a filename opens its start screen. All apps support **Ctrl+O**, file drag-and-drop, **Ctrl+W**, and **F11**. Hover near the top for the filename, open, fullscreen and close buttons. Hover near the bottom for reading or viewing controls. **Tab** makes the controls available from the keyboard. Drag an unclaimed part of the top area to move a window or drag an edge to resize it.

Each app also has an individual build target.

```sh
make zen-image       # GTK and system image loaders, optional libexif
make zen-pdf         # GTK + Poppler
make zen-markdown    # GTK + cmark-gfm
make zen-3d          # GTK + libepoxy + Assimp
make zen-video       # GTK + libepoxy + libmpv
make zen-books       # GTK + libarchive + libxml2
```

Only the chosen target's development dependencies are needed. Libexif adds JPEG camera and exposure details. Install `fonts-noto-core` on Ubuntu for Markdown's preferred Noto Serif typeface. Run `zen-image --formats` to list your system's image formats and see the [image README](apps/zen-image/README.md) for optional loaders. See the [packaging notes](packaging/README.md) for Fedora dependencies.

## Window corners

Zen normally draws 16 px corners for floating windows. In Niri, it hands the window shape to the compositor to avoid two overlapping curves. With Niri's `clip-to-geometry true` already set, your existing rule applies. This rule scopes the setting to the Zen apps.

```kdl
window-rule {
    match app-id=r#"^io\.github\.(zenpdf|zenimage|zenviewers\.(Markdown|ThreeD|Video|Books))$"#
    geometry-corner-radius 8
    clip-to-geometry true
}
```

Use your preferred radius. On other compositors that clip windows, launch the viewer with `ZEN_CORNERS=system` to disable Zen's rounding. Use `ZEN_CORNERS=rounded` to force Zen's corners in windowed mode. [Niri window rules](https://github.com/niri-wm/niri/wiki/Configuration%3A-Window-Rules)

## Install and packages

```sh
make install PREFIX="$HOME/.local"
```

Make sure `$HOME/.local/bin` is on the desktop session's `PATH`. For a system installation use `sudo make install`. To install just one app, add `APPS=zen-image` (or another app name). Use the same `PREFIX` and `APPS` values with `make uninstall` to remove it.

Debian and Ubuntu package builds require `dpkg-dev`.

```sh
sudo apt install dpkg-dev
./scripts/build-deb.sh
sudo apt install ./dist/*_"$(cat VERSION)"-1_*.deb
```

The default build creates one package per app, plus a small `zen-viewers` metapackage that installs all six. To package only one viewer, use `./scripts/build-deb.sh --app zen-image`. The builder detects runtime dependencies from each executable. Build on the target distribution and architecture or an older compatible baseline.

See [packaging notes](packaging/README.md) for RPM builds and release details.

## PDF selection and copy

Drag starting on text to select across lines. Double-click a word or triple-click a line. **Ctrl+A** selects the current page's text, **Ctrl+C** or **Ctrl+Insert** copies it, and **Esc** clears the selection. Completing a selection also supplies the Linux PRIMARY clipboard for middle-paste in another app.

Use the wheel or touchpad to scroll across pages. The left and right arrows slide between pages, and **Ctrl+B** toggles the page sidebar. Drag blank page space to pan. **Ctrl+drag** or middle-drag pans over text. Zoom, scrolling and window resizing preserve selection alignment. Image-only scans need an embedded OCR text layer for selection. Text extraction stays on a worker, and selection changes do not rerender the PDF.

See the controls for [PDF](apps/zen-pdf/README.md), [Image](apps/zen-image/README.md), [Markdown](apps/zen-markdown/README.md), [3D](apps/zen-3d/README.md), [Video](apps/zen-video/README.md), and [Books](apps/zen-books/README.md).

## Structure

```text
common/                 Window, hover overlays, open/drop, fullscreen, feedback
apps/zen-pdf/           Poppler rendering, textured pages, navigation and selection
apps/zen-image/         Image decoding, folder navigation, details and view controls
apps/zen-markdown/      Markdown parsing, native text styles, links and tables
apps/zen-3d/            Assimp import, OpenGL rendering and orbit camera
apps/zen-video/         libmpv playback, OpenGL presentation and timeline
apps/zen-books/         EPUB reflow, comic pages and saved reading state
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

The optional native-window tests use `xvfb xauth xdotool xclip dbus-daemon python3-pil` a WebP loader, and FFmpeg for generating video fixtures.

```sh
make check-ui
```

They verify rendered pixels, edge controls, PDF motion and selection, image navigation and metadata, Markdown tables and copying, 3D navigation, video playback, and book navigation and resume. They run under Xvfb.

## Prototype boundaries

- PDF rendering runs on a Poppler worker. GTK presents cached page textures during scrolling and page turns. The sidebar shows the document outline when available and lists pages otherwise.
- Images are still frames. Large inputs request a preview of at most 32 megapixels/16,384 pixels per axis, and compressed file input is limited to 128 MiB. Loader-specific memory use can exceed the preview size. Rotation applies to the view only.
- Markdown supports CommonMark with the GitHub-style table extension. Images appear as alt-text links and raw HTML appears as text. It reads UTF-8 files up to 8 MiB and bounds nesting, node count and formatting spans.
- Zen 3D opens static geometry such as OBJ, GLB, STL and PLY. It requires desktop OpenGL 3.3. Supported extensions depend on the installed Assimp build.
- Rounded transparency depends on the compositor. A noncomposited X11 desktop can show opaque corner backgrounds.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Linked libraries retain their own licenses.
