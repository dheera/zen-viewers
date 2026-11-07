# Zen Table

A quiet native viewer for CSV, TSV, and Parquet. Open an export, keep the data in view, and leave the controls at the edges.

- A spacious grid with resizable columns, a fixed header, row numbers, and continuous scrolling.
- Click a cell and press **Ctrl+C** to copy its full value. **Ctrl+Shift+C** copies the selected row as quoted TSV. Double-click a cell to read and select its full contents.
- **Ctrl+F** finds text across every row, without blocking the interface. Enter or **F3** finds the next occurrence and wraps at the end. **Esc** hides the search field.
- **Ctrl+L** jumps directly to a row number. **Ctrl+Home / Ctrl+End** jump to the beginning or end.
- The information button shows column names and Parquet types. Hover near the window edges for the shared Zen controls. Use **Ctrl+O** or drop a file to open it, and **F11** for fullscreen.

CSV and TSV use their first record as column names. Zen Table supports quoted delimiters, multiline cells, doubled quotes, UTF-8 BOMs, CRLF, empty fields, and uneven row lengths. Extra columns get generated names, and shorter rows get empty cells. Convert UTF-16 exports to UTF-8 before opening them. Invalid UTF-8 bytes are replaced for display. Browsing is read-only and follows file order.

## Build and run

```sh
make zen-table
build/zen-table examples/tidepool-survey.csv
build/zen-table measurements.parquet
```

The executable uses GTK 4.6 or later, GLib, and the system zstd, zlib, LZ4 and Brotli libraries. On Ubuntu, install `libgtk-4-dev cmake pkg-config libzstd-dev zlib1g-dev liblz4-dev libbrotli-dev` before building.

## Parquet

Parquet uses the bundled Carquet 0.7.3 C reader. It reads the schema and row count from the footer, then decodes requested pages on a worker. Numeric, Boolean, timestamp, decimal, dictionary, binary, list, and struct columns are supported. Nested values display as JSON, binary values as hexadecimal, and nulls as empty cells.

The source archive is pinned by SHA-256 and built locally with CMake. The build links to the distribution's compression libraries. The viewer accepts local regular files.

## Large tables

CSV and TSV are scanned on a worker into a 16-byte-per-record offset index. A first preview is available after 512 records while indexing continues. The viewer reads page values on demand through the open file descriptor. Reopen a table to see edits.

The GTK list creates rows around the visible area. The native cache holds up to 16 pages or 32 MiB. Flat Parquet tables use 128-row requests. Nested columns use a guarded row-group cache. The shared ring indicates work while navigation remains available. Opening another file or closing the window cancels pending work.

The viewer supports up to 512 columns, 16 MiB per cell and page, and 64 MiB per CSV record. Nested row groups have a 16 MiB uncompressed-input limit and a conservative 64 MiB decode estimate; exceptionally large groups need rewriting into smaller groups. Retained decoded arrays are checked against 64 MiB, though codec temporary memory can exceed that. GTK supports up to 4,294,967,295 rows. Large text files also need room for the offset index. Grid labels show previews, and copying and the cell detail window retain the full accepted value.

## Checks

```sh
sh apps/zen-table/tests/data-smoke.sh build/zen-table
# Optional developer checks:
python3 apps/zen-table/tests/data-smoke.py build/zen-table
python3 apps/zen-table/tests/ui-smoke.py build/zen-table
build/zen-table --inspect file.parquet 128 5 'search text'
```

`--inspect` runs without a display. Its optional row offset is zero-based, followed by a row count (up to 128) and optional search text. The shell tests use fixed Parquet fixtures and require no Python. The data tests cover quoting, multiline input, big cells, random access, invalid files, blocked file types, and real compressed Parquet. The UI tests use a private Xvfb display and D-Bus session, and verify selection, copy, global search, distant rows, and closing on 100,000-row CSV and Parquet files.
