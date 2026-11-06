/* SPDX-License-Identifier: GPL-3.0-or-later */
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_6
#include "zen-ui.h"
#include "book.h"
#include <math.h>
#include <string.h>

typedef struct {
    int chapter, offset, zoom;
    gboolean night;
} ReadingPlace;

typedef struct {
    GFile *file;
    ZenBook *book;
    int chapter;
    char *fragment;
    gboolean end;
} Request;

typedef struct {
    ZenBook *book;
    ZenBookDocument *document;
    GdkPixbuf *comic;
    GPtrArray *images;
    ReadingPlace place;
} Page;

typedef struct {
    ZenWindow *shell;
    GtkStack *canvas;
    GtkTextView *text;
    GtkScrolledWindow *scroll;
    GtkPicture *comic;
    GtkWidget *contents, *contents_list, *contents_toggle;
    GtkWidget *previous, *next, *counter, *smaller, *larger, *night_toggle;
    GtkSingleSelection *toc_selection;
    GtkTextTag *font, *headings[6], *styles[6], *link;
    GPtrArray *artwork;
    ZenBook *book;
    ZenBookDocument *document;
    GFile *file;
    GCancellable *loading;
    int chapter, requested, zoom, layout_width, restore_offset;
    guint save_timer, reflow_idle, restore_tick, restore_frames;
    gboolean opening, night, restore_end;
    double press_x, press_y;
} Reader;

static const guint style_flags[] = {ZEN_BOOK_BOLD, ZEN_BOOK_ITALIC, ZEN_BOOK_MONOSPACE,
                                   ZEN_BOOK_UNDERLINE, ZEN_BOOK_STRIKE, ZEN_BOOK_QUOTE};

typedef struct {
    GtkWidget parent;
    GdkTexture *texture;
    int maximum_width;
} BookArt;
typedef struct { GtkWidgetClass parent; } BookArtClass;
G_DEFINE_TYPE(BookArt, book_art, GTK_TYPE_WIDGET)

static void art_measure(GtkWidget *widget, GtkOrientation orientation, int for_size,
                        int *minimum, int *natural, int *min_baseline, int *nat_baseline) {
    (void)for_size;
    BookArt *art = (BookArt *)widget;
    int w = gdk_texture_get_width(art->texture), h = gdk_texture_get_height(art->texture);
    double scale = MIN(1., MIN((double)art->maximum_width / w, 430. / h));
    *minimum = *natural = MAX(1, (int)((orientation == GTK_ORIENTATION_HORIZONTAL ? w : h) * scale));
    *min_baseline = *nat_baseline = -1;
}
static void art_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    BookArt *art = (BookArt *)widget;
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, gtk_widget_get_width(widget), gtk_widget_get_height(widget));
    gtk_snapshot_append_texture(snapshot, art->texture, &bounds);
}
static void art_dispose(GObject *object) {
    g_clear_object(&((BookArt *)object)->texture);
    G_OBJECT_CLASS(book_art_parent_class)->dispose(object);
}
static void book_art_class_init(BookArtClass *klass) {
    GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
    widget->measure = art_measure;
    widget->snapshot = art_snapshot;
    G_OBJECT_CLASS(klass)->dispose = art_dispose;
    gtk_widget_class_set_accessible_role(widget, GTK_ACCESSIBLE_ROLE_IMG);
}
static void book_art_init(BookArt *art) { art->maximum_width = 560; }

static int margin_for_width(int width) {
    return MAX(24, (width - 680) / 2);
}
static gboolean reflow(gpointer data) {
    Reader *r = data;
    r->reflow_idle = 0;
    if (zen_window_is_closed(r->shell)) return G_SOURCE_REMOVE;
    int width = gtk_widget_get_width(GTK_WIDGET(r->text));
    if (width < 1 || width == r->layout_width) return G_SOURCE_REMOVE;
    r->layout_width = width;
    int margin = margin_for_width(width);
    gtk_text_view_set_left_margin(r->text, margin);
    gtk_text_view_set_right_margin(r->text, margin);
    for (guint i = 0; i < r->artwork->len; i++) {
        BookArt *art = g_ptr_array_index(r->artwork, i);
        art->maximum_width = MAX(1, width - 2 * margin);
        gtk_widget_queue_resize(GTK_WIDGET(art));
    }
    return G_SOURCE_REMOVE;
}
typedef struct { GtkTextView parent; } BookText;
typedef struct { GtkTextViewClass parent; } BookTextClass;
G_DEFINE_TYPE(BookText, book_text, GTK_TYPE_TEXT_VIEW)
static void allocate_text(GtkWidget *widget, int width, int height, int baseline) {
    GTK_WIDGET_CLASS(book_text_parent_class)->size_allocate(widget, width, height, baseline);
    Reader *r = g_object_get_data(G_OBJECT(widget), "zen-reader");
    if (r && width != r->layout_width && !r->reflow_idle)
        r->reflow_idle = g_idle_add(reflow, r);
}
static void book_text_class_init(BookTextClass *klass) {
    GTK_WIDGET_CLASS(klass)->size_allocate = allocate_text;
}
static void book_text_init(BookText *text) { (void)text; }

