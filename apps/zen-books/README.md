# Zen Books

Open a DRM-free EPUB or a CBZ comic in a borderless window. EPUB text reflows as the window resizes. The reader uses Noto Serif when available, with DejaVu Serif and the system serif font as fallbacks. CBZ pages fit the window and follow natural filename order.

| Action | Control |
| --- | --- |
| Previous or next chapter or comic page | **← / →** or the bottom arrows |
| Scroll a screen or advance at the chapter edge | **Page Up / Page Down**, **Space / Shift+Space** |
| Open or hide the contents panel | **Ctrl+B** or **Contents**. **Esc** closes it. |
| Adjust EPUB text size | **+ / −** |
| Toggle night mode | **N** |
| Copy text | Select text and press **Ctrl+C** |
| Open, fullscreen, close | **Ctrl+O**, **F11**, **Ctrl+W** |

The viewer saves the chapter or comic page, reading position, text size, and night setting locally under `${XDG_STATE_HOME:-$HOME/.local/state}/zen-books`. When you return to the same file path, it restores your reading place.

EPUB rendering focuses on reflowable text, headings, links, and embedded illustrations. A worker reads chapters and comic images on demand. Inline EPUB images request previews up to 1,400 pixels per side, and comic pages up to 3,000. The reader supports archives up to 1 GiB, individual resources up to 64 MiB, and chapters with up to 2 MiB of rendered text. The renderer uses native GTK and reads local archive resources.

## Build and try it

From the repository root on Ubuntu, install the development packages and build the app.

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libarchive-dev libxml2-dev
make zen-books
build/zen-books examples/above-the-fog.epub
```

Inspect an EPUB or CBZ without a display.

```sh
build/zen-books --inspect examples/above-the-fog.epub
```

Use `./scripts/build-deb.sh --app zen-books` to build a standalone Debian package. [Packaging notes](../../packaging/README.md)

## Checks

```sh
python3 apps/zen-books/tests/ui-smoke.py build/zen-books
```

The window test uses Xvfb, xdotool, xclip, dbus-daemon and Python Pillow. It exercises EPUB text and images, chapter navigation, saved reading state, and CBZ page order.
