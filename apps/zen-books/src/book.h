/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

typedef enum { ZEN_BOOK_EPUB, ZEN_BOOK_CBZ } ZenBookKind;
typedef enum {
    ZEN_BOOK_BOLD = 1 << 0,
    ZEN_BOOK_ITALIC = 1 << 1,
    ZEN_BOOK_MONOSPACE = 1 << 2,
    ZEN_BOOK_UNDERLINE = 1 << 3,
    ZEN_BOOK_STRIKE = 1 << 4,
    ZEN_BOOK_QUOTE = 1 << 5
} ZenBookStyle;

typedef struct { char *title, *path; } ZenBookChapter;
/* target is a normalized archive URI (reserved path characters escaped),
 * optionally followed by #fragment. Chapter/resource paths are decoded. */
typedef struct { char *title, *target; guint depth; } ZenBookTocEntry;

/* Book metadata is immutable after open; each resource read has its own ZIP
 * handle. Keep a reference while worker tasks use this object. */
typedef struct {
    gint references;
    ZenBookKind kind;
    char *title, *author;
    GPtrArray *chapters; /* ZenBookChapter* in spine/natural filename order */
    GPtrArray *toc;      /* ZenBookTocEntry* */
    gpointer private_data;
} ZenBook;

/* Every offset is a Unicode character offset, matching GtkTextBuffer. */
typedef struct { guint start, end, flags, heading_level; } ZenBookSpan;
typedef struct { guint start, end; char *target; gboolean external; } ZenBookLink;
/* The text contains one U+FFFC character at offset; replace it with an image
 * child anchor without changing the character count. Resource bytes are lazy. */
typedef struct { guint offset; char *path, *alt; } ZenBookImage;
typedef struct { guint offset; char *id; } ZenBookAnchor;
typedef struct {
    char *title, *path, *text;
    guint characters;
    GPtrArray *spans;   /* ZenBookSpan* */
    GPtrArray *links;   /* ZenBookLink* */
    GPtrArray *images;  /* ZenBookImage* */
    GPtrArray *anchors; /* ZenBookAnchor* */
} ZenBookDocument;

/* Call I/O/parsing functions on workers. No network access or GUI calls. */
ZenBook *zen_book_open(GFile *file, GCancellable *cancel, GError **error);
ZenBook *zen_book_ref(ZenBook *book);
void zen_book_unref(ZenBook *book);
ZenBookDocument *zen_book_load_chapter(ZenBook *book, guint chapter,
                                      GCancellable *cancel, GError **error);
void zen_book_document_free(ZenBookDocument *document);
GBytes *zen_book_read_resource(ZenBook *book, const char *archive_path,
                               GCancellable *cancel, GError **error);
/* Resolve an already-normalized link/TOC target to a spine index. The optional
 * fragment is newly allocated; returns -1 for a target outside the spine. */
gint zen_book_find_chapter(const ZenBook *book, const char *target, char **fragment);
