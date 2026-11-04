# Zen Markdown

Open a local `.md` file with `zen-markdown notes.md`, drop a document into the window, or use **Ctrl+O**. Zen Markdown reads CommonMark using cmark and displays the formatted document in a selectable GTK text view.

- Select text and use **Ctrl+C** to copy it into another application.
- Click a heading link to jump within the document. Click a web or local file link to open its default handler. Relative links resolve from the Markdown file's directory.
- Use **Ctrl++**, **Ctrl+−** or the hover controls to adjust text size. **Ctrl+0** restores it.
- Toggle **Source** or press **Ctrl+U** to read and copy the original Markdown.

The reader formats headings, lists, quotations, fenced and inline code, emphasis, links and horizontal rules. Images appear as links labeled with their alternative text. HTML appears as source text. The parser accepts UTF-8 files up to 8 MiB. GitHub-specific tables and task lists remain plain text.

The build requires the `gtk4` and `libcmark` pkg-config modules. On Ubuntu, cmark's headers are in `libcmark-dev`. `zen-markdown --self-test` checks the parser and character offsets. For a native window and clipboard check from the repository root, run `apps/zen-markdown/tests/reading-smoke.sh build/zen-markdown`.

The parser bounds file size (8 MiB), nesting and node count, and rejects more than 10,000 formatting spans before applying them to GTK. Closing or replacing a window cancels pending file reads.
