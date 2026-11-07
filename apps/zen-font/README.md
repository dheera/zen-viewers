# Zen Font

A quiet place to look at type. Open a font file and get a spacious, editable specimen on warm paper, with an alphabet and a small text sample underneath. Controls live at the edges and appear on hover, like the other Zen viewers.

- Open TTF, OTF, TTC/OTC collections and WOFF/WOFF2. Availability of webfont decompression follows your distro's FreeType build.
- Edit the large specimen directly, select and copy text, and change the type size.
- Browse the font's Unicode characters in a virtualized grid. Click a glyph to copy it, or search for a character or a code such as `U+03A9`.
- Font details show the designer, version, copyright, glyph counts and format. Collections have a face selector. Variable fonts have live sliders for their axes.
- Files load on a worker thread with the common delayed loading ring. Glyph widgets are allocated as they scroll into view.
- Each window previews the file in a private Fontconfig configuration and Pango font map. Characters outside the font's coverage appear as missing glyphs.

Specimen edits and variation settings last for the current preview. The character browser shows Unicode-mapped characters.

## Usage

```sh
zen-font MyFont.ttf
zen-font Collection.ttc
zen-font --inspect MyFont.ttf
```

| Key | Action |
| --- | --- |
| Ctrl+O | Open a font |
| Ctrl++ / Ctrl+− | Change specimen size |
| Ctrl+G | Switch between specimen and glyphs |
| Ctrl+I | Details, collection faces and variable axes |
| Ctrl+R | Reset specimen, size and axes |
| Ctrl+C | Copy selected specimen text |
| F11 | Fullscreen |
| Ctrl+W / Ctrl+Q | Close |

## Build

The app uses C, GTK 4.6+, FreeType, Fontconfig and Pango. On Ubuntu, install `libgtk-4-dev libfreetype-dev libfontconfig-dev libpango1.0-dev`, then run `make zen-font` from the repository root.

`python3 apps/zen-font/tests/ui-smoke.py build/zen-font` runs the UI smoke test in its own Xvfb display. It needs `xvfb`, `xdotool`, `xclip`, Python Pillow and the DejaVu fonts. An optional second argument points to a variable font for the variation rendering check.
