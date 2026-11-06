/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GDK_VERSION_MIN_REQUIRED
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#endif
#include "zen-ui.h"
#include "document.h"
#include <math.h>
#include <string.h>

typedef struct {
    GtkWidget *box, *grid, *scroll;
    GPtrArray *labels;
    MdTable *table;
} TableView;

typedef struct {
    ZenWindow *shell;
    GtkTextView *view;
    GtkScrolledWindow *scroll;
    GtkWidget *zoom_label, *source_toggle;
    GtkTextTag *base, *code_top, *code_bottom, *tags[MD_STYLE_COUNT];
    GPtrArray *tables;
    GFile *file;
    MdDocument *doc;
    GCancellable *cancel;
    guint generation;
    guint reflow_idle;
    int zoom, layout_width;
    double press_x, press_y;
} Reader;

typedef struct { GFile *file; guint generation; } Request;
static void follow(Reader *r, const char *url);
static void size_tables(Reader *r, int width);
static void style_tables(Reader *r);

static void free_table_view(gpointer data) {
    TableView *table = data;
    g_ptr_array_unref(table->labels);
    g_free(table);
}

static void clear_reader(gpointer data) {
    Reader *r = data;
    if (r->cancel) g_cancellable_cancel(r->cancel);
    if (r->reflow_idle) g_source_remove(r->reflow_idle);
    g_clear_object(&r->cancel);
    g_clear_object(&r->file);
    md_document_free(r->doc);
    g_ptr_array_unref(r->tables);
    g_free(r);
}

static gboolean closing(GtkWindow *window, gpointer data) {
    (void)window; Reader *r = data;
    r->generation++;
    if (r->cancel) g_cancellable_cancel(r->cancel);
    return FALSE;
}

static gboolean source_mode(Reader *r) {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r->source_toggle));
}

typedef struct { GtkTextView parent; } ReaderTextView;
typedef struct { GtkTextViewClass parent; } ReaderTextViewClass;
G_DEFINE_TYPE(ReaderTextView, reader_text_view, GTK_TYPE_TEXT_VIEW)

static void code_background(GtkTextView *view, GtkTextViewLayer layer, GtkSnapshot *snapshot) {
    Reader *r = g_object_get_data(G_OBJECT(view), "reader");
    if (layer != GTK_TEXT_VIEW_LAYER_BELOW_TEXT || !r || !r->doc || source_mode(r)) return;
    GdkRectangle visible;
    gtk_text_view_get_visible_rect(view, &visible);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GdkRGBA color = { .red=.91, .green=.935, .blue=.91, .alpha=1. };
    for (guint i = 0; i < r->doc->spans->len; i++) {
        MdSpan *span = &g_array_index(r->doc->spans, MdSpan, i);
        if (span->style != MD_CODE_BLOCK) continue;
        GtkTextIter start, end;
        GdkRectangle first, last;
        gtk_text_buffer_get_iter_at_offset(buffer, &start, span->start);
        gtk_text_buffer_get_iter_at_offset(buffer, &end, span->end - 1);
        if (gtk_text_iter_get_char(&end) == '\n' && span->end > span->start + 1)
            gtk_text_iter_backward_char(&end);
        gtk_text_view_get_iter_location(view, &start, &first);
        gtk_text_view_get_iter_location(view, &end, &last);
        float top = first.y - 9, bottom = last.y + last.height + 9;
        if (bottom < visible.y || top > visible.y + visible.height) continue;
        int left = gtk_text_view_get_left_margin(view), right = gtk_text_view_get_right_margin(view);
        graphene_rect_t rect = GRAPHENE_RECT_INIT(left, top, MAX(1, visible.width-left-right), bottom-top);
        GskRoundedRect rounded;
        gsk_rounded_rect_init_from_rect(&rounded, &rect, 6.);
        gtk_snapshot_push_rounded_clip(snapshot, &rounded);
        gtk_snapshot_append_color(snapshot, &color, &rect);
        gtk_snapshot_pop(snapshot);
    }
}

static void reader_text_view_class_init(ReaderTextViewClass *klass) {
    GTK_TEXT_VIEW_CLASS(klass)->snapshot_layer = code_background;
}
static void reader_text_view_init(ReaderTextView *view) { (void)view; }

