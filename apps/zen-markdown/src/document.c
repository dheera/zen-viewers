/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "document.h"
#include <cmark.h>
#include <string.h>

#define MD_MAX_BYTES (8u * 1024 * 1024)
#define MAX_NODES 150000u
#define MAX_DEPTH 96u
#define MAX_STYLE_SPANS 10000u

typedef struct {
    MdDocument *doc;
    GString *out;
    GHashTable *anchor_suffixes;
    int offset;
    guint nodes;
    GCancellable *cancel;
    GError **error;
    gboolean failed;
} Builder;

static void clear_span(gpointer item) {
    MdSpan *span = item;
    g_free(span->url);
}

void md_document_free(MdDocument *doc) {
    if (!doc) return;
    g_free(doc->source);
    g_free(doc->text);
    g_array_unref(doc->spans);
    g_hash_table_unref(doc->anchors);
    g_free(doc);
}

static void append(Builder *b, const char *s) {
    if (s && *s) {
        g_string_append(b->out, s);
        b->offset += (int)g_utf8_strlen(s, -1);
    }
}

static void line_gap(Builder *b, unsigned int count) {
    if (!b->out->len) return;
    gsize at = b->out->len;
    while (count && at && b->out->str[at-1] == '\n') { count--; at--; }
    while (count--) append(b, "\n");
}

static void mark(Builder *b, MdStyle style, int start, const char *url) {
    if (b->failed || start == b->offset) return;
    if (b->doc->spans->len >= MAX_STYLE_SPANS) {
        g_set_error_literal(b->error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This Markdown document has too many formatting spans.");
        b->failed = TRUE;
        return;
    }
    MdSpan span = { style, start, b->offset, g_strdup(url) };
    g_array_append_val(b->doc->spans, span);
}

static void add_anchor(Builder *b, gsize byte_start, int pos) {
    GString *id = g_string_new(NULL);
    for (const char *p = b->out->str + byte_start; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_isalnum(c) || c == '_' || c == '-')
            g_string_append_unichar(id, g_unichar_tolower(c));
        else if (g_unichar_isspace(c) && id->len && id->str[id->len-1] != '-')
            g_string_append_c(id, '-');
    }
    if (id->len) {
        char *key = g_strdup(id->str);
        int suffix = MAX(1, GPOINTER_TO_INT(g_hash_table_lookup(b->anchor_suffixes, id->str)));
        while (g_hash_table_contains(b->doc->anchors, key)) {
            g_free(key);
            key = g_strdup_printf("%s-%d", id->str, suffix++);
        }
        g_hash_table_replace(b->anchor_suffixes, g_strdup(id->str), GINT_TO_POINTER(suffix));
        g_hash_table_insert(b->doc->anchors, key, GINT_TO_POINTER(pos + 1));
    }
    g_string_free(id, TRUE);
}

static void render(Builder *b, cmark_node *node, guint depth, guint list_depth, gboolean compact);

static void children(Builder *b, cmark_node *parent, guint depth, guint list_depth, gboolean compact) {
    for (cmark_node *n = cmark_node_first_child(parent); n && !b->failed; n = cmark_node_next(n))
        render(b, n, depth + 1, list_depth, compact);
}

