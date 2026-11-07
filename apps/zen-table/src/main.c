/* SPDX-License-Identifier: GPL-3.0-or-later */
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_6
#include "zen-ui.h"
#include "table-data.h"
#include <locale.h>
#include <math.h>
#include <string.h>
#ifndef ZEN_VERSION
#define ZEN_VERSION "development"
#endif

typedef struct Table Table;
typedef struct { GObject parent; guint index; char **values; } Row;
typedef struct { GObjectClass parent; } RowClass;
G_DEFINE_TYPE(Row, row, G_TYPE_OBJECT)
static void row_finalize(GObject *object) { g_strfreev(((Row *)object)->values); G_OBJECT_CLASS(row_parent_class)->finalize(object); }
static void row_class_init(RowClass *klass) { G_OBJECT_CLASS(klass)->finalize = row_finalize; }
static void row_init(Row *row) { (void)row; }

typedef struct { GPtrArray *items; guint64 stamp; gsize bytes; } CachedPage;
typedef struct {
    GObject parent;
    Table *owner;
    ZtSource *source;
    GHashTable *pages, *failed;
    GQueue pending;
    GCancellable *cancel;
    guint active_page, dispatch;
    guint64 stamp;
    gboolean busy;
} TableModel;
typedef struct { GObjectClass parent; } TableModelClass;
static void model_interface(GListModelInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TableModel, table_model, G_TYPE_OBJECT,
                       G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, model_interface))
struct Table {
    ZenWindow *shell;
    GtkColumnView *view;
    GtkScrolledWindow *scroll;
    GtkSingleSelection *selection;
    TableModel *model;
    GtkWidget *counter, *search, *search_box, *find_button, *info;
    GtkWidget *goto_entry, *goto_box, *active_cell;
    GCancellable *opening, *finding;
    ZtSource *find_source;
    guint generation, selected_column;
    guint jump_idle;
    guint jump_row;
    gboolean indexing, in_search;
};
static void update_status(Table *t);
static void model_pump(TableModel *model);
static void selected_changed(GObject *object, GParamSpec *spec, gpointer data);