static void make_tags(Reader *r) {
    GtkTextBuffer *b = gtk_text_view_get_buffer(r->view);
    r->base = gtk_text_buffer_create_tag(b, "base", "size-points", 13.,
        "family", "Noto Serif, DejaVu Serif, serif", NULL);
    const double scales[] = {1.85, 1.48, 1.26, 1.14, 1.05, 1.};
    for (int i = 0; i < 6; i++)
        r->tags[MD_HEADING1+i] = gtk_text_buffer_create_tag(b, NULL, "scale", scales[i],
            "weight", PANGO_WEIGHT_BOLD, "pixels-above-lines", 4, NULL);
    r->tags[MD_STRONG] = gtk_text_buffer_create_tag(b, NULL, "weight", PANGO_WEIGHT_BOLD, NULL);
    r->tags[MD_EMPHASIS] = gtk_text_buffer_create_tag(b, NULL, "style", PANGO_STYLE_ITALIC, NULL);
    r->tags[MD_CODE] = gtk_text_buffer_create_tag(b, NULL, "family", "monospace",
        "background", "#e4eae3", "scale", .94, NULL);
    r->tags[MD_CODE_BLOCK] = gtk_text_buffer_create_tag(b, NULL, "family", "monospace",
        "scale", .94, "left-margin", 44, "right-margin", 44,
        "pixels-above-lines", 2, "pixels-below-lines", 2, NULL);
    r->code_top = gtk_text_buffer_create_tag(b, NULL, "pixels-above-lines", 12, NULL);
    r->code_bottom = gtk_text_buffer_create_tag(b, NULL, "pixels-below-lines", 12, NULL);
    r->tags[MD_QUOTE] = gtk_text_buffer_create_tag(b, NULL, "foreground", "#50665b",
        "style", PANGO_STYLE_ITALIC, NULL);
    r->tags[MD_LINK] = gtk_text_buffer_create_tag(b, NULL, "foreground", "#19654f",
        "underline", PANGO_UNDERLINE_SINGLE, NULL);
    r->tags[MD_RULE] = gtk_text_buffer_create_tag(b, NULL, "foreground", "#8b9d90", NULL);
    r->tags[MD_STRIKE] = gtk_text_buffer_create_tag(b, NULL, "strikethrough", TRUE, NULL);
}

static gboolean cell_link(GtkLabel *label, const char *uri, gpointer data) {
    (void)label;
    follow(data, uri);
    return TRUE;
}

static void copy_table(GtkButton *button, gpointer data) {
    MdTable *table = data;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), table->tsv);
}

static char *cell_markup(MdCell *cell) {
    GString *out = g_string_new(NULL);
    int at = 0;
    for (guint i = 0; i < cell->spans->len; i++) {
        MdSpan *s = &g_array_index(cell->spans, MdSpan, i);
        if (s->style != MD_LINK || s->start < at) continue;
        const char *begin = g_utf8_offset_to_pointer(cell->text, at);
        const char *start = g_utf8_offset_to_pointer(begin, s->start - at);
        const char *end = g_utf8_offset_to_pointer(start, s->end - s->start);
        g_autofree char *prefix = g_markup_escape_text(begin, start - begin);
        g_autofree char *body = g_markup_escape_text(start, end - start);
        g_autofree char *uri = g_markup_escape_text(s->url, -1);
        g_string_append_printf(out, "%s<a href=\"%s\">%s</a>", prefix, uri, body);
        at = s->end;
    }
    g_autofree char *tail = g_markup_escape_text(g_utf8_offset_to_pointer(cell->text, at), -1);
    g_string_append(out, tail);
    return g_string_free(out, FALSE);
}

static void style_cell(GtkLabel *label, MdCell *cell, gboolean header, int zoom) {
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_family_new("Noto Serif, DejaVu Serif, serif"));
    pango_attr_list_insert(attrs, pango_attr_size_new((int)(13. * zoom / 100. * PANGO_SCALE)));
    if (header) pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    for (guint i = 0; i < cell->spans->len; i++) {
        MdSpan *span = &g_array_index(cell->spans, MdSpan, i);
        PangoAttribute *attr = NULL;
        switch (span->style) {
        case MD_STRONG: attr = pango_attr_weight_new(PANGO_WEIGHT_BOLD); break;
        case MD_EMPHASIS: attr = pango_attr_style_new(PANGO_STYLE_ITALIC); break;
        case MD_STRIKE: attr = pango_attr_strikethrough_new(TRUE); break;
        case MD_CODE: attr = pango_attr_family_new("monospace"); break;
        default: break;
        }
        if (attr) {
            attr->start_index = (guint)(g_utf8_offset_to_pointer(cell->text, span->start) - cell->text);
            attr->end_index = (guint)(g_utf8_offset_to_pointer(cell->text, span->end) - cell->text);
            pango_attr_list_change(attrs, attr);
        }
    }
    gtk_label_set_attributes(label, attrs);
    pango_attr_list_unref(attrs);
}

