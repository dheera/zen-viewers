/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "book.h"
#include <stdio.h>
#include <string.h>

static void string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static gpointer reader_thread(gpointer data) {
    ZenBook *book = data;
    for (guint i = 0; i < 12; i++) {
        GError *error = NULL;
        ZenBookDocument *doc = zen_book_load_chapter(book, i % book->chapters->len, NULL, &error);
        g_assert_no_error(error); g_assert_nonnull(doc);
        if (doc->images->len) {
            ZenBookImage *image = g_ptr_array_index(doc->images, 0);
            GBytes *bytes = zen_book_read_resource(book, image->path, NULL, &error);
            g_assert_no_error(error); g_assert_nonnull(bytes); g_bytes_unref(bytes);
        }
        zen_book_document_free(doc);
    }
    zen_book_unref(book); return NULL;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    GError *error = NULL;
    GFile *file = g_file_new_for_path(argv[2]);
    GCancellable *cancel = g_cancellable_new(); g_cancellable_cancel(cancel);
    ZenBook *book = zen_book_open(file, cancel, &error);
    g_assert_null(book); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error);
    book = zen_book_open(file, NULL, &error); g_object_unref(file);
    if (!book) goto failure;
    if (!strcmp(argv[1], "resource") && argc == 4) {
        GBytes *bytes = zen_book_read_resource(book, argv[3], NULL, &error);
        if (!bytes) goto failure;
        printf("%zu\n", g_bytes_get_size(bytes)); g_bytes_unref(bytes);
    } else if (!strcmp(argv[1], "threads")) {
        GThread *threads[4];
        for (guint i = 0; i < 4; i++) threads[i] = g_thread_new("book-read", reader_thread, zen_book_ref(book));
        for (guint i = 0; i < 4; i++) g_thread_join(threads[i]);
    } else {
        printf("{\"title\":"); string(book->title); printf(",\"author\":"); string(book->author);
        printf(",\"kind\":%d,\"toc\":[", book->kind);
        for (guint i = 0; i < book->toc->len; i++) {
            ZenBookTocEntry *entry = g_ptr_array_index(book->toc, i);
            if (i) putchar(',');
            printf("{\"title\":"); string(entry->title);
            printf(",\"target\":"); string(entry->target); printf(",\"depth\":%u}", entry->depth);
        }
        printf("],\"chapters\":[");
        for (guint c = 0; c < book->chapters->len; c++) {
            ZenBookChapter *chapter = g_ptr_array_index(book->chapters, c);
            if (c) putchar(',');
            printf("{\"path\":"); string(chapter->path);
            printf(",\"title\":"); string(chapter->title);
            if (!strcmp(argv[1], "dump")) {
                ZenBookDocument *doc = zen_book_load_chapter(book, c, cancel, &error);
                g_assert_null(doc); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error);
                doc = zen_book_load_chapter(book, c, NULL, &error);
                if (!doc) goto failure;
                g_assert_true(g_utf8_validate(doc->text, -1, NULL));
                g_assert_cmpuint(doc->characters, ==, g_utf8_strlen(doc->text, -1));
                printf(",\"text\":"); string(doc->text); printf(",\"spans\":[");
                for (guint i = 0; i < doc->spans->len; i++) {
                    ZenBookSpan *span = g_ptr_array_index(doc->spans, i);
                    g_assert_cmpuint(span->start, <=, span->end); g_assert_cmpuint(span->end, <=, doc->characters);
                    printf("%s[%u,%u,%u,%u]", i ? "," : "", span->start, span->end, span->flags, span->heading_level);
                }
                printf("],\"links\":[");
                for (guint i = 0; i < doc->links->len; i++) {
                    ZenBookLink *link = g_ptr_array_index(doc->links, i);
                    g_assert_cmpuint(link->start, <=, link->end); g_assert_cmpuint(link->end, <=, doc->characters);
                    char *fragment = NULL; gint index = zen_book_find_chapter(book, link->target, &fragment);
                    printf("%s{\"start\":%u,\"end\":%u,\"external\":%s,\"chapter\":%d,\"target\":", i ? "," : "", link->start, link->end, link->external ? "true" : "false", index);
                    string(link->target); printf(",\"fragment\":"); string(fragment); putchar('}'); g_free(fragment);
                }
                printf("],\"images\":[");
                for (guint i = 0; i < doc->images->len; i++) {
                    ZenBookImage *image = g_ptr_array_index(doc->images, i);
                    g_assert_cmpuint(image->offset, <, doc->characters);
                    g_assert_cmpuint(g_utf8_get_char(g_utf8_offset_to_pointer(doc->text, image->offset)), ==, 0xfffc);
                    printf("%s{\"offset\":%u,\"path\":", i ? "," : "", image->offset); string(image->path); putchar('}');
                }
                printf("],\"anchors\":[");
                for (guint i = 0; i < doc->anchors->len; i++) {
                    ZenBookAnchor *anchor = g_ptr_array_index(doc->anchors, i);
                    g_assert_cmpuint(anchor->offset, <=, doc->characters);
                    printf("%s{\"offset\":%u,\"id\":", i ? "," : "", anchor->offset); string(anchor->id); putchar('}');
                }
                putchar(']'); zen_book_document_free(doc);
            }
            putchar('}');
        }
        printf("]}\n");
    }
    zen_book_unref(book); g_object_unref(cancel); return 0;
failure:
    g_printerr("%s\n", error ? error->message : "Book failed without an error");
    g_clear_error(&error); zen_book_unref(book); g_object_unref(cancel); return 1;
}
