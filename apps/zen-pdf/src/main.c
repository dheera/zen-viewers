/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Keep the ABI floor at GTK 4.6 (Ubuntu 22.04). */
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_6
#include "zen-ui.h"
#include <poppler.h>
#include <math.h>
#include <errno.h>
#include <string.h>

#define VERSION "0.2.0"
#define CACHE_SIZE 3
#define PIXEL_BUDGET (8 * 1024 * 1024)

typedef struct { double w, h; } Size;
typedef struct {
    gint refs;
    PopplerDocument *pdf;
    GMutex pdf_lock; /* Only workers enter Poppler; a document has one caller at a time. */
    GArray *sizes;
    char *name;
} Document;
typedef struct {
    cairo_surface_t *surface;
    int page, pixels;
    guint64 age;
} Raster;
typedef struct PageText PageText;
typedef struct {
    PageText *text;
    guint load_idle, scroll_timer;
    gboolean loading, want_all, want_copy, dragging;
    int anchor_start, anchor_end, start, end, clicks;
    double press_x, press_y, pointer_x, pointer_y;
} Selection;
typedef struct {
    GtkApplication *application;
    ZenWindow *shell;
    GtkWindow *window;
    GtkWidget *canvas, *prev, *next, *counter, *jump, *zoom_label, *fit_button;
    GtkWidget *password_dialog;
    Document *doc;
    Raster cache[CACHE_SIZE];
    Selection selection;
    gboolean force_pan;
    guint64 age;
    guint render_timer, layout_timer;
    guint open_serial;
    GCancellable *opening;
    gboolean closed, rendering, fit_page, scroll_gesture, swipe_handled;
    double zoom, offset, offset_x, drag_x, drag_y, swipe;
    gint64 last_flip;
    int page, width, height, failed_page, failed_pixels;
} App;
typedef struct {
    Document *doc;
    int page, pixels;
} RenderJob;
typedef struct { GFile *file; char *password; guint serial; } OpenJob;
typedef struct { double x, y, w, h, scale, max_offset; } Layout;

static void request_render(App *a);
static void refresh(App *a);
static void reveal(App *a);
static void open_file(App *a, GFile *file, const char *password);
static void go_page(App *a, int page);

static Document *document_ref(Document *d) { g_atomic_int_inc(&d->refs); return d; }
static void document_unref(Document *d) {
    if (d && g_atomic_int_dec_and_test(&d->refs)) {
        g_mutex_clear(&d->pdf_lock); g_object_unref(d->pdf); g_array_unref(d->sizes); g_free(d->name); g_free(d);
    }
}
static Document *document_load(GFile *file, const char *password, GCancellable *cancel, GError **error) {
    char *path = g_file_get_path(file);
    if (!path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Please open a local PDF file.");
        return NULL;
    }
    g_free(path);
    char *uri = g_file_get_uri(file);
    PopplerDocument *pdf = poppler_document_new_from_file(uri, password, error);
    g_free(uri);
    if (!pdf) return NULL;
    int n = poppler_document_get_n_pages(pdf);
    if (n < 1 || n > 100000) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The PDF has no pages, or exceeds 100,000 pages.");
        g_object_unref(pdf); return NULL;
    }
    Document *d = g_new0(Document, 1);
    d->refs = 1; d->pdf = pdf; g_mutex_init(&d->pdf_lock);
    d->sizes = g_array_sized_new(FALSE, FALSE, sizeof(Size), n);
    d->name = g_file_get_basename(file);
    for (int i = 0; i < n; i++) {
        if (cancel && g_cancellable_set_error_if_cancelled(cancel, error)) { document_unref(d); return NULL; }
        PopplerPage *p = poppler_document_get_page(pdf, i);
        Size s = {0, 0};
        if (p) { poppler_page_get_size(p, &s.w, &s.h); g_object_unref(p); }
        if (!isfinite(s.w) || !isfinite(s.h) || s.w < 1 || s.h < 1 || s.w > 1000000 || s.h > 1000000) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The PDF contains an invalid page size.");
            document_unref(d); return NULL;
        }
        g_array_append_val(d->sizes, s);
    }
    return d;
}