static GtkWidget *new_table(Reader *r, MdTable *table) {
    TableView *view = g_new0(TableView, 1);
    view->table = table;
    view->labels = g_ptr_array_new();
    view->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(view->box, "md-table");
    GtkWidget *scroll = gtk_scrolled_window_new();
    view->scroll = scroll;
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);
    view->grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(view->grid), TRUE);
    gtk_widget_set_hexpand(view->grid, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view->grid);
    gtk_box_append(GTK_BOX(view->box), scroll);
    for (guint i = 0; i < table->cells->len; i++) {
        MdCell *cell = g_ptr_array_index(table->cells, i);
        guint col = i % table->columns, row = i / table->columns;
        GtkWidget *label = gtk_label_new(NULL);
        g_autofree char *markup = cell_markup(cell);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(label), row ? PANGO_WRAP_WORD_CHAR : PANGO_WRAP_WORD);
        gtk_label_set_width_chars(GTK_LABEL(label), 10);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
        guint8 align = table->alignments[col];
        gtk_label_set_xalign(GTK_LABEL(label), align == 'r' ? 1. : align == 'c' ? .5 : 0.);
        gtk_label_set_yalign(GTK_LABEL(label), 0.);
        gtk_label_set_justify(GTK_LABEL(label), align == 'r' ? GTK_JUSTIFY_RIGHT : align == 'c' ? GTK_JUSTIFY_CENTER : GTK_JUSTIFY_LEFT);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "md-cell");
        if (!row) gtk_widget_add_css_class(label, "md-header");
        else if (!(row % 2)) gtk_widget_add_css_class(label, "md-even");
        g_signal_connect(label, "activate-link", G_CALLBACK(cell_link), r);
        gtk_grid_attach(GTK_GRID(view->grid), label, col, row, 1, 1);
        g_ptr_array_add(view->labels, label);
        style_cell(GTK_LABEL(label), cell, !row, r->zoom);
    }
    GtkWidget *copy = gtk_button_new_with_label("Copy table");
    gtk_widget_add_css_class(copy, "flat");
    gtk_widget_add_css_class(copy, "md-table-copy");
    gtk_widget_set_halign(copy, GTK_ALIGN_END);
    gtk_widget_set_tooltip_text(copy, "Copy all cells as tab-separated text");
    g_signal_connect(copy, "clicked", G_CALLBACK(copy_table), table);
    gtk_box_append(GTK_BOX(view->box), copy);
    g_ptr_array_add(r->tables, view);
    return view->box;
}

static void style_tables(Reader *r) {
    for (guint t = 0; t < r->tables->len; t++) {
        TableView *view = g_ptr_array_index(r->tables, t);
        for (guint i = 0; i < view->labels->len; i++)
            style_cell(g_ptr_array_index(view->labels, i), g_ptr_array_index(view->table->cells, i),
                i < view->table->columns, r->zoom);
    }
}

static void size_tables(Reader *r, int width) {
    int inner = MAX(160, width - gtk_text_view_get_left_margin(r->view) - gtk_text_view_get_right_margin(r->view));
    for (guint i = 0; i < r->tables->len; i++) {
        TableView *view = g_ptr_array_index(r->tables, i);
        gtk_widget_set_size_request(view->box, inner, -1);
        int minimum_width, height;
        gtk_widget_set_size_request(view->grid, -1, -1);
        gtk_widget_measure(view->grid, GTK_ORIENTATION_HORIZONTAL, -1, &minimum_width, NULL, NULL, NULL);
        gtk_widget_set_size_request(view->grid, MAX(inner, minimum_width), -1);
        gtk_widget_measure(view->grid, GTK_ORIENTATION_VERTICAL, MAX(inner, minimum_width), &height, NULL, NULL, NULL);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(view->scroll), height);
    }
}