static void cached_free(gpointer data) { CachedPage *page = data; g_ptr_array_unref(page->items); g_free(page); }
static void model_dispose(GObject *object) {
    TableModel *m = (TableModel *)object;
    if (m->dispatch) { g_source_remove(m->dispatch); m->dispatch = 0; }
    g_cancellable_cancel(m->cancel); g_clear_object(&m->cancel);
    g_clear_pointer(&m->source, zt_source_unref);
    g_clear_pointer(&m->pages, g_hash_table_unref); g_clear_pointer(&m->failed, g_hash_table_unref);
    g_queue_clear(&m->pending);
    G_OBJECT_CLASS(table_model_parent_class)->dispose(object);
}
static void table_model_class_init(TableModelClass *klass) { G_OBJECT_CLASS(klass)->dispose = model_dispose; }
static void table_model_init(TableModel *m) {
    m->pages = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, cached_free);
    m->failed = g_hash_table_new(g_direct_hash, g_direct_equal);
    m->cancel = g_cancellable_new(); g_queue_init(&m->pending);
}
static GType model_item_type(GListModel *model) { (void)model; return row_get_type(); }
static guint model_count(GListModel *model) { return zt_source_rows(((TableModel *)model)->source); }
static gboolean dispatch_pages(gpointer data) { TableModel *m = data; m->dispatch = 0; model_pump(m); return G_SOURCE_REMOVE; }
static gpointer model_item(GListModel *model, guint position) {
    TableModel *m = (TableModel *)model;
    if (position >= zt_source_rows(m->source)) return NULL;
    guint page = position / ZT_PAGE_SIZE;
    gpointer key = GUINT_TO_POINTER(page + 1);
    CachedPage *cached = g_hash_table_lookup(m->pages, key);
    if (cached && position % ZT_PAGE_SIZE < cached->items->len) {
        cached->stamp = ++m->stamp;
        return g_object_ref(g_ptr_array_index(cached->items, position % ZT_PAGE_SIZE));
    }
    if (!g_hash_table_contains(m->failed, key) && (!m->busy || m->active_page != page) && !g_queue_find(&m->pending, key)) {
        /* GtkListView requests a small overscan. Keep recent requests bounded
         * when the scrollbar is dragged across millions of rows. */
        if (m->pending.length >= 16) g_queue_pop_head(&m->pending);
        g_queue_push_tail(&m->pending, key);
        if (!m->dispatch) m->dispatch = g_idle_add(dispatch_pages, m);
    }
    Row *row = g_object_new(row_get_type(), NULL); row->index = position; return row;
}
static void model_interface(GListModelInterface *iface) {
    iface->get_item_type = model_item_type; iface->get_n_items = model_count; iface->get_item = model_item;
}
static void fetch_page(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    TableModel *m = object; guint start = GPOINTER_TO_UINT(data);
    g_autoptr(GError) error = NULL;
    ZtPage *page = zt_source_page(m->source, start, ZT_PAGE_SIZE, cancel, &error);
    if (page) g_task_return_pointer(task, page, (GDestroyNotify)zt_page_free);
    else g_task_return_error(task, g_steal_pointer(&error));
}
static void trim_pages(TableModel *m, gsize incoming) {
    gsize bytes = incoming; GHashTableIter sum; gpointer item;
    g_hash_table_iter_init(&sum, m->pages);
    while (g_hash_table_iter_next(&sum, NULL, &item)) bytes += ((CachedPage *)item)->bytes;
    if (g_hash_table_size(m->pages) < 16 && bytes <= 32 * 1024 * 1024) return;
    GHashTableIter it; gpointer key, value, oldest_key = NULL; guint64 oldest = G_MAXUINT64;
    g_hash_table_iter_init(&it, m->pages);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        CachedPage *page = value;
        if (page->stamp < oldest) { oldest = page->stamp; oldest_key = key; }
    }
    if (oldest_key) { g_hash_table_remove(m->pages, oldest_key); trim_pages(m, incoming); }
}
static void page_ready(GObject *object, GAsyncResult *result, gpointer data) {
    (void)data; TableModel *m = (TableModel *)object; m->busy = FALSE;
    g_autoptr(GError) error = NULL; g_autoptr(ZtPage) page = g_task_propagate_pointer(G_TASK(result), &error);
    if (!m->owner || zen_window_is_closed(m->owner->shell) || g_cancellable_is_cancelled(m->cancel)) return;
    if (page) {
        CachedPage *cached = g_new0(CachedPage, 1); cached->stamp = ++m->stamp;
        cached->items = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; i < page->rows->len; i++) {
            Row *row = g_object_new(row_get_type(), NULL); row->index = page->start + i;
            row->values = g_ptr_array_index(page->rows, i); g_ptr_array_index(page->rows, i) = NULL;
            g_ptr_array_add(cached->items, row);
            for (guint j = 0; row->values[j]; j++) cached->bytes += strlen(row->values[j]) + sizeof(char *) + 1;
        }
        trim_pages(m, cached->bytes);
        g_hash_table_replace(m->pages, GUINT_TO_POINTER(page->start / ZT_PAGE_SIZE + 1), cached);
        guint selected = gtk_single_selection_get_selected(m->owner->selection);
        g_list_model_items_changed(G_LIST_MODEL(m), page->start, cached->items->len, cached->items->len);
        /* Replacing placeholder rows should not clear the user's selection. */
        gtk_single_selection_set_selected(m->owner->selection, selected);
    } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_hash_table_add(m->failed, GUINT_TO_POINTER(m->active_page + 1));
        zen_window_notice(m->owner->shell, error ? error->message : "Unable to read this page.");
    }
    if (!m->owner->indexing && !m->owner->in_search) zen_window_hide_progress(m->owner->shell);
    model_pump(m);
}
static void model_pump(TableModel *m) {
    if (m->busy || !m->owner || g_cancellable_is_cancelled(m->cancel)) return;
    while (!g_queue_is_empty(&m->pending)) {
        gpointer key = g_queue_pop_head(&m->pending);
        if (g_hash_table_contains(m->pages, key) || g_hash_table_contains(m->failed, key)) continue;
        m->active_page = GPOINTER_TO_UINT(key) - 1; m->busy = TRUE;
        if (!m->owner->indexing && !m->owner->in_search) zen_window_show_progress(m->owner->shell, -1);
        GTask *task = g_task_new(m, m->cancel, page_ready, NULL);
        g_task_set_task_data(task, GUINT_TO_POINTER(m->active_page * ZT_PAGE_SIZE), NULL);
        g_task_run_in_thread(task, fetch_page); g_object_unref(task); break;
    }
}
static void detach_model(Table *t, gboolean stop_helper) {
    if (!t->model) return;
    t->model->owner = NULL; g_cancellable_cancel(t->model->cancel);
    if (stop_helper) zt_source_cancel(t->model->source);
    g_clear_object(&t->model);
}

