/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

typedef enum {
    MD_HEADING1, MD_HEADING2, MD_HEADING3, MD_HEADING4, MD_HEADING5, MD_HEADING6,
    MD_STRONG, MD_EMPHASIS, MD_CODE, MD_CODE_BLOCK, MD_QUOTE, MD_LINK, MD_RULE, MD_STRIKE,
    MD_STYLE_COUNT
} MdStyle;

typedef struct {
    MdStyle style;
    int start, end;   /* Unicode character offsets, as required by GtkTextBuffer */
    char *url;
} MdSpan;

typedef struct {
    char *text;
    GArray *spans;
} MdCell;

typedef struct {
    int offset;          /* One object-replacement character in document text. */
    guint columns;
    guint8 *alignments;   /* cmark-gfm: 'l', 'c', 'r', or zero (left). */
    GPtrArray *cells;     /* Row-major; first row is the header. */
    char *tsv;
} MdTable;

typedef struct {
    char *source, *text;
    GArray *spans;
    GHashTable *anchors;   /* heading id -> one-based character offset */
    GPtrArray *tables;
} MdDocument;

MdDocument *md_parse(const char *source, gsize size, GCancellable *cancel, GError **error);
MdDocument *md_load(GFile *file, GCancellable *cancel, GError **error);
void md_document_free(MdDocument *doc);
char *md_copy_text(MdDocument *doc, int start, int end);