static void show_content(Reader *r) {
    if (!r->doc) return;
    GtkTextBuffer *b = gtk_text_view_get_buffer(r->view);
    gboolean source = source_mode(r);
    g_ptr_array_set_size(r->tables, 0);
    g_object_set(r->base, "family", source ? "monospace" : "Noto Serif, DejaVu Serif, serif", NULL);
    gtk_text_view_set_monospace(r->view, source);
    gtk_text_buffer_set_text(b, source ? r->doc->source : r->doc->text, -1);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(b, &start, &end);
    gtk_text_buffer_apply_tag(b, r->base, &start, &end);
    if (!source) {
        for (guint i = 0; i < r->doc->spans->len; i++) {
            MdSpan *span = &g_array_index(r->doc->spans, MdSpan, i);
            gtk_text_buffer_get_iter_at_offset(b, &start, span->start);
            gtk_text_buffer_get_iter_at_offset(b, &end, span->end);
            gtk_text_buffer_apply_tag(b, r->tags[span->style], &start, &end);
            if (span->style == MD_CODE_BLOCK) {
                GtkTextIter first = start, last = end;
                gtk_text_iter_forward_char(&first);
                gtk_text_buffer_apply_tag(b, r->code_top, &start, &first);
                if (gtk_text_iter_backward_char(&last) && gtk_text_iter_get_char(&last) == '\n')
                    gtk_text_iter_backward_char(&last);
                gtk_text_buffer_apply_tag(b, r->code_bottom, &last, &end);
            }
        }
        for (guint i = 0; i < r->doc->tables->len; i++) {
            MdTable *table = g_ptr_array_index(r->doc->tables, i);
            gtk_text_buffer_get_iter_at_offset(b, &start, table->offset);
            end = start;
            gtk_text_iter_forward_char(&end);
            gtk_text_buffer_delete(b, &start, &end);
            GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(b, &start);
            gtk_text_view_add_child_at_anchor(r->view, new_table(r, table), anchor);
        }
        size_tables(r, gtk_widget_get_width(GTK_WIDGET(r->view)));
    }
    gtk_text_buffer_get_start_iter(b, &start);
    gtk_text_buffer_place_cursor(b, &start);
    gtk_text_view_set_cursor_visible(r->view,FALSE);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(r->scroll), 0);
    gtk_widget_grab_focus(GTK_WIDGET(r->view));
}

static void zoom_to(Reader *r, int percent) {
    r->zoom = CLAMP(percent, 60, 220);
    char label[16];
    g_snprintf(label, sizeof label, "%d%%", r->zoom);
    gtk_label_set_text(GTK_LABEL(r->zoom_label), label);
    g_object_set(r->base, "size-points", 13. * r->zoom / 100., NULL);
    style_tables(r);
    size_tables(r, gtk_widget_get_width(GTK_WIDGET(r->view)));
}

static void load_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    GError *error = NULL;
    MdDocument *doc = md_load(((Request *)data)->file, cancel, &error);
    if (doc) g_task_return_pointer(task, doc, (GDestroyNotify)md_document_free);
    else g_task_return_error(task, error);
}

static void request_free(gpointer data) {
    Request *req = data;
    g_object_unref(req->file);
    g_free(req);
}

static void loaded(GObject *source, GAsyncResult *result, gpointer unused) {
    (void)unused;
    Reader *r = g_object_get_data(source, "md-reader");
    Request *req = g_task_get_task_data(G_TASK(result));
    GError *error = NULL;
    MdDocument *doc = g_task_propagate_pointer(G_TASK(result), &error);
    if (req->generation == r->generation && !zen_window_is_closed(r->shell)) {
        g_clear_object(&r->cancel);
        if (doc) {
            g_set_object(&r->file, req->file);
            md_document_free(r->doc);
            r->doc = doc;
            doc = NULL;
            g_autofree char *filename = g_file_get_basename(r->file);
            zen_window_set_title(r->shell, filename);
            zen_window_set_ready(r->shell, TRUE);
            zen_window_clear_notice(r->shell);
            show_content(r);
        } else if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            zen_window_notice(r->shell, error->message);
        }
        zen_window_hide_progress(r->shell);
    }
    md_document_free(doc);
    g_clear_error(&error);
}

static void open_file(ZenWindow *shell, GFile *file, gpointer data) {
    Reader *r = data;
    if (r->cancel) g_cancellable_cancel(r->cancel);
    g_clear_object(&r->cancel);
    r->cancel = g_cancellable_new();
    Request *req = g_new0(Request, 1);
    req->file = g_object_ref(file);
    req->generation = ++r->generation;
    GTask *task = g_task_new(zen_window_get_window(shell), r->cancel, loaded, NULL);
    g_task_set_task_data(task, req, request_free);
    zen_window_clear_notice(shell);
    zen_window_show_progress(shell, -1);
    g_task_run_in_thread(task, load_thread);
    g_object_unref(task);
}