typedef struct { Table *table; guint column; } CellColumn;
static char *display_value(const char *value) {
    if (!value) return g_strdup("");
    /* Layout only a bounded single-line preview; copy retains the entire value. */
    g_autoptr(GString) text = g_string_sized_new(MIN(strlen(value), 512));
    guint count = 0;
    const char *p = value;
    for (; *p && count < 512; p = g_utf8_next_char(p), count++) {
        gunichar c = g_utf8_get_char(p);
        if (c == '\n' || c == '\r') g_string_append(text, " ↵ ");
        else if (c == '\t') g_string_append(text, "   ");
        else if (g_unichar_iscntrl(c)) g_string_append_c(text, ' ');
        else g_string_append_unichar(text, c);
    }
    if (*p) g_string_append(text, "…");
    return g_string_free(g_steal_pointer(&text), FALSE);
}
static void cell_pressed(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
    (void)n; (void)x; (void)y; GtkListItem *item = data;
    CellColumn *column = g_object_get_data(G_OBJECT(item), "column");
    Row *row = gtk_list_item_get_item(item);
    if (!row) return;
    if (column->table->active_cell) {
        gtk_widget_remove_css_class(column->table->active_cell, "table-active");
        g_object_remove_weak_pointer(G_OBJECT(column->table->active_cell), (gpointer *)&column->table->active_cell);
    }
    column->table->active_cell = gtk_list_item_get_child(item);
    g_object_add_weak_pointer(G_OBJECT(column->table->active_cell), (gpointer *)&column->table->active_cell);
    gtk_widget_add_css_class(column->table->active_cell, "table-active");
    column->table->selected_column = column->column;
    gtk_single_selection_set_selected(column->table->selection, row->index);
    gtk_widget_grab_focus(GTK_WIDGET(column->table->view));
    if (n == 2 && row->values && column->column < zt_source_columns(column->table->model->source)) {
        const char *value = row->values[column->column];
        GtkWidget *dialog = gtk_window_new();
        gtk_window_set_transient_for(GTK_WINDOW(dialog), zen_window_get_window(column->table->shell));
        gtk_window_set_title(GTK_WINDOW(dialog), zt_source_header(column->table->model->source, column->column));
        gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 400);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
        GtkWidget *scrolled = gtk_scrolled_window_new(), *text = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text), 24); gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text), 24);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text), 24); gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text), 24);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), value, -1);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), text); gtk_window_set_child(GTK_WINDOW(dialog), scrolled);
        gtk_window_present(GTK_WINDOW(dialog));
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }
}
static void cell_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory; CellColumn *column = data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), column->column == G_MAXUINT ? 1 : 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
    gtk_widget_add_css_class(label, column->column == G_MAXUINT ? "table-number" : "table-cell");
    gtk_list_item_set_child(item, label); g_object_set_data(G_OBJECT(item), "column", column);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(cell_pressed), item);
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(click));
}
static void cell_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory; CellColumn *column = data; Row *row = gtk_list_item_get_item(item);
    GtkLabel *label = GTK_LABEL(gtk_list_item_get_child(item));
    if (column->table->selection && row->index == gtk_single_selection_get_selected(column->table->selection) && column->column == column->table->selected_column)
        gtk_widget_add_css_class(GTK_WIDGET(label), "table-active");
    else gtk_widget_remove_css_class(GTK_WIDGET(label), "table-active");
    if (column->column == G_MAXUINT) {
        g_autofree char *number = g_strdup_printf("%u", row->index + 1); gtk_label_set_text(label, number);
    } else if (!row->values) gtk_label_set_text(label, "·");
    else {
        g_autofree char *value = display_value(row->values[column->column]);
        gtk_label_set_text(label, value);
        gtk_widget_set_tooltip_text(GTK_WIDGET(label), strlen(value) > 22 ? value : NULL);
    }
}
static void add_column(Table *t, const char *name, const char *type, guint index, int width) {
    CellColumn *data = g_new0(CellColumn, 1); data->table = t; data->column = index;
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_object_set_data_full(G_OBJECT(factory), "column", data, g_free);
    g_signal_connect(factory, "setup", G_CALLBACK(cell_setup), data); g_signal_connect(factory, "bind", G_CALLBACK(cell_bind), data);
    g_autofree char *title = display_value(name);
    GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);
    gtk_column_view_column_set_fixed_width(column, width);
    gtk_column_view_column_set_resizable(column, index != G_MAXUINT);
    if (index != G_MAXUINT && index + 1 == zt_source_columns(t->model->source)) gtk_column_view_column_set_expand(column, TRUE);
    gtk_column_view_append_column(t->view, column); g_object_unref(column);
    (void)type;
}
static void install_source(Table *t, ZtSource *source, gboolean final) {
    guint selection = t->selection ? gtk_single_selection_get_selected(t->selection) : GTK_INVALID_LIST_POSITION;
    gboolean same_schema = t->model && zt_source_columns(t->model->source) == zt_source_columns(source);
    detach_model(t, FALSE);
    t->model = g_object_new(table_model_get_type(), NULL); t->model->owner = t; t->model->source = zt_source_ref(source);
    if (!t->selection) {
        t->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(t->model)));
        gtk_single_selection_set_autoselect(t->selection, FALSE); gtk_single_selection_set_can_unselect(t->selection, TRUE);
        gtk_single_selection_set_selected(t->selection, GTK_INVALID_LIST_POSITION);
        g_signal_connect(t->selection, "notify::selected", G_CALLBACK(selected_changed), t);
        gtk_column_view_set_model(t->view, GTK_SELECTION_MODEL(t->selection));
    } else gtk_single_selection_set_model(t->selection, G_LIST_MODEL(t->model));
    if (!same_schema) {
        GListModel *columns = gtk_column_view_get_columns(t->view);
        while (g_list_model_get_n_items(columns)) {
            GtkColumnViewColumn *column = g_list_model_get_item(columns, 0); gtk_column_view_remove_column(t->view, column); g_object_unref(column);
        }
        add_column(t, "", "", G_MAXUINT, 66);
        for (guint i = 0; i < zt_source_columns(source); i++) add_column(t, zt_source_header(source, i), zt_source_type(source, i), i, 180);
    }
    if (selection < zt_source_rows(source)) gtk_single_selection_set_selected(t->selection, selection);
    t->indexing = !final; update_status(t); zen_window_set_ready(t->shell, TRUE);
}
static void update_status(Table *t) {
    if (!t->model) return;
    ZtSource *s = t->model->source;
    guint selected = t->selection ? gtk_single_selection_get_selected(t->selection) : GTK_INVALID_LIST_POSITION;
    g_autofree char *count = NULL;
    if (selected != GTK_INVALID_LIST_POSITION)
        count = g_strdup_printf("Row %u of %u%s · %u columns", selected + 1, zt_source_rows(s), t->indexing ? "+" : "", zt_source_columns(s));
    else count = g_strdup_printf("%u%s rows · %u columns", zt_source_rows(s), t->indexing ? "+" : "", zt_source_columns(s));
    gtk_label_set_text(GTK_LABEL(t->counter), count);
    gtk_widget_set_sensitive(t->find_button, !t->indexing && !t->in_search);
}
static void selected_changed(GObject *object, GParamSpec *spec, gpointer data) { (void)object; (void)spec; update_status(data); }