static char *position_path(GFile *file) {
    g_autofree char *uri = g_file_get_uri(file);
    g_autofree char *name = g_compute_checksum_for_string(G_CHECKSUM_SHA256, uri, -1);
    return g_build_filename(g_get_user_state_dir(), "zen-books", name, NULL);
}
static ReadingPlace read_position(GFile *file) {
    g_autofree char *path = position_path(file);
    g_autoptr(GKeyFile) state = g_key_file_new();
    ReadingPlace place = {.zoom = 100};
    if (g_key_file_load_from_file(state, path, G_KEY_FILE_NONE, NULL)) {
        place.chapter = MAX(0, g_key_file_get_integer(state, "Reading", "chapter", NULL));
        place.offset = MAX(0, g_key_file_get_integer(state, "Reading", "offset", NULL));
        int zoom = g_key_file_get_integer(state, "Reading", "zoom", NULL);
        if (zoom) place.zoom = CLAMP(zoom, 70, 190);
        place.night = g_key_file_get_boolean(state, "Reading", "night", NULL);
    }
    return place;
}
static int visible_offset(Reader *r) {
    if (!r->document) return 0;
    GdkRectangle area;
    GtkTextIter iter;
    gtk_text_view_get_visible_rect(r->text, &area);
    gtk_text_view_get_line_at_y(r->text, &iter, area.y + 2, NULL);
    return gtk_text_iter_get_offset(&iter);
}
static void save_position(Reader *r) {
    if (!r->file || !r->book) return;
    g_autofree char *path = position_path(r->file);
    g_autofree char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0) return;
    g_autoptr(GKeyFile) state = g_key_file_new();
    g_key_file_set_integer(state, "Reading", "chapter", r->chapter);
    g_key_file_set_integer(state, "Reading", "offset", visible_offset(r));
    g_key_file_set_integer(state, "Reading", "zoom", r->zoom);
    g_key_file_set_boolean(state, "Reading", "night", r->night);
    gsize size;
    g_autofree char *contents = g_key_file_to_data(state, &size, NULL);
    g_file_set_contents_full(path, contents, size, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}