static gboolean iter_at(Reader *r, double x, double y, GtkTextIter *iter) {
    int bx, by;
    gtk_text_view_window_to_buffer_coords(r->view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    return gtk_text_view_get_iter_at_location(r->view, iter, bx, by);
}

static gboolean claim_text(double x, double y, gpointer data) {
    Reader *r = data;
    graphene_point_t point = GRAPHENE_POINT_INIT((float)x, (float)y), local;
    GtkTextIter iter;
    return r->doc && gtk_widget_compute_point(zen_window_get_root(r->shell), GTK_WIDGET(r->view), &point, &local)
        && iter_at(r, local.x, local.y, &iter);
}

static const char *link_at(Reader *r, GtkTextIter *iter) {
    if (!r->doc || source_mode(r) || !gtk_text_iter_has_tag(iter, r->tags[MD_LINK])) return NULL;
    int offset = gtk_text_iter_get_offset(iter);
    for (guint i = 0; i < r->doc->spans->len; i++) {
        MdSpan *span = &g_array_index(r->doc->spans, MdSpan, i);
        if (span->style == MD_LINK && span->start <= offset && offset < span->end) return span->url;
    }
    return NULL;
}

static void launch_done(GObject *unused, GAsyncResult *result, gpointer data) {
    (void)unused;
    GtkWindow *parent = data;
    Reader *r = g_object_get_data(G_OBJECT(parent), "md-reader");
    g_autoptr(GError) error = NULL;
    if (!g_app_info_launch_default_for_uri_finish(result, &error) && !zen_window_is_closed(r->shell))
        zen_window_notice(r->shell, error->message);
    g_object_unref(parent);
}

static void follow(Reader *r, const char *url) {
    if (!url || !*url) return;
    if (*url == '#') {
        g_autofree char *fragment = g_uri_unescape_string(url + 1, NULL);
        int index = fragment ? GPOINTER_TO_INT(g_hash_table_lookup(r->doc->anchors, fragment)) : 0;
        if (!index) { zen_window_notice(r->shell, "That heading was not found."); return; }
        GtkTextIter iter;
        GtkTextBuffer *b = gtk_text_view_get_buffer(r->view);
        gtk_text_buffer_get_iter_at_offset(b, &iter, index-1);
        gtk_text_buffer_place_cursor(b, &iter);
        gtk_text_view_scroll_to_iter(r->view, &iter, 0., TRUE, 0., .12);
        gtk_widget_grab_focus(GTK_WIDGET(r->view));
        return;
    }
    g_autofree char *base = g_file_get_uri(r->file);
    g_autoptr(GError) error = NULL;
    g_autofree char *uri = g_uri_resolve_relative(base, url, G_URI_FLAGS_PARSE_RELAXED, &error);
    if (!uri) { zen_window_notice(r->shell, error->message); return; }
    g_autofree char *scheme = g_uri_parse_scheme(uri);
    if (!scheme || (g_ascii_strcasecmp(scheme, "https") && g_ascii_strcasecmp(scheme, "http") &&
                    g_ascii_strcasecmp(scheme, "mailto") && g_ascii_strcasecmp(scheme, "file"))) {
        zen_window_notice(r->shell, "This link type is not supported.");
        return;
    }
    GtkWindow *parent = zen_window_get_window(r->shell);
    GdkAppLaunchContext *context = gdk_display_get_app_launch_context(gtk_widget_get_display(GTK_WIDGET(parent)));
    g_app_info_launch_default_for_uri_async(uri, G_APP_LAUNCH_CONTEXT(context), NULL, launch_done, g_object_ref(parent));
    g_object_unref(context);
}

static void pressed(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
    (void)gesture; (void)n;
    Reader *r = data;
    r->press_x = x;
    r->press_y = y;
}
static void released(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
    (void)gesture; Reader *r = data;
    GtkTextIter iter;
    if (n == 1 && fabs(r->press_x-x) < 4 && fabs(r->press_y-y) < 4 &&
        !gtk_text_buffer_get_has_selection(gtk_text_view_get_buffer(r->view)) && iter_at(r,x,y,&iter))
        follow(r,link_at(r,&iter));
}

static void motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer data) {
    (void)ctrl; Reader *r = data;
    GtkTextIter iter;
    gboolean link = r->doc && !source_mode(r) && iter_at(r,x,y,&iter) && gtk_text_iter_has_tag(&iter,r->tags[MD_LINK]);
    gtk_widget_set_cursor_from_name(GTK_WIDGET(r->view),link ? "pointer" : "text");
}