typedef struct { GtkWindow *window; guint generation; char *path; } OpenRequest;
typedef struct { GtkWindow *window; guint generation; ZtSource *preview; double fraction; } Progress;
static void free_request(gpointer data) { OpenRequest *r = data; g_free(r->path); g_free(r); }
static gboolean progress_ready(gpointer data) {
    Progress *p = data; Table *t = g_object_get_data(G_OBJECT(p->window), "table");
    if (t && t->opening && t->generation == p->generation && !zen_window_is_closed(t->shell)) {
        if (p->preview) install_source(t, p->preview, FALSE);
        zen_window_show_progress(t->shell, p->fraction);
    }
    if (p->preview) zt_source_unref(p->preview);
    g_object_unref(p->window); g_free(p); return G_SOURCE_REMOVE;
}
static void progress(ZtSource *preview, double fraction, gpointer data) {
    OpenRequest *r = data; Progress *p = g_new0(Progress, 1);
    p->window = g_object_ref(r->window); p->generation = r->generation; p->fraction = fraction;
    p->preview = preview ? zt_source_ref(preview) : NULL;
    g_idle_add(progress_ready, p);
}
static void open_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    (void)object; OpenRequest *r = data; g_autoptr(GError) error = NULL;
    ZtSource *source = zt_source_open(r->path, cancel, progress, r, &error);
    if (source) g_task_return_pointer(task, source, (GDestroyNotify)zt_source_unref);
    else g_task_return_error(task, g_steal_pointer(&error));
}
static void open_ready(GObject *object, GAsyncResult *result, gpointer data) {
    (void)data; Table *t = g_object_get_data(object, "table"); OpenRequest *r = g_task_get_task_data(G_TASK(result));
    g_autoptr(GError) error = NULL; g_autoptr(ZtSource) source = g_task_propagate_pointer(G_TASK(result), &error);
    if (!t || t->generation != r->generation || zen_window_is_closed(t->shell)) return;
    g_clear_object(&t->opening); t->indexing = FALSE; zen_window_hide_progress(t->shell);
    if (source) install_source(t, source, TRUE);
    else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) zen_window_notice(t->shell, error ? error->message : "Unable to open this table.");
    update_status(t);
}
static void cancel_work(Table *t) {
    if (t->opening) { g_cancellable_cancel(t->opening); g_clear_object(&t->opening); }
    if (t->finding) { g_cancellable_cancel(t->finding); g_clear_object(&t->finding); }
    if (t->find_source) { zt_source_cancel(t->find_source); g_clear_pointer(&t->find_source, zt_source_unref); }
    detach_model(t, TRUE);
    if (t->selection) gtk_single_selection_set_model(t->selection, NULL);
    if (t->jump_idle) { g_source_remove(t->jump_idle); t->jump_idle = 0; }
    t->in_search = FALSE;
}
static void open_file(ZenWindow *shell, GFile *file, gpointer data) {
    Table *t = data; g_autofree char *path = g_file_get_path(file);
    if (!path) { zen_window_notice(shell, "Save this table locally before opening it."); return; }
    cancel_work(t); t->generation++; t->indexing = TRUE; t->selected_column = 0;
    t->opening = g_cancellable_new(); zen_window_clear_notice(shell); zen_window_show_progress(shell, -1);
    zen_window_set_ready(shell, FALSE);
    g_autofree char *name = g_file_get_basename(file); zen_window_set_title(shell, name);
    OpenRequest *request = g_new0(OpenRequest, 1); request->window = zen_window_get_window(shell);
    request->path = g_strdup(path); request->generation = t->generation;
    GTask *task = g_task_new(request->window, t->opening, open_ready, NULL);
    g_task_set_task_data(task, request, free_request); g_task_run_in_thread(task, open_worker); g_object_unref(task);
}
static char *quote_tsv(const char *value) {
    if (!strpbrk(value, "\t\r\n\"")) return g_strdup(value);
    GString *quoted = g_string_new("\"");
    for (const char *p = value; *p; p++) { if (*p == '"') g_string_append_c(quoted, '"'); g_string_append_c(quoted, *p); }
    g_string_append_c(quoted, '"'); return g_string_free(quoted, FALSE);
}
static void copy_selection(Table *t, gboolean whole_row) {
    if (!t->model || !t->selection) return;
    Row *row = gtk_single_selection_get_selected_item(t->selection);
    if (!row || !row->values) return;
    if (!whole_row && t->selected_column < zt_source_columns(t->model->source)) {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t->view)), row->values[t->selected_column]);
    } else {
        g_autoptr(GString) line = g_string_new(NULL);
        for (guint i = 0; row->values[i]; i++) {
            g_autofree char *quoted = quote_tsv(row->values[i]);
            if (i) g_string_append_c(line, '\t');
            g_string_append(line, quoted);
        }
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t->view)), line->str);
    }
}
static void copy_clicked(GtkButton *button, gpointer data) { (void)button; copy_selection(data, FALSE); }
static gboolean finish_jump(gpointer data) {
    Table *t = data; t->jump_idle = 0;
    if (!t->model || zen_window_is_closed(t->shell)) return G_SOURCE_REMOVE;
    GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(t->scroll);
    guint rows = zt_source_rows(t->model->source);
    double height = rows ? gtk_adjustment_get_upper(a) / rows : 0;
    gtk_adjustment_set_value(a, MIN(gtk_adjustment_get_upper(a) - gtk_adjustment_get_page_size(a), t->jump_row * height));
    return G_SOURCE_REMOVE;
}
static void jump_to(Table *t, guint row) {
    if (!t->model || row >= zt_source_rows(t->model->source)) return;
    gtk_single_selection_set_selected(t->selection, row);
    t->jump_row = row;
    if (t->jump_idle) g_source_remove(t->jump_idle);
    t->jump_idle = g_idle_add(finish_jump, t);
}
typedef struct { ZtSource *source; guint start, generation; char *text; } FindRequest;
static void free_find(gpointer data) { FindRequest *r = data; zt_source_unref(r->source); g_free(r->text); g_free(r); }
static void find_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    (void)object; FindRequest *r = data; g_autoptr(GError) error = NULL;
    gint64 found = zt_source_find(r->source, r->start, r->text, cancel, &error);
    if (error) g_task_return_error(task, g_steal_pointer(&error));
    else { gint64 *result = g_new(gint64, 1); *result = found; g_task_return_pointer(task, result, g_free); }
}
static void find_ready(GObject *object, GAsyncResult *result, gpointer data) {
    (void)data; Table *t = g_object_get_data(object, "table"); FindRequest *r = g_task_get_task_data(G_TASK(result));
    g_autoptr(GError) error = NULL; g_autofree gint64 *row = g_task_propagate_pointer(G_TASK(result), &error);
    if (!t || r->generation != t->generation || zen_window_is_closed(t->shell)) return;
    t->in_search = FALSE; g_clear_object(&t->finding); g_clear_pointer(&t->find_source, zt_source_unref);
    zen_window_hide_progress(t->shell); update_status(t);
    if (row && *row >= 0) jump_to(t, *row);
    else if (row) zen_window_notice(t->shell, "No matching rows.");
    else if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) zen_window_notice(t->shell, error->message);
}
static void find_next(GtkWidget *widget, gpointer data) {
    (void)widget; Table *t = data;
    if (!t->model || t->in_search || t->indexing) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(t->search)); if (!*text) return;
    guint selected = gtk_single_selection_get_selected(t->selection);
    FindRequest *r = g_new0(FindRequest, 1); r->source = zt_source_ref(t->model->source);
    r->text = g_strdup(text); r->generation = t->generation; r->start = selected == GTK_INVALID_LIST_POSITION ? 0 : selected + 1;
    t->finding = g_cancellable_new(); t->find_source = zt_source_ref(r->source); t->in_search = TRUE;
    zen_window_show_progress(t->shell, -1); update_status(t);
    GTask *task = g_task_new(zen_window_get_window(t->shell), t->finding, find_ready, NULL);
    g_task_set_task_data(task, r, free_find); g_task_run_in_thread(task, find_worker); g_object_unref(task);
}
static void show_find(GtkButton *button, gpointer data) {
    (void)button; Table *t = data; gtk_widget_set_visible(t->search_box, !gtk_widget_get_visible(t->search_box));
    if (gtk_widget_get_visible(t->search_box)) gtk_widget_grab_focus(t->search);
}
static void goto_row(GtkEntry *entry, gpointer data) {
    Table *t = data; const char *text = gtk_editable_get_text(GTK_EDITABLE(entry)); char *end;
    guint64 row = g_ascii_strtoull(text, &end, 10);
    if (end != text && !*end && row && row <= G_MAXUINT) jump_to(t, row - 1);
    gtk_widget_set_visible(t->goto_box, FALSE); gtk_widget_grab_focus(GTK_WIDGET(t->view));
}
static void show_info(GtkButton *button, gpointer data) {
    (void)button; Table *t = data; if (!t->model) return;
    ZtSource *s = t->model->source;
    GtkWidget *popover = gtk_popover_new(); gtk_widget_set_parent(popover, t->info);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    GtkWidget *title = gtk_label_new(zt_source_is_parquet(s) ? "Parquet · schema" : "Delimited text · first row is the header");
    gtk_widget_add_css_class(title, "heading"); gtk_box_append(GTK_BOX(box), title);
    for (guint i = 0; i < zt_source_columns(s); i++) {
        g_autofree char *name = display_value(zt_source_header(s, i));
        g_autofree char *line = g_strdup_printf("%s    %s", name, zt_source_type(s, i));
        GtkWidget *label = gtk_label_new(line); gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE); gtk_box_append(GTK_BOX(box), label);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 360);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 300);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box); gtk_popover_set_child(GTK_POPOVER(popover), scroll);
    g_signal_connect_swapped(popover, "closed", G_CALLBACK(gtk_widget_unparent), popover); gtk_popover_popup(GTK_POPOVER(popover));
}
static gboolean keypress(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer data) {
    (void)controller; (void)code; Table *t = data; gboolean ctrl = state & GDK_CONTROL_MASK;
    if (ctrl && (key == GDK_KEY_f || key == GDK_KEY_F)) { gtk_widget_set_visible(t->search_box, TRUE); gtk_widget_grab_focus(t->search); return TRUE; }
    if (ctrl && (key == GDK_KEY_l || key == GDK_KEY_L)) { gtk_widget_set_visible(t->goto_box, TRUE); gtk_widget_grab_focus(t->goto_entry); return TRUE; }
    if (key == GDK_KEY_Escape && (gtk_widget_get_visible(t->search_box) || gtk_widget_get_visible(t->goto_box))) {
        gtk_widget_set_visible(t->search_box, FALSE); gtk_widget_set_visible(t->goto_box, FALSE); gtk_widget_grab_focus(GTK_WIDGET(t->view)); return TRUE;
    }
    GtkWidget *focus = gtk_window_get_focus(zen_window_get_window(t->shell));
    if (focus && (GTK_IS_EDITABLE(focus) || gtk_widget_is_ancestor(focus, t->search_box) || gtk_widget_is_ancestor(focus, t->goto_box))) return FALSE;
    if (ctrl && (key == GDK_KEY_c || key == GDK_KEY_C)) { copy_selection(t, state & GDK_SHIFT_MASK); return TRUE; }
    if (key == GDK_KEY_F3) { find_next(NULL, t); return TRUE; }
    if (ctrl && key == GDK_KEY_Home) { jump_to(t, 0); return TRUE; }
    if (ctrl && key == GDK_KEY_End && t->model) { guint rows = zt_source_rows(t->model->source); if (rows) jump_to(t, rows - 1); return TRUE; }
    return FALSE;
}
static gboolean claim_pointer(double x, double y, gpointer data) {
    Table *t = data; (void)x;
    return t->model && y >= 48 && y < gtk_widget_get_height(zen_window_get_root(t->shell)) - 70;
}
static gboolean closing(GtkWindow *window, gpointer data) { (void)window; Table *t = data; t->generation++; cancel_work(t); return FALSE; }
static void free_table(gpointer data) { Table *t = data;
    if (t->active_cell) g_object_remove_weak_pointer(G_OBJECT(t->active_cell), (gpointer *)&t->active_cell);
    cancel_work(t); g_clear_object(&t->selection); g_free(t); }