static gboolean save_later(gpointer data) {
    Reader *r = data;
    r->save_timer = 0;
    save_position(r);
    return G_SOURCE_REMOVE;
}
static void schedule_save(Reader *r) {
    if (!r->book || zen_window_is_closed(r->shell)) return;
    g_clear_handle_id(&r->save_timer, g_source_remove);
    r->save_timer = g_timeout_add(650, save_later, r);
}
static void scroll_changed(GtkAdjustment *adjustment, gpointer data) {
    (void)adjustment;
    schedule_save(data);
}
static void scroll_to_offset(Reader *r, int offset, gboolean end) {
    if (!end && offset <= 0) {
        gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(r->scroll), 0.);
        return;
    }
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(r->text);
    GtkTextIter iter;
    int length = gtk_text_buffer_get_char_count(buffer);
    gtk_text_buffer_get_iter_at_offset(buffer, &iter, end ? length : CLAMP(offset, 0, length));
    GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "zen-reading-place");
    if (mark) gtk_text_buffer_move_mark(buffer, mark, &iter);
    else mark = gtk_text_buffer_create_mark(buffer, "zen-reading-place", &iter, TRUE);
    gtk_text_view_scroll_to_mark(r->text, mark, 0., TRUE, 0., end ? 1. : 0.);
}
static void cancel_restore(Reader *r) {
    if (r->restore_tick) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(r->text), r->restore_tick);
        r->restore_tick = 0;
    }
}
static gboolean restore_position(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)widget;
    (void)clock;
    Reader *r = data;
    if (r->restore_frames) { r->restore_frames--; return G_SOURCE_CONTINUE; }
    if (r->reflow_idle) return G_SOURCE_CONTINUE;
    r->restore_tick = 0;
    scroll_to_offset(r, r->restore_offset, r->restore_end);
    return G_SOURCE_REMOVE;
}
static void restore_after_layout(Reader *r, int offset, gboolean end) {
    cancel_restore(r);
    r->restore_offset = offset;
    r->restore_end = end;
    /* Let the new chapter take its allocation and apply the reading margins. */
    r->restore_frames = 2;
    r->restore_tick = gtk_widget_add_tick_callback(GTK_WIDGET(r->text), restore_position, r, NULL);
}
static int anchor_offset(ZenBookDocument *document, const char *fragment) {
    if (fragment) for (guint i = 0; i < document->anchors->len; i++) {
        ZenBookAnchor *anchor = g_ptr_array_index(document->anchors, i);
        if (!strcmp(anchor->id, fragment)) return (int)anchor->offset;
    }
    return 0;
}
static void set_font(Reader *r, int percent) {
    r->zoom = CLAMP(percent, 70, 190);
    g_object_set(r->font, "size-points", 14. * r->zoom / 100., NULL);
    gtk_widget_set_sensitive(r->smaller, r->book && r->book->kind == ZEN_BOOK_EPUB && r->zoom > 70);
    gtk_widget_set_sensitive(r->larger, r->book && r->book->kind == ZEN_BOOK_EPUB && r->zoom < 190);
}
static void zoom(Reader *r, int step) {
    if (!r->document) return;
    int offset = visible_offset(r);
    set_font(r, r->zoom + step);
    restore_after_layout(r, offset, FALSE);
    schedule_save(r);
}
static void night_toggled(GtkToggleButton *button, gpointer data) {
    Reader *r = data;
    r->night = gtk_toggle_button_get_active(button);
    if (r->night) gtk_widget_add_css_class(GTK_WIDGET(r->text), "night");
    else gtk_widget_remove_css_class(GTK_WIDGET(r->text), "night");
    g_object_set(r->link, "foreground", r->night ? "#aadcc8" : "#256d55", NULL);
    g_object_set(r->styles[5], "foreground", r->night ? "#b9cbc0" : "#50675c", NULL);
    schedule_save(r);
}
static void make_tags(Reader *r) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(r->text);
    r->font = gtk_text_buffer_create_tag(buffer, NULL, "family", "Noto Serif, DejaVu Serif, serif",
                                       "size-points", 14., NULL);
    const double sizes[] = {2., 1.58, 1.30, 1.15, 1.08, 1.};
    for (guint i = 0; i < G_N_ELEMENTS(sizes); i++)
        r->headings[i] = gtk_text_buffer_create_tag(buffer, NULL, "scale", sizes[i],
            "weight", PANGO_WEIGHT_BOLD, "pixels-above-lines", 7, NULL);
    r->styles[0] = gtk_text_buffer_create_tag(buffer, NULL, "weight", PANGO_WEIGHT_BOLD, NULL);
    r->styles[1] = gtk_text_buffer_create_tag(buffer, NULL, "style", PANGO_STYLE_ITALIC, NULL);
    r->styles[2] = gtk_text_buffer_create_tag(buffer, NULL, "family", "monospace", "scale", .92, NULL);
    r->styles[3] = gtk_text_buffer_create_tag(buffer, NULL, "underline", PANGO_UNDERLINE_SINGLE, NULL);
    r->styles[4] = gtk_text_buffer_create_tag(buffer, NULL, "strikethrough", TRUE, NULL);
    r->styles[5] = gtk_text_buffer_create_tag(buffer, NULL, "style", PANGO_STYLE_ITALIC,
                                             "foreground", "#50675c", NULL);
    r->link = gtk_text_buffer_create_tag(buffer, NULL, "foreground", "#256d55",
                                        "underline", PANGO_UNDERLINE_SINGLE, NULL);
}
static void show_document(Reader *r, Page *page, int offset, gboolean end) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(r->text);
    ZenBookDocument *doc = r->document;
    g_ptr_array_set_size(r->artwork, 0);
    gtk_text_buffer_set_text(buffer, doc->text, -1);
    for (guint i = 0; i < doc->images->len; i++) {
        ZenBookImage *image = g_ptr_array_index(doc->images, i);
        GdkPixbuf *pixbuf = g_ptr_array_index(page->images, i);
        GtkWidget *art;
        if (pixbuf) {
            BookArt *photo = g_object_new(book_art_get_type(), NULL);
            photo->texture = gdk_texture_new_for_pixbuf(pixbuf);
            if (r->layout_width)
                photo->maximum_width = MAX(1, r->layout_width - 2 * margin_for_width(r->layout_width));
            art = GTK_WIDGET(photo);
            g_ptr_array_add(r->artwork, art);
        } else {
            art = gtk_label_new(image->alt && *image->alt ? image->alt : "Illustration unavailable");
            gtk_label_set_ellipsize(GTK_LABEL(art), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(art), 32);
            gtk_widget_add_css_class(art, "dim-label");
        }
        if (image->alt && *image->alt)
            gtk_accessible_update_property(GTK_ACCESSIBLE(art), GTK_ACCESSIBLE_PROPERTY_LABEL, image->alt, -1);
        GtkTextIter begin, next;
        gtk_text_buffer_get_iter_at_offset(buffer, &begin, image->offset);
        next = begin;
        gtk_text_iter_forward_char(&next);
        gtk_text_buffer_delete(buffer, &begin, &next);
        GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(buffer, &begin);
        gtk_text_view_add_child_at_anchor(r->text, art, anchor);
    }
    GtkTextIter begin, next;
    gtk_text_buffer_get_bounds(buffer, &begin, &next);
    gtk_text_buffer_apply_tag(buffer, r->font, &begin, &next);
    for (guint i = 0; i < doc->spans->len; i++) {
        ZenBookSpan *span = g_ptr_array_index(doc->spans, i);
        gtk_text_buffer_get_iter_at_offset(buffer, &begin, span->start);
        gtk_text_buffer_get_iter_at_offset(buffer, &next, span->end);
        if (span->heading_level >= 1 && span->heading_level <= 6)
            gtk_text_buffer_apply_tag(buffer, r->headings[span->heading_level - 1], &begin, &next);
        for (guint j = 0; j < G_N_ELEMENTS(style_flags); j++)
            if (span->flags & style_flags[j]) gtk_text_buffer_apply_tag(buffer, r->styles[j], &begin, &next);
    }
    for (guint i = 0; i < doc->links->len; i++) {
        ZenBookLink *link = g_ptr_array_index(doc->links, i);
        gtk_text_buffer_get_iter_at_offset(buffer, &begin, link->start);
        gtk_text_buffer_get_iter_at_offset(buffer, &next, link->end);
        gtk_text_buffer_apply_tag(buffer, r->link, &begin, &next);
    }
    gtk_text_buffer_get_start_iter(buffer, &begin);
    gtk_text_buffer_place_cursor(buffer, &begin);
    gtk_stack_set_visible_child_name(r->canvas, "reading");
    restore_after_layout(r, offset, end);
}