/* Geometry is in logical pixels; raster resolution is independent of window size. */
static Layout layout_for(Size s, int width, int height, gboolean fit_page, double zoom, double offset) {
    Layout l = {0};
    if (width <= 0 || height <= 0 || s.w <= 0 || s.h <= 0) return l;
    l.scale = (double)width / s.w;
    if (fit_page) l.scale = MIN(l.scale, (double)height / s.h);
    l.scale *= zoom;
    l.w = s.w * l.scale; l.h = s.h * l.scale;
    l.max_offset = MAX(0, l.h - height);
    l.x = (width - l.w) / 2;
    l.y = l.h < height ? (height - l.h) / 2 : -CLAMP(offset, 0, l.max_offset);
    return l;
}
static int raster_width(Size s, double desired) {
    double max_width = MIN(sqrt((double)PIXEL_BUDGET * s.w / s.h), floor(32767.0 * s.w / s.h));
    /* Include ceil(height) padding in the budget, even for extreme aspect ratios. */
    int w = (int)MAX(1, floor(MIN(MIN(desired, max_width), 16384)));
    while (w > 1 && (double)w * ceil(w * s.h / s.w) > PIXEL_BUDGET) w--;
    return w;
}
static cairo_surface_t *render_page(Document *d, int page, int pixels, GError **error) {
    if (page < 0 || page >= (int)d->sizes->len) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Page number out of range."); return NULL;
    }
    Size s = g_array_index(d->sizes, Size, page);
    int width = raster_width(s, pixels);
    double scale = width / s.w;
    double height = ceil(s.h * width / s.w);
    if (height > 32767 || width * height > PIXEL_BUDGET) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This page's aspect ratio exceeds the renderer limit."); return NULL;
    }
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, (int)height);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    PopplerPage *p = poppler_document_get_page(d->pdf, page);
    if (p) { poppler_page_render(p, cr); g_object_unref(p); }
    cairo_status_t status = cairo_status(cr);
    if (status == CAIRO_STATUS_SUCCESS) status = cairo_surface_status(surface);
    cairo_destroy(cr);
    if (!p || status != CAIRO_STATUS_SUCCESS) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not render page: %s", cairo_status_to_string(status));
        cairo_surface_destroy(surface); return NULL;
    }
    cairo_surface_flush(surface); return surface;
}
static Layout current_layout(App *a) {
    if (!a->doc) return (Layout){0};
    Layout l = layout_for(g_array_index(a->doc->sizes, Size, a->page), a->width, a->height, a->fit_page, a->zoom, a->offset);
    double range = MAX(0, (l.w - a->width) / 2);
    l.x += CLAMP(a->offset_x, -range, range);
    return l;
}
static void clamp_offsets(App *a) {
    Layout l = current_layout(a);
    double range = MAX(0, (l.w - a->width) / 2);
    a->offset_x = CLAMP(a->offset_x, -range, range);
    a->offset = CLAMP(a->offset, 0, l.max_offset);
}
static int desired_pixels(App *a) {
    Size s = g_array_index(a->doc->sizes, Size, a->page);
    Layout l = current_layout(a);
    int scale = gtk_widget_get_scale_factor(a->canvas);
    return raster_width(s, MAX(1, ceil(l.w * scale)));
}
static Raster *cached(App *a, int page, int pixels, gboolean exact) {
    Raster *best = NULL;
    for (int i = 0; i < CACHE_SIZE; i++) {
        Raster *r = &a->cache[i];
        if (!r->surface || r->page != page) continue;
        if (r->pixels == pixels) return r;
        if (!exact && (!best || ABS(r->pixels - pixels) < ABS(best->pixels - pixels))) best = r;
    }
    return best;
}
static void clear_cache(App *a) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (a->cache[i].surface) cairo_surface_destroy(a->cache[i].surface);
        a->cache[i] = (Raster){0};
    }
}
static void notice(App *a, const char *text) { zen_window_notice(a->shell, text); }
#include "selection.inc"