static Table *new_table(GtkApplication *app) {
    Table *t = g_new0(Table, 1); t->shell = zen_window_new(app, "Zen Table", 1120, 760);
    GtkWindow *window = zen_window_get_window(t->shell);
    g_object_set_data_full(G_OBJECT(window), "table", t, free_table); g_signal_connect(window, "close-request", G_CALLBACK(closing), t);
    zen_window_set_open_handler(t->shell, open_file, t); zen_window_set_pointer_claim_func(t->shell, claim_pointer, t);
    zen_window_set_empty_text(t->shell, "Zen Table", "A little room to see the data.\nOpen a CSV, TSV, or Parquet file.");
    const char *mimes[] = {"text/csv", "text/tab-separated-values", "application/vnd.apache.parquet", NULL};
    const char *patterns[] = {"*.csv", "*.CSV", "*.tsv", "*.TSV", "*.tab", "*.parquet", "*.PARQUET", NULL};
    zen_window_add_file_filter(t->shell, "Tables", mimes, patterns);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_widget_add_css_class(box, "table-sheet");

    t->search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); gtk_widget_add_css_class(t->search_box, "table-search");
    t->search = gtk_search_entry_new(); gtk_widget_set_hexpand(t->search, TRUE);
    g_object_set(t->search, "placeholder-text", "Find a value in any row…", NULL);
    gtk_box_append(GTK_BOX(t->search_box), t->search);
    t->find_button = gtk_button_new_with_label("Find next"); g_signal_connect(t->find_button, "clicked", G_CALLBACK(find_next), t);
    gtk_box_append(GTK_BOX(t->search_box), t->find_button); gtk_widget_set_visible(t->search_box, FALSE); gtk_box_append(GTK_BOX(box), t->search_box);
    g_signal_connect(t->search, "activate", G_CALLBACK(find_next), t);
    t->goto_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); gtk_widget_add_css_class(t->goto_box, "table-search");
    gtk_box_append(GTK_BOX(t->goto_box), gtk_label_new("Go to row")); t->goto_entry = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(t->goto_entry), GTK_INPUT_PURPOSE_DIGITS); gtk_box_append(GTK_BOX(t->goto_box), t->goto_entry);
    g_signal_connect(t->goto_entry, "activate", G_CALLBACK(goto_row), t); gtk_widget_set_visible(t->goto_box, FALSE); gtk_box_append(GTK_BOX(box), t->goto_box);
    t->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new()); gtk_widget_set_vexpand(GTK_WIDGET(t->scroll), TRUE);
    gtk_scrolled_window_set_policy(t->scroll, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    t->view = GTK_COLUMN_VIEW(gtk_column_view_new(NULL)); gtk_widget_add_css_class(GTK_WIDGET(t->view), "zen-table");
    gtk_column_view_set_show_row_separators(t->view, TRUE); gtk_column_view_set_show_column_separators(t->view, FALSE);
    gtk_column_view_set_reorderable(t->view, FALSE); gtk_widget_set_focusable(GTK_WIDGET(t->view), TRUE);
    gtk_scrolled_window_set_child(t->scroll, GTK_WIDGET(t->view)); gtk_box_append(GTK_BOX(box), GTK_WIDGET(t->scroll)); zen_window_set_child(t->shell, box);
    GtkWidget *controls = zen_window_get_controls(t->shell);
    t->counter = gtk_label_new(""); gtk_widget_add_css_class(t->counter, "table-counter"); gtk_box_append(GTK_BOX(controls), t->counter);
    gtk_box_append(GTK_BOX(controls), zen_button_new("edit-find-symbolic", "Find a value (Ctrl+F)", G_CALLBACK(show_find), t));
    gtk_box_append(GTK_BOX(controls), zen_button_new("edit-copy-symbolic", "Copy cell (Ctrl+C); copy row (Ctrl+Shift+C)", G_CALLBACK(copy_clicked), t));
    t->info = zen_button_new("dialog-information-symbolic", "Column names and types", G_CALLBACK(show_info), t); gtk_box_append(GTK_BOX(controls), t->info);
    GtkEventController *keys = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(keypress), t); gtk_widget_add_controller(GTK_WIDGET(window), keys);
    zen_window_set_ready(t->shell, FALSE); zen_window_present(t->shell); return t;
}
static void startup(GApplication *app, gpointer data) {
    (void)app; (void)data; zen_ui_init(); GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".table-sheet {padding:50px 0 54px;}"
        ".table-active {box-shadow:inset 0 0 0 1px #739b79;background:#d4e5d2;}"
        ".table-sheet, columnview.zen-table, columnview.zen-table listview {background:#f7f8f4;color:#29372f;}"
        "columnview.zen-table header button {background:#eef1e9;color:#4b6254;border:0;border-bottom:1px solid #d5ddd2;min-height:42px;padding:0 16px;font-weight:600;}"
        "columnview.zen-table row {min-height:37px;background:#f7f8f4;border-color:#e7ebe3;}"
        "columnview.zen-table row:nth-child(even) {background:#f2f5ee;}"
        "columnview.zen-table row:selected {background:#dbe9da;color:#1c3727;}"
        "columnview.zen-table cell {padding:0;}"
        ".table-cell {padding:9px 16px;font-size:13px;}"
        ".table-number {padding:9px 14px;color:#899688;font-size:11px;font-feature-settings:'tnum';}"
        ".table-search {padding:12px 22px;background:#eef1e9;}"
        ".table-counter {font-size:12px;opacity:.8;}", -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(css);
}
static void activate(GtkApplication *app, gpointer data) { (void)data; new_table(app); }
static void opened(GtkApplication *app, GFile **files, int count, const char *hint, gpointer data) {
    (void)hint; (void)data; for (int i = 0; i < count; i++) { Table *t = new_table(app); open_file(t->shell, files[i], t); }
}
static int inspect(const char *path, guint start, guint count, const char *needle) {
    g_autoptr(GError) error = NULL; g_autoptr(GCancellable) cancel = g_cancellable_new();
    g_autoptr(ZtSource) s = zt_source_open(path, cancel, NULL, NULL, &error);
    if (!s) { g_printerr("%s\n", error->message); return 1; }
    g_print("%u rows, %u columns, %s\n", zt_source_rows(s), zt_source_columns(s), zt_source_is_parquet(s) ? "Parquet" : "delimited text");
    for (guint i = 0; i < zt_source_columns(s); i++) {
        g_autofree char *header = quote_tsv(zt_source_header(s, i)); g_print("%s%s", i ? "\t" : "", header);
    }
    g_print("\n");
    g_autoptr(ZtPage) page = zt_source_page(s, start, count, cancel, &error);
    if (!page) { g_printerr("%s\n", error->message); return 1; }
    for (guint i = 0; i < page->rows->len; i++) {
        char **row = g_ptr_array_index(page->rows, i);
        for (guint j = 0; row[j]; j++) { g_autofree char *value = quote_tsv(row[j]); g_print("%s%s", j ? "\t" : "", value); }
        g_print("\n");
    }
    if (needle) {
        gint64 found = zt_source_find(s, start, needle, cancel, &error);
        if (error) { g_printerr("%s\n", error->message); return 1; }
        g_print("match: %" G_GINT64_FORMAT "\n", found < 0 ? -1 : found + 1);
    }
    return 0;
}
int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    if (argc > 1 && !strcmp(argv[1], "--version")) { g_print("zen-table %s\n", ZEN_VERSION); return 0; }
    if (argc > 2 && !strcmp(argv[1], "--inspect")) return inspect(argv[2], argc > 3 ? g_ascii_strtoull(argv[3], NULL, 10) : 0,
        argc > 4 ? g_ascii_strtoull(argv[4], NULL, 10) : 5, argc > 5 ? argv[5] : NULL);
    GtkApplication *app = gtk_application_new("io.github.zenviewers.Table", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(startup), NULL); g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(opened), NULL); int status = g_application_run(G_APPLICATION(app), argc, argv); g_object_unref(app); return status;
}