static void request_free(gpointer data) {
    Request *request = data;
    g_clear_object(&request->file);
    zen_book_unref(request->book);
    g_free(request->fragment);
    g_free(request);
}
static void pixbuf_unref(gpointer data) {
    if (data) g_object_unref(data);
}
static void page_free(gpointer data) {
    Page *page = data;
    if (!page) return;
    zen_book_unref(page->book);
    zen_book_document_free(page->document);
    g_clear_object(&page->comic);
    g_clear_pointer(&page->images, g_ptr_array_unref);
    g_free(page);
}
static void bound_picture(GdkPixbufLoader *loader, int w, int h, gpointer data) {
    int limit = GPOINTER_TO_INT(data);
    if (w > limit || h > limit) {
        double scale = (double)limit / MAX(w, h);
        gdk_pixbuf_loader_set_size(loader, MAX(1, (int)(w * scale)), MAX(1, (int)(h * scale)));
    }
}
static GdkPixbuf *read_picture(ZenBook *book, const char *path, int limit,
                               GCancellable *cancel, GError **error) {
    g_autoptr(GBytes) bytes = zen_book_read_resource(book, path, cancel, error);
    if (!bytes || g_cancellable_set_error_if_cancelled(cancel, error)) return NULL;
    g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new();
    g_signal_connect(loader, "size-prepared", G_CALLBACK(bound_picture), GINT_TO_POINTER(limit));
    gsize length;
    const guchar *contents = g_bytes_get_data(bytes, &length);
    gboolean okay = gdk_pixbuf_loader_write(loader, contents, length, error);
    if (!gdk_pixbuf_loader_close(loader, okay ? error : NULL) || !okay) return NULL;
    GdkPixbuf *image = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!image) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "This book image could not be decoded.");
        return NULL;
    }
    return gdk_pixbuf_apply_embedded_orientation(image);
}
static void read_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    Request *request = data;
    GError *error = NULL;
    Page *page = g_new0(Page, 1);
    page->book = request->file ? zen_book_open(request->file, cancel, &error) : zen_book_ref(request->book);
    if (!page->book) goto failed;
    page->place = request->file ? read_position(request->file) : (ReadingPlace){.chapter = request->chapter};
    page->place.chapter = CLAMP(page->place.chapter, 0, (int)page->book->chapters->len - 1);
    if (page->book->kind == ZEN_BOOK_CBZ) {
        ZenBookChapter *chapter = g_ptr_array_index(page->book->chapters, page->place.chapter);
        page->comic = read_picture(page->book, chapter->path, 3000, cancel, &error);
        if (!page->comic) goto failed;
    } else {
        page->document = zen_book_load_chapter(page->book, page->place.chapter, cancel, &error);
        if (!page->document) goto failed;
        page->images = g_ptr_array_new_with_free_func(pixbuf_unref);
        guint64 pixels = 0;
        for (guint i = 0; i < page->document->images->len; i++) {
            if (g_cancellable_set_error_if_cancelled(cancel, &error)) goto failed;
            ZenBookImage *image = g_ptr_array_index(page->document->images, i);
            GdkPixbuf *pixbuf = NULL;
            if (image->path && pixels < 20u * 1024 * 1024)
                pixbuf = read_picture(page->book, image->path, 1400, cancel, NULL);
            if (pixbuf)
                pixels += (guint64)gdk_pixbuf_get_width(pixbuf) * gdk_pixbuf_get_height(pixbuf);
            g_ptr_array_add(page->images, pixbuf);
        }
    }
    g_task_return_pointer(task, page, page_free);
    return;
failed:
    page_free(page);
    if (error) g_task_return_error(task, error);
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "This book page could not be read.");
}