static gboolean keypress(GtkEventControllerKey *ctrl, guint key, guint code, GdkModifierType state, gpointer data) {
    (void)ctrl; (void)code; Reader *r = data;
    if (key == GDK_KEY_Left || key == GDK_KEY_Right || key == GDK_KEY_Up || key == GDK_KEY_Down ||
        key == GDK_KEY_Home || key == GDK_KEY_End)
        gtk_text_view_set_cursor_visible(r->view,TRUE);
    if (state & GDK_CONTROL_MASK) {
        switch (key) {
        case GDK_KEY_equal: case GDK_KEY_plus: case GDK_KEY_KP_Add: zoom_to(r,r->zoom+10); return TRUE;
        case GDK_KEY_minus: case GDK_KEY_KP_Subtract: zoom_to(r,r->zoom-10); return TRUE;
        case GDK_KEY_0: zoom_to(r,100); return TRUE;
        case GDK_KEY_u: gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->source_toggle), !source_mode(r)); return TRUE;
        }
    }
    if (key == GDK_KEY_Return && gtk_widget_has_focus(GTK_WIDGET(r->view))) {
        GtkTextBuffer *b = gtk_text_view_get_buffer(r->view);
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_mark(b, &iter, gtk_text_buffer_get_insert(b));
        const char *href = link_at(r,&iter);
        if (href) { follow(r,href); return TRUE; }
    }
    return FALSE;
}

static void minus(GtkButton *b, gpointer data) { (void)b; Reader *r=data; zoom_to(r,r->zoom-10); }
static void plus(GtkButton *b, gpointer data) { (void)b; Reader *r=data; zoom_to(r,r->zoom+10); }
static void toggle(GtkToggleButton *b, gpointer data) { (void)b; show_content(data); }

static void copy_selection(GtkTextView *view, gpointer data) {
    Reader *r = data;
    GtkTextIter start, end;
    if (!r->doc || source_mode(r) || !r->doc->tables->len ||
        !gtk_text_buffer_get_selection_bounds(gtk_text_view_get_buffer(view), &start, &end)) return;
    g_autofree char *text = md_copy_text(r->doc, gtk_text_iter_get_offset(&start), gtk_text_iter_get_offset(&end));
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(view)), text);
    g_signal_stop_emission_by_name(view, "copy-clipboard");
}

static gboolean apply_reflow(gpointer data) {
    Reader *r = data;
    r->reflow_idle = 0;
    int width = r->layout_width;
    int gutter = MAX(24, (width - 820)/2);
    if (gtk_text_view_get_left_margin(r->view) != gutter) {
        gtk_text_view_set_left_margin(r->view,gutter);
        gtk_text_view_set_right_margin(r->view,gutter);
    }
    int code_margin;
    g_object_get(r->tags[MD_CODE_BLOCK], "left-margin", &code_margin, NULL);
    if (code_margin != gutter+12)
        g_object_set(r->tags[MD_CODE_BLOCK], "left-margin", gutter+12, "right-margin", gutter+12, NULL);
    size_tables(r, width);
    return G_SOURCE_REMOVE;
}

static void reflow(GtkAdjustment *adjustment, gpointer data) {
    Reader *r = data;
    int width = (int)gtk_adjustment_get_page_size(adjustment);
    /* Adjustment notifications also occur during GtkTextView validation. */
    if (width > 0 && width != r->layout_width) {
        r->layout_width = width;
        if (!r->reflow_idle) r->reflow_idle = g_idle_add(apply_reflow, r);
    }
}

