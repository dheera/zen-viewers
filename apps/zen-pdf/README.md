# zen-pdf

A small native PDF reader for Linux. Open a PDF and the page fills the width of a rounded, borderless window. Scroll vertically to read and continue through pages, or use the left and right arrows to turn them. Controls appear near the top and bottom edges when you need them. An optional contents sidebar stays hidden until you open it.

## Build and run on Ubuntu

Run these commands in the source directory. The viewer is written in C and uses GTK 4 for the window and Poppler for PDF rendering. Ubuntu 22.04 and later provide both development libraries through apt.

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libpoppler-glib-dev
make
./zen-pdf /path/to/document.pdf
```

Run `./zen-pdf` by itself to open the start screen. Use **Ctrl+O** to choose a local file or drop a PDF into the window. If the document requires a password, the viewer prompts for it.

To add **Zen PDF** to your application menu and your file manager's **Open With** list, run `sudo make install`. To install only for your account, use `make install PREFIX="$HOME/.local"` and make sure `$HOME/.local/bin` is on your desktop session's `PATH`. Remove an installation with `sudo make uninstall` or `make uninstall PREFIX="$HOME/.local"`, respectively.

## Distribution packages

To build an installable Debian package from the same source, install `dpkg-dev` along with the build dependencies above and run `./scripts/build-deb.sh`. Install the resulting package with `sudo apt install ./dist/zen-pdf_"$(cat ../../VERSION)"-1_"$(dpkg --print-architecture)".deb`. Build on the release and architecture where you intend to run it. For Fedora, use `./scripts/build-rpm.sh`. [Package build and installation instructions](packaging/README.md)

## Reading

| Action | Control |
| --- | --- |
| Turn one page | **←** or **→**, **Page Up** or **Page Down**, the floating arrows, or a horizontal swipe when the page fits the window width |
| Go to the beginning or end | **Home** or **End** |
| Jump to a page number | **Ctrl+L** |
| Scroll and continue to adjacent pages | Wheel, vertical touchpad scroll, **↑** / **↓**, or **Space** / **Shift+Space** |
| Drag the page | Drag blank paper and pull past its edge to turn vertically |
| Show or hide contents and pages | Contents button, **Ctrl+B**, or **F9**; **Esc** closes it |
| Pan sideways when zoomed in | Drag blank paper, **Shift+wheel**, or **Shift+←** / **Shift+→** |
| Pan from over text | **Ctrl+drag** or middle-button drag |
| Select text | Drag over text, double-click a word, or triple-click a line |
| Copy the selection | **Ctrl+C** or **Ctrl+Insert** |
| Select all text on this page | **Ctrl+A** |
| Clear the selection | **Esc** |
| Fit the page width | **W** (default) |
| Show the whole page | **P** |
| Zoom in or out | **+** or **−**, the floating zoom buttons, or **Ctrl+wheel** |
| Toggle fullscreen | **F** or **F11** |
| Leave fullscreen | **Esc** |
| Move or maximize the window | Drag the top area or double-click it |
| Resize | Drag an edge or corner |
| Close the window | **Ctrl+W** |

Fit width preserves the page's aspect ratio as you resize. When a page is taller than the window, scrolling reaches its foot and then continues to the next page. Short pages also turn with the vertical wheel. A shorter page is centered vertically, and its surrounding margins take their colors from the page edges. Choose fit page to keep the whole sheet visible.

Scrolling and page turns animate on the frame clock. Pulling beyond the beginning or end of a document gives a bounded rubber-band effect. The desktop’s reduced-motion setting (`gtk-enable-animations`) disables these transitions and the bounce. The sidebar reads the PDF’s table of contents only when requested, on a background worker. Nested entries and named destinations navigate to their pages. Documents without a usable outline show a numbered page list. Outline traversal is bounded to 10,000 entries and 32 levels, and the result is cached for the open document.

Poppler rendering and text extraction run off the GTK interface thread. The reader caches up to three page images, preloads the adjacent page in the direction of travel, and limits each raster to 8,388,608 pixels (32 MiB). Cached images are retained GDK textures: GTK’s GL/Vulkan renderer moves and scales them during scrolling without rerunning Poppler or repainting the page through Cairo. GTK’s software renderer is supported too. The departing texture can briefly remain alive during a page transition after its cache entry is evicted. Edge colors are sampled once per raster, and the text-selection overlay uses the same page transform. Very large pages are rendered at a lower resolution and scaled to the requested display size. Rounded corners depend on transparency support from the compositor. A noncomposited X11 session may show an opaque area around them.

Selection follows the PDF’s embedded text. On Linux, selecting text also makes it available for middle-click paste into another app. For scanned pages, selection requires an existing OCR text layer.

## Verification

```sh
make check
tests/smoke.sh
```

The smoke test uses Python 3's standard library to create portrait, landscape and tall pages. It runs without a display, checks PNG dimensions and a narrow 1:240 page, and verifies error handling for malformed PDFs, arguments and file paths.

These commands exercise selection and copy in a private X11 session.

```sh
sudo apt install xvfb xauth xdotool xclip dbus-daemon python3-pil
python3 tests/text-selection.py
python3 tests/motion.py
```

The motion suite checks animated intermediate frames, vertical page boundaries, colored margins, document-end settling, sidebar navigation, nested and named outline destinations, and reduced motion.

To try the same document interactively, run the commands below.

```sh
python3 tests/make-fixture.py /tmp/zen-pdf-sample.pdf
./zen-pdf /tmp/zen-pdf-sample.pdf
```

You can also render a single page to a PNG from the terminal. The final arguments give the requested width in pixels (1–16384) and the page number, starting at one. Output size is subject to the same raster limits as the viewer.

```sh
./zen-pdf --render /path/to/document.pdf page.png 1000 1
```

## License

GNU General Public License version 3 or later. See [LICENSE](LICENSE).