static void update_navigation(Reader *r) {
    int count = (int)r->book->chapters->len;
    int index = r->loading && !r->opening ? r->requested : r->chapter;
    gtk_widget_set_sensitive(r->previous, index > 0);
    gtk_widget_set_sensitive(r->next, index + 1 < count);
    char text[32];
    g_snprintf(text, sizeof text, "%d / %d", r->chapter + 1, count);
    gtk_label_set_text(GTK_LABEL(r->counter), text);
}
static void select_contents(Reader *r) {
    guint row = r->chapter;
    if (r->book->toc->len) {
        row = GTK_INVALID_LIST_POSITION;
        for (guint i = 0; i < r->book->toc->len; i++) {
            ZenBookTocEntry *entry = g_ptr_array_index(r->book->toc, i);
            if (zen_book_find_chapter(r->book, entry->target, NULL) == r->chapter) { row = i; break; }
        }
    }
    gtk_single_selection_set_selected(r->toc_selection, row);
}
static void populate_contents(Reader *r) {
    GtkStringList *names = gtk_string_list_new(NULL);
    if (r->book->toc->len) {
        for (guint i = 0; i < r->book->toc->len; i++) {
            ZenBookTocEntry *entry = g_ptr_array_index(r->book->toc, i);
            g_autofree char *title = g_strdup_printf("%*s%s", (int)MIN(entry->depth, 4u) * 2, "", entry->title);
            gtk_string_list_append(names, title);
        }
    } else {
        for (guint i = 0; i < r->book->chapters->len; i++) {
            ZenBookChapter *chapter = g_ptr_array_index(r->book->chapters, i);
            gtk_string_list_append(names, chapter->title);
        }
    }
    gtk_single_selection_set_model(r->toc_selection, G_LIST_MODEL(names));
    g_object_unref(names);
}
static void page_loaded(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    Reader *r = g_object_get_data(source, "zen-books-reader");
    GTask *task = G_TASK(result);
    Request *request = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    Page *page = g_task_propagate_pointer(task, &error);
    if (r->loading == g_task_get_cancellable(task) && !zen_window_is_closed(r->shell)) {
        g_clear_object(&r->loading);
        r->opening = FALSE;
        if (page) {
            if (request->file) {
                g_clear_handle_id(&r->save_timer, g_source_remove);
                g_set_object(&r->file, request->file);
                zen_book_unref(r->book);
                r->book = zen_book_ref(page->book);
                zen_window_set_title(r->shell, r->book->title);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->night_toggle), page->place.night);
                gtk_widget_set_visible(r->night_toggle, r->book->kind == ZEN_BOOK_EPUB);
                gtk_widget_set_visible(r->smaller, r->book->kind == ZEN_BOOK_EPUB);
                gtk_widget_set_visible(r->larger, r->book->kind == ZEN_BOOK_EPUB);
                set_font(r, page->place.zoom);
                populate_contents(r);
            }
            r->chapter = r->requested = page->place.chapter;
            zen_book_document_free(r->document);
            r->document = g_steal_pointer(&page->document);
            if (r->document) {
                gtk_picture_set_paintable(r->comic, NULL);
                int offset = request->fragment ? anchor_offset(r->document, request->fragment) : page->place.offset;
                show_document(r, page, offset, request->end);
                gtk_widget_grab_focus(GTK_WIDGET(r->text));
            } else {
                cancel_restore(r);
                g_ptr_array_set_size(r->artwork, 0);
                gtk_text_buffer_set_text(gtk_text_view_get_buffer(r->text), "", 0);
                GdkTexture *texture = gdk_texture_new_for_pixbuf(page->comic);
                gtk_picture_set_paintable(r->comic, GDK_PAINTABLE(texture));
                g_object_unref(texture);
                gtk_stack_set_visible_child_name(r->canvas, "comic");
                gtk_widget_grab_focus(GTK_WIDGET(r->comic));
            }
            select_contents(r);
            zen_window_set_ready(r->shell, TRUE);
            zen_window_clear_notice(r->shell);
            schedule_save(r);
        } else if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            r->requested = r->chapter;
            zen_window_notice(r->shell, error->message);
        }
        if (r->book) update_navigation(r);
        zen_window_hide_progress(r->shell);
    }
    page_free(page);
}
static void read_page(Reader *r, Request *request) {
    if (r->loading) g_cancellable_cancel(r->loading);
    g_clear_object(&r->loading);
    r->loading = g_cancellable_new();
    r->opening = request->file != NULL;
    GTask *task = g_task_new(zen_window_get_window(r->shell), r->loading, page_loaded, NULL);
    g_task_set_task_data(task, request, request_free);
    zen_window_clear_notice(r->shell);
    zen_window_show_progress(r->shell, -1);
    g_task_run_in_thread(task, read_worker);
    g_object_unref(task);
}
static void open_book(ZenWindow *shell, GFile *file, gpointer data) {
    (void)shell;
    Reader *r = data;
    save_position(r);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->contents_toggle), FALSE);
    Request *request = g_new0(Request, 1);
    request->file = g_object_ref(file);
    read_page(r, request);
}
static void go_to(Reader *r, int chapter, const char *fragment, gboolean end) {
    if (!r->book || r->opening || chapter < 0 || chapter >= (int)r->book->chapters->len) return;
    if (!r->loading && chapter == r->chapter) {
        if (r->document) scroll_to_offset(r, anchor_offset(r->document, fragment), end);
        return;
    }
    Request *request = g_new0(Request, 1);
    request->book = zen_book_ref(r->book);
    request->chapter = r->requested = chapter;
    request->fragment = g_strdup(fragment);
    request->end = end;
    read_page(r, request);
    update_navigation(r);
}
static void step(Reader *r, int amount, gboolean end) {
    go_to(r, (r->loading ? r->requested : r->chapter) + amount, NULL, end);
}
static void scroll_page(Reader *r, int direction) {
    if (!r->document || r->loading) { step(r, direction, direction < 0); return; }
    cancel_restore(r);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(r->scroll);
    double value = gtk_adjustment_get_value(adjustment);
    double size = gtk_adjustment_get_page_size(adjustment);
    double bottom = MAX(0., gtk_adjustment_get_upper(adjustment) - size);
    if ((direction < 0 && value < 1.) || (direction > 0 && value >= bottom - 1.))
        step(r, direction, direction < 0);
    else gtk_adjustment_set_value(adjustment, CLAMP(value + direction * size * .87, 0., bottom));
}
static void follow_target(Reader *r, const char *target) {
    g_autofree char *fragment = NULL;
    int chapter = zen_book_find_chapter(r->book, target, &fragment);
    if (chapter < 0) zen_window_notice(r->shell, "This link points outside the book’s reading order.");
    else go_to(r, chapter, fragment, FALSE);
}
static gboolean iter_at(Reader *r, double x, double y, GtkTextIter *iter) {
    int bx, by;
    gtk_text_view_window_to_buffer_coords(r->text, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    return gtk_text_view_get_iter_at_location(r->text, iter, bx, by);
}
static gboolean claim_text(double x, double y, gpointer data) {
    Reader *r = data;
    if (!r->document) return FALSE;
    graphene_point_t point = GRAPHENE_POINT_INIT(x, y), local;
    GtkTextIter iter;
    return gtk_widget_compute_point(zen_window_get_root(r->shell), GTK_WIDGET(r->text), &point, &local) &&
           iter_at(r, local.x, local.y, &iter);
}
static void link_press(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
    (void)gesture;
    (void)n;
    Reader *r = data;
    r->press_x = x;
    r->press_y = y;
}
static void link_release(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
    (void)gesture;
    Reader *r = data;
    GtkTextIter iter;
    if (n != 1 || !r->document || r->loading || fabs(r->press_x-x) >= 4 || fabs(r->press_y-y) >= 4 ||
        gtk_text_buffer_get_has_selection(gtk_text_view_get_buffer(r->text)) || !iter_at(r, x, y, &iter) ||
        !gtk_text_iter_has_tag(&iter, r->link)) return;
    guint offset = (guint)gtk_text_iter_get_offset(&iter);
    for (guint i = 0; i < r->document->links->len; i++) {
        ZenBookLink *link = g_ptr_array_index(r->document->links, i);
        if (offset < link->start || offset >= link->end) continue;
        if (link->external) {
            g_autofree char *scheme = g_uri_parse_scheme(link->target);
            if (scheme && (!g_ascii_strcasecmp(scheme, "http") || !g_ascii_strcasecmp(scheme, "https") ||
                           !g_ascii_strcasecmp(scheme, "mailto")))
                gtk_show_uri(zen_window_get_window(r->shell), link->target, GDK_CURRENT_TIME);
        } else follow_target(r, link->target);
        break;
    }
}
static void link_motion(GtkEventControllerMotion *controller, double x, double y, gpointer data) {
    (void)controller;
    Reader *r = data;
    GtkTextIter iter;
    gboolean over = r->document && iter_at(r, x, y, &iter) && gtk_text_iter_has_tag(&iter, r->link);
    gtk_widget_set_cursor_from_name(GTK_WIDGET(r->text), over ? "pointer" : "text");
}

static void contents_toggled(GtkToggleButton *button, gpointer data) {
    Reader *r = data;
    gboolean active = gtk_toggle_button_get_active(button);
    gtk_revealer_set_reveal_child(GTK_REVEALER(r->contents), active);
    if (!active && r->book)
        gtk_widget_grab_focus(r->document ? GTK_WIDGET(r->text) : GTK_WIDGET(r->comic));
}
static void contents_revealed(GObject *revealer, GParamSpec *property, gpointer data) {
    (void)property;
    Reader *r = data;
    if (gtk_revealer_get_child_revealed(GTK_REVEALER(revealer)))
        gtk_widget_grab_focus(r->contents_list);
}
static void close_contents(GtkButton *button, gpointer data) {
    (void)button;
    Reader *r = data;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->contents_toggle), FALSE);
}
static void contents_activated(GtkListView *view, guint index, gpointer data) {
    (void)view;
    Reader *r = data;
    if (!r->book) return;
    close_contents(NULL, r);
    if (r->book->toc->len) {
        if (index >= r->book->toc->len) return;
        ZenBookTocEntry *entry = g_ptr_array_index(r->book->toc, index);
        follow_target(r, entry->target);
    } else go_to(r, (int)index, NULL, FALSE);
}
static void toc_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory;
    (void)data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(label, 14);
    gtk_widget_set_margin_end(label, 10);
    gtk_widget_set_margin_top(label, 10);
    gtk_widget_set_margin_bottom(label, 10);
    gtk_list_item_set_child(item, label);
}
static void toc_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory;
    (void)data;
    GtkStringObject *name = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    GtkWidget *label = gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(name));
    gtk_widget_set_tooltip_text(label, gtk_string_object_get_string(name));
}
static void make_contents(Reader *r, GtkOverlay *overlay) {
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(panel, "zen-book-contents");
    gtk_widget_set_size_request(panel, 215, -1);
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(heading, 14);
    gtk_widget_set_margin_end(heading, 8);
    gtk_widget_set_margin_top(heading, 6);
    GtkWidget *title = gtk_label_new("Contents");
    gtk_label_set_xalign(GTK_LABEL(title), 0.);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_add_css_class(title, "heading");
    gtk_box_append(GTK_BOX(heading), title);
    gtk_box_append(GTK_BOX(heading), zen_button_new("window-close-symbolic", "Hide contents",
                                                  G_CALLBACK(close_contents), r));
    gtk_box_append(GTK_BOX(panel), heading);
    r->toc_selection = gtk_single_selection_new(NULL);
    gtk_single_selection_set_autoselect(r->toc_selection, FALSE);
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(toc_setup), r);
    g_signal_connect(factory, "bind", G_CALLBACK(toc_bind), r);
    r->contents_list = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(r->toc_selection)), factory);
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(r->contents_list), TRUE);
    g_signal_connect(r->contents_list, "activate", G_CALLBACK(contents_activated), r);
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), r->contents_list);
    gtk_box_append(GTK_BOX(panel), scrolled);
    r->contents = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(r->contents), panel);
    gtk_revealer_set_transition_type(GTK_REVEALER(r->contents), GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(r->contents), 180);
    gtk_widget_set_halign(r->contents, GTK_ALIGN_START);
    gtk_widget_set_valign(r->contents, GTK_ALIGN_FILL);
    gtk_widget_set_margin_start(r->contents, 12);
    gtk_widget_set_margin_top(r->contents, 68);
    gtk_widget_set_margin_bottom(r->contents, 68);
    g_signal_connect(r->contents, "notify::child-revealed", G_CALLBACK(contents_revealed), r);
    gtk_overlay_add_overlay(overlay, r->contents);
}
static void previous_clicked(GtkButton *button, gpointer data) { (void)button; step(data, -1, FALSE); }
static void next_clicked(GtkButton *button, gpointer data) { (void)button; step(data, 1, FALSE); }
static void smaller_clicked(GtkButton *button, gpointer data) { (void)button; zoom(data, -10); }
static void larger_clicked(GtkButton *button, gpointer data) { (void)button; zoom(data, 10); }
static GtkWidget *toggle_button(const char *label, const char *tip, GCallback callback, Reader *r) {
    GtkWidget *widget = gtk_toggle_button_new_with_label(label);
    gtk_widget_set_tooltip_text(widget, tip);
    gtk_widget_set_focus_on_click(widget, FALSE);
    g_signal_connect(widget, "toggled", callback, r);
    return widget;
}
static void make_controls(Reader *r) {
    GtkBox *bar = GTK_BOX(zen_window_get_controls(r->shell));
    r->contents_toggle = toggle_button("Contents", "Contents (Ctrl+B)", G_CALLBACK(contents_toggled), r);
    r->previous = zen_button_new("go-previous-symbolic", "Previous chapter or page (Left)", G_CALLBACK(previous_clicked), r);
    r->counter = gtk_label_new(NULL);
    gtk_widget_add_css_class(r->counter, "muted");
    r->next = zen_button_new("go-next-symbolic", "Next chapter or page (Right)", G_CALLBACK(next_clicked), r);
    r->smaller = zen_button_new("zoom-out-symbolic", "Smaller text (−)", G_CALLBACK(smaller_clicked), r);
    r->larger = zen_button_new("zoom-in-symbolic", "Larger text (+)", G_CALLBACK(larger_clicked), r);
    r->night_toggle = toggle_button("Night", "Night reading mode (N)", G_CALLBACK(night_toggled), r);
    gtk_box_append(bar, r->contents_toggle);
    gtk_box_append(bar, r->previous);
    gtk_box_append(bar, r->counter);
    gtk_box_append(bar, r->next);
    gtk_box_append(bar, r->smaller);
    gtk_box_append(bar, r->larger);
    gtk_box_append(bar, r->night_toggle);
}
static gboolean key_pressed(GtkEventControllerKey *controller, guint key, guint code,
                            GdkModifierType state, gpointer data) {
    (void)controller;
    (void)code;
    Reader *r = data;
    if (!r->book || (state & (GDK_ALT_MASK | GDK_SUPER_MASK))) return FALSE;
    gboolean contents = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r->contents_toggle));
    key = gdk_keyval_to_lower(key);
    if ((state & GDK_CONTROL_MASK) && key == GDK_KEY_b) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->contents_toggle), !contents);
        return TRUE;
    }
    if (key == GDK_KEY_Escape && contents) { close_contents(NULL, r); return TRUE; }
    switch (key) {
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
        if (r->document) { zoom(r, 10); return TRUE; }
        return FALSE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        if (r->document) { zoom(r, -10); return TRUE; }
        return FALSE;
    default: break;
    }
    if (state & GDK_CONTROL_MASK) return FALSE;
    GtkWidget *focus = gtk_window_get_focus(zen_window_get_window(r->shell));
    if (contents && focus && (focus == r->contents_list || gtk_widget_is_ancestor(focus, r->contents_list)))
        return FALSE;
    if ((state & GDK_SHIFT_MASK) && key != GDK_KEY_space) return FALSE;
    switch (key) {
    case GDK_KEY_Left: step(r, -1, FALSE); return TRUE;
    case GDK_KEY_Right: step(r, 1, FALSE); return TRUE;
    case GDK_KEY_Page_Up: scroll_page(r, -1); return TRUE;
    case GDK_KEY_Page_Down: scroll_page(r, 1); return TRUE;
    case GDK_KEY_space: scroll_page(r, state & GDK_SHIFT_MASK ? -1 : 1); return TRUE;
    case GDK_KEY_n:
        if (!r->document) return FALSE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->night_toggle), !r->night);
        return TRUE;
    default: return FALSE;
    }
}
static gboolean close_request(GtkWindow *window, gpointer data) {
    (void)window;
    Reader *r = data;
    if (r->loading) g_cancellable_cancel(r->loading);
    cancel_restore(r);
    g_clear_handle_id(&r->save_timer, g_source_remove);
    g_clear_handle_id(&r->reflow_idle, g_source_remove);
    save_position(r);
    return FALSE;
}
static void reader_free(gpointer data) {
    Reader *r = data;
    g_clear_handle_id(&r->save_timer, g_source_remove);
    g_clear_handle_id(&r->reflow_idle, g_source_remove);
    g_clear_object(&r->loading);
    g_clear_object(&r->toc_selection);
    g_clear_object(&r->file);
    zen_book_document_free(r->document);
    zen_book_unref(r->book);
    g_ptr_array_unref(r->artwork);
    g_free(r);
}
static Reader *new_reader(GtkApplication *app) {
    Reader *r = g_new0(Reader, 1);
    r->zoom = 100;
    r->artwork = g_ptr_array_new();
    r->shell = zen_window_new(app, "Zen Books", 820, 760);
    GtkWindow *window = zen_window_get_window(r->shell);
    g_object_set_data_full(G_OBJECT(window), "zen-books-reader", r, reader_free);
    g_signal_connect(window, "close-request", G_CALLBACK(close_request), r);
    zen_window_set_empty_text(r->shell, "Open a book", "Drop an EPUB or CBZ here to start reading.");
    zen_window_set_open_handler(r->shell, open_book, r);
    zen_window_set_pointer_claim_func(r->shell, claim_text, r);
    const char *mimes[] = {"application/epub+zip", "application/vnd.comicbook+zip", NULL};
    const char *patterns[] = {"*.epub", "*.EPUB", "*.cbz", "*.CBZ", NULL};
    zen_window_add_file_filter(r->shell, "Books and comics", mimes, patterns);

    GtkWidget *overlay = gtk_overlay_new();
    r->canvas = GTK_STACK(gtk_stack_new());
    gtk_stack_add_named(r->canvas, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "empty");
    r->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(r->scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    r->text = g_object_new(book_text_get_type(), NULL);
    g_object_set_data(G_OBJECT(r->text), "zen-reader", r);
    gtk_widget_add_css_class(GTK_WIDGET(r->text), "zen-book-text");
    gtk_text_view_set_wrap_mode(r->text, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable(r->text, FALSE);
    gtk_text_view_set_cursor_visible(r->text, FALSE);
    gtk_text_view_set_left_margin(r->text, 24);
    gtk_text_view_set_right_margin(r->text, 24);
    gtk_text_view_set_top_margin(r->text, 54);
    gtk_text_view_set_bottom_margin(r->text, 64);
    gtk_text_view_set_pixels_inside_wrap(r->text, 5);
    gtk_text_view_set_pixels_below_lines(r->text, 4);
    make_tags(r);
    gtk_scrolled_window_set_child(r->scroll, GTK_WIDGET(r->text));
    gtk_stack_add_named(r->canvas, GTK_WIDGET(r->scroll), "reading");
    g_signal_connect(gtk_scrolled_window_get_vadjustment(r->scroll), "value-changed", G_CALLBACK(scroll_changed), r);

    r->comic = GTK_PICTURE(gtk_picture_new());
    gtk_widget_add_css_class(GTK_WIDGET(r->comic), "zen-book-comic");
    gtk_picture_set_can_shrink(r->comic, TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(r->comic), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(r->comic), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(r->comic), TRUE);
    gtk_stack_add_named(r->canvas, GTK_WIDGET(r->comic), "comic");
    gtk_stack_set_visible_child_name(r->canvas, "empty");
    gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(r->canvas));
    make_contents(r, GTK_OVERLAY(overlay));
    zen_window_set_child(r->shell, overlay);
    make_controls(r);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(link_press), r);
    g_signal_connect(click, "released", G_CALLBACK(link_release), r);
    gtk_widget_add_controller(GTK_WIDGET(r->text), GTK_EVENT_CONTROLLER(click));
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(link_motion), r);
    gtk_widget_add_controller(GTK_WIDGET(r->text), motion);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), r);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);
    zen_window_present(r->shell);
    return r;
}
static void startup(GApplication *app, gpointer data) {
    (void)app;
    (void)data;
    zen_ui_init();
    GtkCssProvider *style = gtk_css_provider_new();
    gtk_css_provider_load_from_data(style,
        ".zen-book-text,.zen-book-text text {background:#f8f8f0;color:#27362e;}"
        ".zen-book-text.night,.zen-book-text.night text {background:#202823;color:#e6ebe1;}"
        ".zen-book-text text selection {background:#bdd9c9;color:#192b21;}"
        ".zen-book-comic {background:#161a18;}"
        ".zen-book-contents {background:rgba(26,36,31,.97);color:#f1f5eb;border-radius:14px;"
        "border:1px solid rgba(207,228,211,.23);}"
        ".zen-book-contents listview {background:transparent;color:#f1f5eb;}"
        ".zen-book-contents row:selected {background:rgba(180,215,193,.26);}"
        ".zen-book-contents button {background:transparent;color:#f1f5eb;box-shadow:none;}", -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(style),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(style);
}
static void activate(GtkApplication *app, gpointer data) { (void)data; new_reader(app); }
static void opened(GtkApplication *app, GFile **files, int count, const char *hint, gpointer data) {
    (void)hint;
    (void)data;
    for (int i = 0; i < count; i++) {
        Reader *r = new_reader(app);
        open_book(r->shell, files[i], r);
    }
}
static int inspect(const char *path) {
    g_autoptr(GFile) file = g_file_new_for_commandline_arg(path);
    g_autoptr(GError) error = NULL;
    ZenBook *book = zen_book_open(file, NULL, &error);
    if (!book) { g_printerr("%s\n", error ? error->message : "This book could not be opened."); return 1; }
    g_print("%s\n%s\n%u %s\n", book->title, book->author ? book->author : "",
            book->chapters->len, book->kind == ZEN_BOOK_EPUB ? "chapters" : "pages");
    zen_book_unref(book);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { g_print("zen-books " ZEN_VERSION "\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        g_print("Usage: zen-books [FILE.epub | FILE.cbz …]\n"
                "Left/Right chapter or page · Page Up/Down scroll · Ctrl+B contents\n"
                "+/− text size · N night mode · Ctrl+O open · F11 fullscreen\n"
                "--inspect FILE   Show book title and chapter or page count\n");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--inspect")) {
        if (argc == 3) return inspect(argv[2]);
        g_printerr("Usage: zen-books --inspect FILE\n");
        return 2;
    }
    g_set_prgname("zen-books");
    g_set_application_name("Zen Books");
    g_autoptr(GtkApplication) app = gtk_application_new("io.github.zenviewers.Books",
        G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(opened), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