static void render_job_free(gpointer data) {
    RenderJob *j = data; document_unref(j->doc); g_free(j);
}
static void render_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; (void)cancel;
    RenderJob *j = data; GError *error = NULL;
    g_mutex_lock(&j->doc->pdf_lock);
    cairo_surface_t *surface = render_page(j->doc, j->page, j->pixels, &error);
    g_mutex_unlock(&j->doc->pdf_lock);
    if (surface) g_task_return_pointer(task, surface, (GDestroyNotify)cairo_surface_destroy);
    else g_task_return_error(task, error);
}
static void render_done(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; App *a = data;
    GTask *task = G_TASK(result); RenderJob *j = g_task_get_task_data(task);
    GError *error = NULL;
    cairo_surface_t *surface = g_task_propagate_pointer(task, &error);
    a->rendering = FALSE;
    if (!a->closed && a->doc == j->doc) {
        if (surface) {
            int slot = 0;
            for (int i = 0; i < CACHE_SIZE; i++) {
                if (!a->cache[i].surface) { slot = i; break; }
                if (a->cache[i].age < a->cache[slot].age) slot = i;
            }
            if (a->cache[slot].surface) cairo_surface_destroy(a->cache[slot].surface);
            a->cache[slot] = (Raster){surface, j->page, j->pixels, ++a->age};
            surface = NULL;
        } else {
            a->failed_page = j->page; a->failed_pixels = j->pixels;
            notice(a, error ? error->message : "Could not render this page.");
        }
        gtk_widget_queue_draw(a->canvas);
    }
    if (surface) cairo_surface_destroy(surface);
    g_clear_error(&error);
    if (!a->closed) request_render(a);
    g_application_release(G_APPLICATION(a->application));
}
static gboolean start_render(gpointer data) {
    App *a = data; a->render_timer = 0;
    if (a->closed || !a->doc || a->rendering || a->width < 1) return G_SOURCE_REMOVE;
    int pixels = desired_pixels(a);
    Raster *r = cached(a, a->page, pixels, TRUE);
    if (r) { r->age = ++a->age; return G_SOURCE_REMOVE; }
    if (a->failed_page == a->page && a->failed_pixels == pixels) return G_SOURCE_REMOVE;
    RenderJob *j = g_new0(RenderJob, 1);
    j->doc = document_ref(a->doc); j->page = a->page; j->pixels = pixels;
    a->rendering = TRUE;
    GTask *task = g_task_new(a->window, NULL, render_done, a);
    g_task_set_task_data(task, j, render_job_free);
    g_application_hold(G_APPLICATION(a->application));
    g_task_run_in_thread(task, render_thread); g_object_unref(task);
    return G_SOURCE_REMOVE;
}
static void request_render(App *a) {
    if (a->closed || !a->doc || a->rendering) return;
    if (a->render_timer) g_source_remove(a->render_timer);
    a->render_timer = g_timeout_add(45, start_render, a);
}
static gboolean update_controls(gpointer data) {
    App *a = data; a->layout_timer = 0;
    if (!a->closed) gtk_widget_set_visible(a->zoom_label, a->width >= 430);
    return G_SOURCE_REMOVE;
}
static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    (void)area; App *a = data;
    if (width != a->width || height != a->height) {
        a->width = width; a->height = height;
        if (!a->layout_timer) a->layout_timer = g_idle_add(update_controls, a);
        if (a->doc) { clamp_offsets(a); request_render(a); }
    }
    cairo_set_source_rgb(cr, .09, .105, .10); cairo_paint(cr);
    if (!a->doc) return;
    Layout l = current_layout(a);
    cairo_rectangle(cr, l.x, l.y, l.w, l.h);
    cairo_set_source_rgb(cr, 1, 1, 1); cairo_fill(cr);
    Raster *r = cached(a, a->page, desired_pixels(a), FALSE);
    if (!r) return;
    cairo_save(cr); cairo_rectangle(cr, l.x, l.y, l.w, l.h); cairo_clip(cr);
    cairo_translate(cr, l.x, l.y);
    double sw = cairo_image_surface_get_width(r->surface);
    /* Use the same scale on both axes; the last raster row can contain padding. */
    cairo_scale(cr, l.w / sw, l.w / sw);
    cairo_set_source_surface(cr, r->surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr); cairo_restore(cr);
    selection_draw(a, cr, l);
    selection_schedule(a);
}