static Reader *new_reader(GtkApplication *app) {
    Reader *r = g_new0(Reader,1);
    r->tables = g_ptr_array_new_with_free_func(free_table_view);
    r->shell = zen_window_new(app, "Zen Markdown", 800, 760);
    GtkWindow *win = zen_window_get_window(r->shell);
    g_object_set_data_full(G_OBJECT(win), "md-reader", r, clear_reader);
    g_signal_connect(win, "close-request", G_CALLBACK(closing), r);
    zen_window_set_empty_text(r->shell, "Zen Markdown", "Open a Markdown file or drop one here.");
    zen_window_set_open_handler(r->shell, open_file, r);
    zen_window_set_pointer_claim_func(r->shell, claim_text, r);
    const char *mime[] = {"text/markdown","text/x-markdown","text/plain",NULL};
    const char *patterns[] = {"*.md","*.markdown","*.mdown","*.mkd","*.txt",NULL};
    zen_window_add_file_filter(r->shell, "Markdown documents", mime, patterns);
    r->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(r->scroll, GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
    r->view = g_object_new(reader_text_view_get_type(), NULL);
    g_object_set_data(G_OBJECT(r->view), "reader", r);
    gtk_widget_add_css_class(GTK_WIDGET(r->view), "md-reader");
    gtk_text_view_set_wrap_mode(r->view,GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable(r->view,FALSE);
    gtk_text_view_set_left_margin(r->view,32); gtk_text_view_set_right_margin(r->view,32);
    gtk_text_view_set_top_margin(r->view,56); gtk_text_view_set_bottom_margin(r->view,80);
    gtk_text_view_set_pixels_inside_wrap(r->view,3);
    make_tags(r);
    g_signal_connect(r->view, "copy-clipboard", G_CALLBACK(copy_selection), r);
    g_signal_connect(gtk_scrolled_window_get_hadjustment(r->scroll), "changed", G_CALLBACK(reflow), r);
    gtk_scrolled_window_set_child(r->scroll,GTK_WIDGET(r->view));
    zen_window_set_child(r->shell,GTK_WIDGET(r->scroll));
    GtkWidget *controls = zen_window_get_controls(r->shell);
    gtk_box_append(GTK_BOX(controls),zen_button_new("zoom-out-symbolic","Smaller text (Ctrl+−)",G_CALLBACK(minus),r));
    r->zoom_label = gtk_label_new(NULL);
    gtk_box_append(GTK_BOX(controls),r->zoom_label);
    gtk_box_append(GTK_BOX(controls),zen_button_new("zoom-in-symbolic","Larger text (Ctrl++)",G_CALLBACK(plus),r));
    r->source_toggle = gtk_toggle_button_new_with_label("Source");
    gtk_widget_set_tooltip_text(r->source_toggle,"View Markdown source (Ctrl+U)");
    g_signal_connect(r->source_toggle,"toggled",G_CALLBACK(toggle),r);
    gtk_box_append(GTK_BOX(controls),r->source_toggle);
    zoom_to(r,100);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys,GTK_PHASE_CAPTURE);
    g_signal_connect(keys,"key-pressed",G_CALLBACK(keypress),r);
    gtk_widget_add_controller(GTK_WIDGET(win),keys);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),GTK_PHASE_CAPTURE);
    g_signal_connect(click,"pressed",G_CALLBACK(pressed),r);
    g_signal_connect(click,"released",G_CALLBACK(released),r);
    gtk_widget_add_controller(GTK_WIDGET(r->view),GTK_EVENT_CONTROLLER(click));
    GtkEventController *move = gtk_event_controller_motion_new();
    g_signal_connect(move,"motion",G_CALLBACK(motion),r);
    gtk_widget_add_controller(GTK_WIDGET(r->view),move);
    zen_window_set_ready(r->shell, FALSE);
    zen_window_present(r->shell);
    return r;
}

static void startup(GApplication *app, gpointer data) {
    (void)app; (void)data; zen_ui_init();
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "textview.md-reader, textview.md-reader text {background-color:#fbfcf8;color:#26332c;}"
        "textview.md-reader text selection {background-color:#a8cfbb;color:#14221b;}"
        ".md-table {color:#26332c;}"
        ".md-cell {padding:10px 14px;border-bottom:1px solid #dce3dc;border-right:1px solid #e5eae3;}"
        ".md-header {background:#e8eee6;border-bottom:2px solid #b9cabb;}"
        ".md-even {background:#f2f5ef;}"
        ".md-cell selection {background:#a8cfbb;color:#14221b;}"
        ".md-cell link {color:#19654f;}"
        ".md-table-copy {font:11px sans-serif;min-height:22px;padding:2px 7px;color:#50665b;opacity:0;}"
        ".md-table:hover .md-table-copy,.md-table:focus-within .md-table-copy {opacity:1;}",-1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}
static void activate(GtkApplication *app, gpointer data) { (void)data; new_reader(app); }
static void opened(GtkApplication *app,GFile **files,int n,const char *hint,gpointer data) {
    (void)hint; (void)data;
    for (int i=0; i<n; i++) {Reader *r=new_reader(app); open_file(r->shell,files[i],r);}
}