static void render(Builder *b, cmark_node *n, guint depth, guint list_depth, gboolean compact) {
    if (++b->nodes > MAX_NODES || depth > MAX_DEPTH || b->out->len > MD_MAX_BYTES * 2u) {
        g_set_error_literal(b->error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This Markdown document is too complex to display.");
        b->failed = TRUE;
        return;
    }
    if ((b->nodes & 255) == 0 && b->cancel && g_cancellable_set_error_if_cancelled(b->cancel, b->error)) {
        b->failed = TRUE;
        return;
    }
    int start = b->offset;
    switch (cmark_node_get_type(n)) {
    case CMARK_NODE_DOCUMENT:
    case CMARK_NODE_ITEM:
        children(b, n, depth, list_depth, compact);
        break;
    case CMARK_NODE_PARAGRAPH:
        children(b, n, depth, list_depth, compact);
        line_gap(b, compact ? 1 : 2);
        break;
    case CMARK_NODE_HEADING: {
        gsize byte_start = b->out->len;
        children(b, n, depth, list_depth, compact);
        mark(b, MD_HEADING1 + cmark_node_get_heading_level(n) - 1, start, NULL);
        add_anchor(b, byte_start, start);
        line_gap(b, 2);
        break;
    }
    case CMARK_NODE_LIST: {
        int index = cmark_node_get_list_start(n);
        gboolean ordered = cmark_node_get_list_type(n) == CMARK_ORDERED_LIST;
        gboolean tight = cmark_node_get_list_tight(n);
        for (cmark_node *item = cmark_node_first_child(n); item && !b->failed; item = cmark_node_next(item)) {
            for (guint i = 0; i < list_depth; i++) append(b, "   ");
            if (ordered) {
                char number[32];
                g_snprintf(number, sizeof number, "%d.  ", index++);
                append(b, number);
            } else append(b, "•  ");
            render(b, item, depth + 1, list_depth + 1, tight);
            line_gap(b, tight ? 1 : 2);
        }
        line_gap(b, compact ? 1 : 2);
        break;
    }
    case CMARK_NODE_BLOCK_QUOTE:
        children(b, n, depth, list_depth, TRUE);
        mark(b, MD_QUOTE, start, NULL);
        line_gap(b, compact ? 1 : 2);
        break;
    case CMARK_NODE_CODE_BLOCK:
    case CMARK_NODE_HTML_BLOCK:
        append(b, cmark_node_get_literal(n));
        mark(b, MD_CODE_BLOCK, start, NULL);
        line_gap(b, 2);
        break;
    case CMARK_NODE_THEMATIC_BREAK:
        append(b, "────────────");
        mark(b, MD_RULE, start, NULL);
        line_gap(b, 2);
        break;
    case CMARK_NODE_STRONG:
    case CMARK_NODE_EMPH:
        children(b, n, depth, list_depth, compact);
        mark(b, cmark_node_get_type(n) == CMARK_NODE_STRONG ? MD_STRONG : MD_EMPHASIS, start, NULL);
        break;
    case CMARK_NODE_LINK:
        children(b, n, depth, list_depth, compact);
        mark(b, MD_LINK, start, cmark_node_get_url(n));
        break;
    case CMARK_NODE_IMAGE:
        append(b, "Image: ");
        children(b, n, depth, list_depth, compact);
        if (b->offset == start + 7) append(b, "open image");
        mark(b, MD_LINK, start, cmark_node_get_url(n));
        break;
    case CMARK_NODE_CODE:
    case CMARK_NODE_HTML_INLINE:
        append(b, cmark_node_get_literal(n));
        mark(b, MD_CODE, start, NULL);
        break;
    case CMARK_NODE_TEXT: append(b, cmark_node_get_literal(n)); break;
    case CMARK_NODE_LINEBREAK: append(b, "\n"); break;
    case CMARK_NODE_SOFTBREAK: append(b, " "); break;
    default: children(b, n, depth, list_depth, compact); break;
    }
}

MdDocument *md_parse(const char *source, gsize size, GCancellable *cancel, GError **error) {
    if (size > MD_MAX_BYTES || memchr(source, 0, size) || !g_utf8_validate(source, size, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Open a UTF-8 Markdown file up to 8 MiB.");
        return NULL;
    }
    if (size >= 3 && !memcmp(source, "\xef\xbb\xbf", 3)) { source += 3; size -= 3; }
    cmark_node *root = cmark_parse_document(source, size, CMARK_OPT_DEFAULT);
    if (!root) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not parse this Markdown file.");
        return NULL;
    }
    MdDocument *doc = g_new0(MdDocument, 1);
    doc->spans = g_array_new(FALSE, FALSE, sizeof(MdSpan));
    g_array_set_clear_func(doc->spans, clear_span);
    doc->anchors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    Builder b = {.doc=doc, .out=g_string_new(NULL), .cancel=cancel, .error=error};
    b.anchor_suffixes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    render(&b, root, 0, 0, FALSE);
    g_hash_table_unref(b.anchor_suffixes);
    cmark_node_free(root);
    if (b.failed) {
        g_string_free(b.out, TRUE);
        md_document_free(doc);
        return NULL;
    }
    doc->source = g_strndup(source, size);
    doc->text = g_string_free(b.out, FALSE);
    return doc;
}

MdDocument *md_load(GFile *file, GCancellable *cancel, GError **error) {
    if (!g_file_is_native(file)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Choose a local Markdown file.");
        return NULL;
    }
    g_autoptr(GFileInputStream) stream = g_file_read(file, cancel, error);
    if (!stream) return NULL;
    g_autoptr(GByteArray) buffer = g_byte_array_new();
    guint8 chunk[65536];
    for (;;) {
        gssize n = g_input_stream_read(G_INPUT_STREAM(stream), chunk, sizeof chunk, cancel, error);
        if (n < 0) return NULL;
        if (!n) break;
        if (buffer->len + n > MD_MAX_BYTES) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This file exceeds the 8 MiB Markdown limit.");
            return NULL;
        }
        g_byte_array_append(buffer, chunk, n);
    }
    return md_parse(buffer->len ? (const char *)buffer->data : "", buffer->len, cancel, error);
}