static void reveal(App *a) { zen_window_reveal_controls(a->shell); }
static void refresh(App *a) {
    gboolean has = a->doc != NULL;

    gtk_widget_set_sensitive(a->prev, has && a->page > 0);
    gtk_widget_set_sensitive(a->next, has && a->page + 1 < (int)a->doc->sizes->len);
    gtk_widget_set_sensitive(a->jump, has);
    if (has) {
        char *count = g_strdup_printf("/ %u", a->doc->sizes->len);
        char *page = g_strdup_printf("%d", a->page + 1);
        gtk_label_set_text(GTK_LABEL(a->counter), count);
        gtk_editable_set_text(GTK_EDITABLE(a->jump), page);
        g_free(count); g_free(page);
    }
    char *zoom = g_strdup_printf("%s · %.0f%%", a->fit_page ? "Page" : "Width", a->zoom * 100);
    gtk_label_set_text(GTK_LABEL(a->zoom_label), zoom); g_free(zoom);
    gtk_widget_set_tooltip_text(a->fit_button, a->fit_page ? "Fit width (W)" : "Fit page (P)");
    gtk_widget_queue_draw(a->canvas); request_render(a);
}
static void go_page(App *a, int page) {
    if (!a->doc) return;
    page = CLAMP(page, 0, (int)a->doc->sizes->len - 1);
    if (page == a->page) return;
    selection_reset(a);
    a->page = page; a->offset = 0; a->offset_x = 0;
    a->failed_page = -1; refresh(a);
}
static void set_zoom(App *a, double zoom) {
    double before = current_layout(a).scale;
    a->zoom = CLAMP(zoom, .5, 4.0);
    double after = current_layout(a).scale;
    if (before > 0) {
        a->offset = MAX(0, (a->offset + a->height / 2.0) * after / before - a->height / 2.0);
        a->offset_x *= after / before;
    }
    clamp_offsets(a);
    refresh(a);
}
static void set_fit(App *a, gboolean page) {
    a->fit_page = page; a->zoom = 1; a->offset = 0; a->offset_x = 0; refresh(a);
}
static void prev_clicked(GtkButton *b, gpointer data) { (void)b; App *a = data; go_page(a, a->page - 1); }
static void next_clicked(GtkButton *b, gpointer data) { (void)b; App *a = data; go_page(a, a->page + 1); }
static void minus_clicked(GtkButton *b, gpointer data) { (void)b; App *a = data; set_zoom(a, a->zoom / 1.2); }
static void plus_clicked(GtkButton *b, gpointer data) { (void)b; App *a = data; set_zoom(a, a->zoom * 1.2); }
static void fit_clicked(GtkButton *b, gpointer data) { (void)b; App *a = data; set_fit(a, !a->fit_page); }
static void jump_activate(GtkEntry *entry, gpointer data) {
    App *a = data; const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    char *end = NULL; errno = 0; long page = strtol(text, &end, 10);
    if (a->doc && !errno && end != text && !*end && page >= 1 && page <= a->doc->sizes->len) go_page(a, page - 1);
    else notice(a, "Enter a page number within this document.");
    refresh(a); gtk_widget_grab_focus(a->canvas); reveal(a);
}
static gboolean key_pressed(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer data) {
    (void)controller; (void)code; App *a = data;
    gboolean ctrl = state & GDK_CONTROL_MASK;
    /* Shared shortcuts must reach the shell before plain W is treated as fit-width. */
    if (ctrl && (key == GDK_KEY_w || key == GDK_KEY_W || key == GDK_KEY_q ||
                 key == GDK_KEY_Q || key == GDK_KEY_o || key == GDK_KEY_O)) return FALSE;
    if (key == GDK_KEY_Escape) {
        if (a->selection.end != a->selection.start || a->selection.want_all) {
            selection_clear(a); return TRUE;
        }
        if (gtk_window_is_fullscreen(a->window)) gtk_window_unfullscreen(a->window);
        gtk_widget_grab_focus(a->canvas); reveal(a); return TRUE;
    }
    if (ctrl && key == GDK_KEY_l) {
        reveal(a); gtk_widget_grab_focus(a->jump); gtk_editable_select_region(GTK_EDITABLE(a->jump), 0, -1); return TRUE;
    }
    GtkWidget *focus = gtk_window_get_focus(a->window);
    if (focus && GTK_IS_EDITABLE(focus)) return FALSE;
    if (ctrl && (key == GDK_KEY_c || key == GDK_KEY_C || key == GDK_KEY_Insert || key == GDK_KEY_KP_Insert)) {
        selection_copy(a); return TRUE;
    }
    if (ctrl && (key == GDK_KEY_a || key == GDK_KEY_A)) { selection_all(a); return TRUE; }
    if (key == GDK_KEY_space && focus && GTK_IS_BUTTON(focus)) return FALSE;
    if ((state & GDK_SHIFT_MASK) && (key == GDK_KEY_Left || key == GDK_KEY_Right)) {
        a->offset_x += key == GDK_KEY_Left ? 80 : -80; clamp_offsets(a);
        gtk_widget_queue_draw(a->canvas); return TRUE;
    }
    switch (key) {
    case GDK_KEY_Right: case GDK_KEY_Page_Down: go_page(a, a->page + 1); return TRUE;
    case GDK_KEY_Left: case GDK_KEY_Page_Up: go_page(a, a->page - 1); return TRUE;
    case GDK_KEY_Home: go_page(a, 0); return TRUE;
    case GDK_KEY_End: if (a->doc) go_page(a, a->doc->sizes->len - 1); return TRUE;
    case GDK_KEY_Down: a->offset += 64; break;
    case GDK_KEY_Up: a->offset -= 64; break;
    case GDK_KEY_space: a->offset += (state & GDK_SHIFT_MASK ? -1 : 1) * a->height * .85; break;
    case GDK_KEY_f: zen_window_toggle_fullscreen(a->shell); return TRUE;
    case GDK_KEY_w: set_fit(a, FALSE); return TRUE;
    case GDK_KEY_p: set_fit(a, TRUE); return TRUE;
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add: set_zoom(a, a->zoom * 1.2); return TRUE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract: set_zoom(a, a->zoom / 1.2); return TRUE;
    case GDK_KEY_0: set_fit(a, FALSE); return TRUE;
    case GDK_KEY_Tab: reveal(a); return FALSE;
    default: return FALSE;
    }
    clamp_offsets(a);
    gtk_widget_queue_draw(a->canvas); return TRUE;
}
static gboolean scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer data) {
    App *a = data;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    if (state & GDK_CONTROL_MASK) { set_zoom(a, a->zoom * exp(-dy * .10)); return TRUE; }
    Layout l = current_layout(a);
    if ((state & GDK_SHIFT_MASK) || (l.w > a->width + 1 && fabs(dx) > fabs(dy))) {
        a->offset_x -= (state & GDK_SHIFT_MASK ? dy : dx) * 48;
        clamp_offsets(a); gtk_widget_queue_draw(a->canvas); return TRUE;
    }
    if (fabs(dx) > fabs(dy) && fabs(dx) > .01) {
        if (a->swipe_handled) return TRUE;
        a->swipe += dx;
        gint64 now = g_get_monotonic_time();
        if (fabs(a->swipe) >= (a->scroll_gesture ? 2 : 1) && now - a->last_flip > 300000) {
            go_page(a, a->page + (a->swipe > 0 ? 1 : -1));
            a->last_flip = now; a->swipe = 0; a->swipe_handled = a->scroll_gesture;
        }
    } else {
        a->offset += dy * 48; clamp_offsets(a);
        gtk_widget_queue_draw(a->canvas);
    }
    return TRUE;
}
static void scroll_begin(GtkEventControllerScroll *c, gpointer data) {
    (void)c; App *a = data; a->scroll_gesture = TRUE; a->swipe = 0; a->swipe_handled = FALSE;
}
static void scroll_end(GtkEventControllerScroll *c, gpointer data) {
    (void)c; App *a = data; a->scroll_gesture = FALSE; a->swipe = 0; a->swipe_handled = FALSE;
}
static void drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)x; App *a = data;
    a->force_pan = force_pan(GTK_GESTURE_SINGLE(gesture));
    if (a->force_pan) a->selection.dragging = FALSE;
    if (y < 72 && !a->selection.dragging && !a->force_pan && !gtk_window_is_fullscreen(a->window)) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED); return;
    }
    a->drag_x = a->offset_x; a->drag_y = a->offset;
    gtk_widget_grab_focus(a->canvas);
}
static void drag_update(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)gesture; App *a = data;
    if (a->selection.dragging) {
        selection_drag_to(a, a->selection.press_x + x, a->selection.press_y + y); return;
    }
    a->offset_x = a->drag_x + x; a->offset = a->drag_y - y; clamp_offsets(a);
    gtk_widget_queue_draw(a->canvas);
}
static void drag_end(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)gesture; App *a = data;
    if (a->selection.dragging) { selection_release(a); return; }
    if (!a->force_pan && current_layout(a).w <= a->width + 1 && fabs(x) > 80 && fabs(x) > fabs(y) * 1.5) {
        go_page(a, a->page + (x < 0 ? 1 : -1));
    }
}

