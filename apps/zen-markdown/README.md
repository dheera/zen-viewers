# Zen Markdown

Open a local `.md` file with `zen-markdown notes.md`, drop a document into the window, or use **Ctrl+O**. Zen Markdown reads CommonMark with GitHub-style tables and strikethrough using cmark-gfm, and displays it with native GTK text and table widgets.

- Select text and use **Ctrl+C** to copy it into another application.
- Click a heading link to jump within the document. Click a web or local file link to open its default handler. Relative links resolve from the Markdown file's directory.
- Use **Ctrl++**, **Ctrl+−** or the hover controls to adjust text size. **Ctrl+0** restores it.
- Toggle **Source** or press **Ctrl+U** to read and copy the original Markdown.
- Select text within a table cell to copy it. Hover over a table to reveal **Copy table**, which copies every cell as tab-separated text suitable for a spreadsheet. A document selection that includes a table copies its cells in reading order too.

The reader formats headings, lists, quotations, fenced and inline code, emphasis, strikethrough, links and horizontal rules. Tables have a shaded header, the alignment specified by their delimiter row, wrapped cell text and horizontal scrolling when the columns need more space. Bold, italic, code and links also work inside cells. Images appear as links labeled with their alternative text. HTML appears as source text. Task-list checkboxes remain plain text.

Reading text uses Noto Serif when installed, falling back to DejaVu Serif and the system serif font. Source and code use the system monospace font. Code blocks have a continuous, subtly shaded background with padding, including around blank lines. Ubuntu’s optional `fonts-noto-core` package supplies Noto Serif.

The build requires the `gtk4` and `libcmark-gfm` pkg-config modules and the cmark-gfm extensions library. On Ubuntu, install `libcmark-gfm-dev libcmark-gfm-extensions-dev`. `zen-markdown --self-test` checks parsing, Unicode offsets, table alignment, clipboard expansion and input limits. From the repository root, `apps/zen-markdown/tests/reading-smoke.sh build/zen-markdown` checks native reading/link/clipboard behavior, and `apps/zen-markdown/tests/table-smoke.sh build/zen-markdown` checks table copying, cell selection, table links, narrow resizing and source round trips. These optional UI tests need Xvfb, xauth, xdotool, xclip, D-Bus and Python 3. `tests/tables.md` is a visual fixture.

The parser bounds file size (8 MiB), nesting and node count, and rejects more than 10,000 formatting spans before applying them to GTK. Tables are limited to 32 columns and 2,048 cells across a document to keep native widget creation bounded. Closing or replacing a window cancels pending file reads. Drag to select text in a cell. Use **Copy table** or select the surrounding document to copy across columns.