static int self_test(void) {
    const char *input="# Café\n\n**Bold** and *quiet* with [link](#café).\n\n3. three\n4. four\n\n> quote\n\n```c\nint x;\n```\n";
    g_autoptr(GError) error=NULL;
    MdDocument *d=md_parse(input,strlen(input),NULL,&error);
    g_assert_no_error(error); g_assert_nonnull(d);
    g_assert_cmpstr(d->text,==,"Café\n\nBold and quiet with link.\n\n3.  three\n4.  four\n\nquote\n\nint x;\n\n");
    g_assert_true(g_hash_table_contains(d->anchors,"café"));
    gboolean found=FALSE;
    for (guint i=0; i<d->spans->len; i++) {
        MdSpan *s=&g_array_index(d->spans,MdSpan,i);
        if (s->style==MD_LINK) {
            g_autofree char *word=g_utf8_substring(d->text,s->start,s->end);
            g_assert_cmpstr(word,==,"link");
            g_assert_cmpstr(s->url,==,"#café");
            found=TRUE;
        }
    }
    g_assert_true(found); md_document_free(d);
    const char *table_source = "# Café\n\n| Name | Style | Value |\n| :--- | :---: | ---: |\n"
        "| naïve | **bold** and `a\\|b` | €2 |\n| [next](#after) | ~~old~~ | €3 |\n\n# After\n";
    d = md_parse(table_source, strlen(table_source), NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(d);
    g_assert_cmpuint(d->tables->len, ==, 1);
    MdTable *table = g_ptr_array_index(d->tables, 0);
    g_assert_cmpuint(table->columns, ==, 3);
    g_assert_cmpuint(table->cells->len, ==, 9);
    g_assert_cmpint(table->alignments[0], ==, 'l');
    g_assert_cmpint(table->alignments[1], ==, 'c');
    g_assert_cmpint(table->alignments[2], ==, 'r');
    g_assert_cmpstr(table->tsv, ==, "Name\tStyle\tValue\nnaïve\tbold and a|b\t€2\nnext\told\t€3\n");
    MdCell *styled = g_ptr_array_index(table->cells, 4);
    g_assert_cmpuint(styled->spans->len, ==, 2);
    g_autofree char *copy = md_copy_text(d, 0, (int)g_utf8_strlen(d->text, -1));
    g_assert_cmpstr(copy, ==, "Café\n\nName\tStyle\tValue\nnaïve\tbold and a|b\t€2\nnext\told\t€3\n\nAfter\n\n");
    int after = GPOINTER_TO_INT(g_hash_table_lookup(d->anchors, "after")) - 1;
    g_autofree char *tail = md_copy_text(d, after, after + 5);
    g_assert_cmpstr(tail, ==, "After");
    g_assert_cmpstr(d->source, ==, table_source);
    md_document_free(d);
    GString *large = g_string_new("| Header |\n| --- |\n");
    for (int i = 0; i < 2048; i++) g_string_append(large, "| row |\n");
    g_assert_null(md_parse(large->str, large->len, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
    g_clear_error(&error);
    g_string_free(large, TRUE);
    const char invalid[] = "A\0B";
    g_assert_null(md_parse(invalid,3,NULL,&error)); g_assert_nonnull(error);
    g_print("PASS Markdown parsing, Unicode offsets, anchors, GFM tables, alignment, TSV copy and input limits.\n"); return 0;
}
int main(int argc, char **argv) {
    if (argc==2 && !strcmp(argv[1],"--self-test")) return self_test();
    if (argc==2 && !strcmp(argv[1],"--version")) {g_print("zen-markdown " ZEN_VERSION "\n");return 0;}
    if (argc==2 && !strcmp(argv[1],"--help")) {
        g_print("Usage: zen-markdown [FILE.md …]\nCtrl+O open · Ctrl+C copy · Ctrl++/− text size · Ctrl+U source\n");
        return 0;
    }
    g_set_prgname("zen-markdown"); g_set_application_name("Zen Markdown");
    GtkApplication *app=gtk_application_new("io.github.zenviewers.Markdown",G_APPLICATION_HANDLES_OPEN|G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app,"startup",G_CALLBACK(startup),NULL); g_signal_connect(app,"activate",G_CALLBACK(activate),NULL);
    g_signal_connect(app,"open",G_CALLBACK(opened),NULL);
    int status=g_application_run(G_APPLICATION(app),argc,argv); g_object_unref(app); return status;
}