/* The shared shell moves the window from its empty top edge. Leave PDF
 * glyphs there to the canvas so headers are selectable like any other text. */
static gboolean claim_text(double x, double y, gpointer data) {
    App *a = data;
    return a->selection.dragging || selection_hit(a, x, y, FALSE) >= 0;
}
static void motion(GtkEventControllerMotion *c, double x, double y, gpointer data) {
    (void)c; App *a = data;
    if (zen_window_edge_at(a->shell, x, y) < 0)
        gtk_widget_set_cursor_from_name(a->canvas, selection_hit(a, x, y, FALSE) >= 0 ? "text" : "default");
}
static void pressed(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
    App *a = data;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    if (!event || force_pan(GTK_GESTURE_SINGLE(gesture))) return;
    gtk_widget_grab_focus(a->canvas);
    GdkDevice *device = gdk_event_get_device(event);
    if (device && gdk_device_get_source(device) != GDK_SOURCE_TOUCHSCREEN && selection_press(a, n, x, y)) return;
    selection_clear(a);
}

static void open_job_free(gpointer data) {
    OpenJob *j = data; g_object_unref(j->file);
    if (j->password) { memset(j->password, 0, strlen(j->password)); g_free(j->password); }
    g_free(j);
}
static void open_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; OpenJob *j = data;
    if (g_task_return_error_if_cancelled(task)) return;
    GError *error = NULL; Document *d = document_load(j->file, j->password, cancel, &error);
    if (g_cancellable_is_cancelled(cancel)) { if (d) document_unref(d); g_clear_error(&error); g_task_return_error_if_cancelled(task); }
    else if (d) g_task_return_pointer(task, d, (GDestroyNotify)document_unref);
    else g_task_return_error(task, error);
}
static void password_response(GtkDialog *dialog, int response, gpointer data) {
    App *a = data;
    GFile *file = g_object_get_data(G_OBJECT(dialog), "file");
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "entry");
    char *password = response == GTK_RESPONSE_ACCEPT ? g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))) : NULL;
    g_object_ref(file); a->password_dialog = NULL; gtk_window_destroy(GTK_WINDOW(dialog));
    if (password) { open_file(a, file, password); memset(password, 0, strlen(password)); g_free(password); }
    g_object_unref(file);
}
static void ask_password(App *a, GFile *file) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Unlock PDF", a->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "Cancel", GTK_RESPONSE_CANCEL, "Unlock", GTK_RESPONSE_ACCEPT, NULL);
    a->password_dialog = dialog;
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_widget_set_margin_start(box, 24); gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    gtk_box_append(GTK_BOX(box), gtk_label_new("This document needs a password."));
    GtkWidget *entry = gtk_entry_new(); gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_append(GTK_BOX(box), entry);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_object_set_data_full(G_OBJECT(dialog), "file", g_object_ref(file), g_object_unref);
    g_object_set_data(G_OBJECT(dialog), "entry", entry);
    g_signal_connect(dialog, "response", G_CALLBACK(password_response), a);
    gtk_window_present(GTK_WINDOW(dialog));
}
static void open_done(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; App *a = data; GTask *task = G_TASK(result);
    OpenJob *j = g_task_get_task_data(task); GError *error = NULL;
    Document *d = g_task_propagate_pointer(task, &error);
    if (!a->closed && j->serial == a->open_serial) {
        g_clear_object(&a->opening);
        if (d) {
            selection_reset(a); clear_cache(a); document_unref(a->doc); a->doc = d; d = NULL;
            a->page = 0; a->zoom = 1; a->offset = 0; a->offset_x = 0; a->fit_page = FALSE;
            a->failed_page = -1;
            zen_window_clear_notice(a->shell);
            zen_window_set_title(a->shell, a->doc->name);
            zen_window_set_ready(a->shell, TRUE);
            refresh(a); gtk_widget_grab_focus(a->canvas);
        } else if (error && error->domain == POPPLER_ERROR && error->code == POPPLER_ERROR_ENCRYPTED) {
            ask_password(a, j->file);
        } else if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) notice(a, error->message);
    }
    if (d) document_unref(d);
    g_clear_error(&error); g_application_release(G_APPLICATION(a->application));
}
static void open_file(App *a, GFile *file, const char *password) {
    if (a->closed) return;
    if (a->password_dialog) { gtk_window_destroy(GTK_WINDOW(a->password_dialog)); a->password_dialog = NULL; }
    if (a->opening) g_cancellable_cancel(a->opening);
    g_clear_object(&a->opening); a->opening = g_cancellable_new();
    OpenJob *j = g_new0(OpenJob, 1);
    j->file = g_object_ref(file); j->password = g_strdup(password); j->serial = ++a->open_serial;
    GTask *task = g_task_new(a->window, a->opening, open_done, a);
    g_task_set_task_data(task, j, open_job_free);
    g_application_hold(G_APPLICATION(a->application));
    g_task_run_in_thread(task, open_thread); g_object_unref(task);
    notice(a, "Opening document…");
}
static void open_from_shell(ZenWindow *shell, GFile *file, gpointer data) {
    (void)shell; open_file(data, file, NULL);
}
static void cleanup_sources(App *a) {
    selection_stop_sources(a);
    if (a->layout_timer) { g_source_remove(a->layout_timer); a->layout_timer = 0; }
    if (a->render_timer) { g_source_remove(a->render_timer); a->render_timer = 0; }
}
static gboolean close_request(GtkWindow *window, gpointer data) {
    (void)window; App *a = data; a->closed = TRUE; cleanup_sources(a);
    if (a->opening) g_cancellable_cancel(a->opening);
    return FALSE;
}
static void app_free(gpointer data) {
    App *a = data; cleanup_sources(a); page_text_free(a->selection.text); clear_cache(a); document_unref(a->doc);
    g_clear_object(&a->opening); g_object_unref(a->application); g_free(a);
}
static App *new_window(GtkApplication *application) {
    App *a = g_new0(App, 1); a->application = g_object_ref(application); a->zoom = 1; a->failed_page = -1;
    a->shell = zen_window_new(application, "Zen PDF", 720, 930);
    a->window = zen_window_get_window(a->shell);
    g_object_set_data_full(G_OBJECT(a->window), "zen-app", a, app_free);
    g_signal_connect(a->window, "close-request", G_CALLBACK(close_request), a);
    zen_window_set_pointer_claim_func(a->shell, claim_text, a);
    zen_window_set_open_handler(a->shell, open_from_shell, a);
    const char *types[] = {"application/pdf", NULL};
    const char *patterns[] = {"*.pdf", "*.PDF", NULL};
    zen_window_add_file_filter(a->shell, "PDF documents", types, patterns);
    zen_window_set_empty_text(a->shell, "A little room to read.", "Drop a PDF here, or open a document.");
    a->canvas = gtk_drawing_area_new(); gtk_widget_set_hexpand(a->canvas, TRUE); gtk_widget_set_vexpand(a->canvas, TRUE);
    gtk_widget_set_focusable(a->canvas, TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(a->canvas), GTK_ACCESSIBLE_PROPERTY_LABEL, "PDF page. Drag over text to select it. Control+C copies. Left and right turn pages; up and down scroll.", -1);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a->canvas), draw, a, NULL);
    zen_window_set_child(a->shell, a->canvas);
    GtkWidget *bottom = zen_window_get_controls(a->shell);
    a->prev = zen_button_new("go-previous-symbolic", "Previous page (Left)", G_CALLBACK(prev_clicked), a);
    a->next = zen_button_new("go-next-symbolic", "Next page (Right)", G_CALLBACK(next_clicked), a);
    gtk_box_append(GTK_BOX(bottom), a->prev);
    a->jump = gtk_entry_new(); gtk_editable_set_width_chars(GTK_EDITABLE(a->jump), 3); gtk_editable_set_max_width_chars(GTK_EDITABLE(a->jump), 6);
    gtk_entry_set_alignment(GTK_ENTRY(a->jump), 1); gtk_entry_set_input_purpose(GTK_ENTRY(a->jump), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_tooltip_text(a->jump, "Go to page (Ctrl+L)");
    gtk_accessible_update_property(GTK_ACCESSIBLE(a->jump), GTK_ACCESSIBLE_PROPERTY_LABEL, "Page number", -1);
    g_signal_connect(a->jump, "activate", G_CALLBACK(jump_activate), a);
    gtk_box_append(GTK_BOX(bottom), a->jump);
    a->counter = gtk_label_new("/ 0"); gtk_widget_add_css_class(a->counter, "muted"); gtk_box_append(GTK_BOX(bottom), a->counter);
    gtk_box_append(GTK_BOX(bottom), a->next);
    gtk_box_append(GTK_BOX(bottom), zen_button_new("zoom-out-symbolic", "Zoom out (−)", G_CALLBACK(minus_clicked), a));
    a->zoom_label = gtk_label_new("Width · 100%"); gtk_widget_add_css_class(a->zoom_label, "muted"); gtk_box_append(GTK_BOX(bottom), a->zoom_label);
    gtk_box_append(GTK_BOX(bottom), zen_button_new("zoom-in-symbolic", "Zoom in (+)", G_CALLBACK(plus_clicked), a));
    a->fit_button = zen_button_new("view-restore-symbolic", "Fit page (P)", G_CALLBACK(fit_clicked), a); gtk_box_append(GTK_BOX(bottom), a->fit_button);
    GtkEventController *motion_c = gtk_event_controller_motion_new();
    g_signal_connect(motion_c, "motion", G_CALLBACK(motion), a);
    gtk_widget_add_controller(a->canvas, motion_c);
    GtkEventController *key_c = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(key_c, GTK_PHASE_CAPTURE);
    g_signal_connect(key_c, "key-pressed", G_CALLBACK(key_pressed), a); gtk_widget_add_controller(GTK_WIDGET(a->window), key_c);
    GtkEventController *scroll_c = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    g_signal_connect(scroll_c, "scroll-begin", G_CALLBACK(scroll_begin), a);
    g_signal_connect(scroll_c, "scroll", G_CALLBACK(scroll), a); g_signal_connect(scroll_c, "scroll-end", G_CALLBACK(scroll_end), a);
    gtk_widget_add_controller(a->canvas, scroll_c);
    for (guint button_number = 1; button_number <= 2; button_number++) {
        GtkGesture *drag = gtk_gesture_drag_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), button_number);
        g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), a);
        g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), a);
        g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), a);
        gtk_widget_add_controller(a->canvas, GTK_EVENT_CONTROLLER(drag));
    }
    GtkGesture *click = gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(pressed), a); gtk_widget_add_controller(a->canvas, GTK_EVENT_CONTROLLER(click));
    refresh(a); zen_window_set_ready(a->shell, FALSE); zen_window_present(a->shell); gtk_widget_grab_focus(a->canvas);
    /* The document starts quiet; a pointer movement or Tab reveals the controls. */
    return a;
}
static void activate(GtkApplication *application, gpointer data) { (void)data; new_window(application); }
static void app_open(GtkApplication *application, GFile **files, int n, const char *hint, gpointer data) {
    (void)hint; (void)data;
    for (int i = 0; i < n; i++) { App *a = new_window(application); open_file(a, files[i], NULL); }
}
static void startup(GApplication *application, gpointer data) {
    (void)application; (void)data; zen_ui_init();
}

static void test_layout(void) {
    Layout l = layout_for((Size){600, 800}, 900, 500, FALSE, 1, 0);
    g_assert_cmpfloat(l.w, ==, 900); g_assert_cmpfloat(l.h, ==, 1200); g_assert_cmpfloat(l.max_offset, ==, 700);
    l = layout_for((Size){600, 800}, 900, 500, FALSE, 1, 9000);
    g_assert_cmpfloat(l.y, ==, -700);
    l = layout_for((Size){800, 600}, 500, 900, FALSE, 1, 20);
    g_assert_cmpfloat(l.w, ==, 500); g_assert_cmpfloat(l.h, ==, 375); g_assert_cmpfloat(l.y, ==, 262.5);
    l = layout_for((Size){600, 800}, 900, 500, TRUE, 1, 0);
    g_assert_cmpfloat(l.h, ==, 500); g_assert_cmpfloat(l.w, ==, 375);
    l = layout_for((Size){600, 800}, 600, 800, FALSE, 2, 0);
    g_assert_cmpfloat(l.w, ==, 1200); g_assert_cmpfloat(l.x, ==, -300);
}
static void test_budget(void) {
    const Size sizes[] = {{600,800},{800,600},{100,10000},{10000,100},{1,1000000}};
    for (guint i = 0; i < G_N_ELEMENTS(sizes); i++) {
        int w = raster_width(sizes[i], 1000000);
        g_assert_cmpint(w, >=, 1);
        g_assert_cmpfloat((double)w * ceil(w * sizes[i].h / sizes[i].w), <=, PIXEL_BUDGET);
    }
}
static void test_cache(void) {
    App a = {0};
    a.cache[0] = (Raster){cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1), 0, 600, 1};
    a.cache[1] = (Raster){cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1), 1, 900, 2};
    g_assert_true(cached(&a, 0, 600, TRUE) == &a.cache[0]);
    g_assert_null(cached(&a, 1, 600, TRUE));
    g_assert_true(cached(&a, 1, 600, FALSE) == &a.cache[1]);
    g_assert_null(cached(&a, 2, 600, FALSE)); clear_cache(&a);
}
static int render_cli(int argc, char **argv) {
    if (argc != 6) { g_printerr("Usage: zen-pdf --render FILE OUTPUT.png WIDTH PAGE\n"); return 2; }
    char *end = NULL; errno = 0; long width = strtol(argv[4], &end, 10);
    if (errno || end == argv[4] || *end || width < 1 || width > 16384) { g_printerr("Width must be 1–16384.\n"); return 2; }
    errno = 0; long page = strtol(argv[5], &end, 10);
    if (errno || end == argv[5] || *end || page < 1 || page > G_MAXINT) { g_printerr("Invalid page number.\n"); return 2; }
    GFile *file = g_file_new_for_commandline_arg(argv[2]); GError *error = NULL;
    Document *d = document_load(file, NULL, NULL, &error); g_object_unref(file);
    if (!d) { g_printerr("%s\n", error->message); g_error_free(error); return 1; }
    cairo_surface_t *surface = render_page(d, page - 1, width, &error); document_unref(d);
    if (!surface) { g_printerr("%s\n", error->message); g_error_free(error); return 1; }
    cairo_status_t status = cairo_surface_write_to_png(surface, argv[3]); cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) { g_printerr("%s\n", cairo_status_to_string(status)); return 1; }
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--self-test")) {
        g_test_init(&argc, &argv, NULL);
        g_test_add_func("/zen/layout", test_layout); g_test_add_func("/zen/raster-budget", test_budget); g_test_add_func("/zen/cache", test_cache);
        return g_test_run();
    }
    if (argc > 1 && !strcmp(argv[1], "--render")) return render_cli(argc, argv);
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "--help"))) {
        g_print("zen-pdf " VERSION "\nUsage: zen-pdf [FILE.pdf …]\nA quiet, borderless PDF reader.\n\nDrag over text to select · Ctrl+C copy · Ctrl+A select this page\nCtrl+O open · Left/Right pages · Up/Down scroll · W fit width · P fit page\n+/− zoom · Ctrl+L go to page · F11 fullscreen · Ctrl+W close\n\n--render FILE OUTPUT.png WIDTH PAGE   Render a page without a display\n--self-test                          Run internal checks\n"); return 0;
    }
    g_set_prgname("zen-pdf"); g_set_application_name("Zen PDF");
    GtkApplication *application = gtk_application_new("io.github.zenpdf", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL); g_signal_connect(application, "open", G_CALLBACK(app_open), NULL);
    int result = g_application_run(G_APPLICATION(application), argc, argv); g_object_unref(application); return result;
}
