/* SPDX-License-Identifier: GPL-3.0-or-later
 * Text positions are Unicode character indices in Poppler's reading order.
 * The rectangle array is in the rotated page's top-left coordinate system.
 */
#define TEXT_GROUP_SIZE 64
#define MAX_LAYOUT_CHARS 500000

struct PageText {
    char *utf8;
    int length;
    guint *offset;                /* Character boundary -> UTF-8 byte offset. */
    PangoLogAttr *attrs;
    PopplerRectangle *box;
    PopplerRectangle *group;      /* Skip runs outside the pointer or viewport. */
    int groups;
};
typedef struct { Document *doc; int page; } TextJob;

static void page_text_free(PageText *p) {
    if (!p) return;
    g_free(p->utf8); g_free(p->offset); g_free(p->attrs);
    g_free(p->box); g_free(p->group); g_free(p);
}
static gboolean glyph_rect(PopplerRectangle r) {
    return r.x2 > r.x1 && r.y2 > r.y1 && isfinite(r.x1) && isfinite(r.y1) &&
           isfinite(r.x2) && isfinite(r.y2);
}
static PageText *read_page_text(Document *doc, int index) {
    PageText *p = g_new0(PageText, 1); guint boxes = 0;
    g_mutex_lock(&doc->pdf_lock);
    PopplerPage *page = poppler_document_get_page(doc->pdf, index);
    if (page) {
        p->utf8 = poppler_page_get_text(page);
        if (p->utf8 && g_utf8_validate(p->utf8, -1, NULL) && strlen(p->utf8) <= G_MAXINT) {
            p->length = (int)g_utf8_strlen(p->utf8, -1);
            if (p->length && p->length <= MAX_LAYOUT_CHARS)
                poppler_page_get_text_layout(page, &p->box, &boxes);
        }
        g_object_unref(page);
    }
    g_mutex_unlock(&doc->pdf_lock);
    /* A malformed mapping must never index outside the string. Whole-page copy
     * is still available for this case and for unusually large text layers. */
    if (!p->box || boxes != (guint)p->length) {
        g_clear_pointer(&p->box, g_free); return p;
    }
    p->offset = g_new(guint, p->length + 1);
    p->attrs = g_new0(PangoLogAttr, p->length + 1);
    pango_get_log_attrs(p->utf8, -1, -1, pango_language_get_default(), p->attrs, p->length + 1);
    p->groups = (p->length + TEXT_GROUP_SIZE - 1) / TEXT_GROUP_SIZE;
    p->group = g_new(PopplerRectangle, p->groups);
    const char *ch = p->utf8;
    for (int i = 0; i < p->length; i++, ch = g_utf8_next_char(ch)) {
        p->offset[i] = (guint)(ch - p->utf8);
        PopplerRectangle *g = &p->group[i / TEXT_GROUP_SIZE];
        if (i % TEXT_GROUP_SIZE == 0)
            *g = (PopplerRectangle){G_MAXDOUBLE,G_MAXDOUBLE,-G_MAXDOUBLE,-G_MAXDOUBLE};
        PopplerRectangle r = p->box[i];
        if (!glyph_rect(r)) continue;  /* In particular, newlines have no area. */
        g->x1 = MIN(g->x1, r.x1); g->y1 = MIN(g->y1, r.y1);
        g->x2 = MAX(g->x2, r.x2); g->y2 = MAX(g->y2, r.y2);
    }
    p->offset[p->length] = (guint)(ch - p->utf8);
    return p;
}

static void selection_schedule(App *a);
static void selection_all(App *a);
static void selection_copy(App *a);

static void text_job_free(gpointer data) {
    TextJob *job = data; document_unref(job->doc); g_free(job);
}
static void text_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; (void)cancel; TextJob *job = data;
    g_task_return_pointer(task, read_page_text(job->doc, job->page), (GDestroyNotify)page_text_free);
}
static void text_done(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; App *a = data; GTask *task = G_TASK(result);
    TextJob *job = g_task_get_task_data(task);
    PageText *text = g_task_propagate_pointer(task, NULL); a->selection.loading = FALSE;
    if (!a->closed && job->doc == a->doc && job->page == a->page) {
        a->selection.text = text; text = NULL;
        if (a->selection.want_all) selection_all(a);
        if (a->selection.want_copy) selection_copy(a);
    }
    page_text_free(text);
    if (!a->closed && a->doc && cached(a, a->page, 0, FALSE)) selection_schedule(a);
    g_application_release(G_APPLICATION(a->application));
}
static gboolean start_text(gpointer data) {
    App *a = data; a->selection.load_idle = 0;
    if (a->closed || !a->doc || a->selection.loading || a->selection.text) return G_SOURCE_REMOVE;
    TextJob *job = g_new0(TextJob, 1);
    job->doc = document_ref(a->doc); job->page = a->page;
    /* The window reference held by the task keeps its App alive for text_done. */
    GTask *task = g_task_new(a->window, NULL, text_done, a);
    g_task_set_task_data(task, job, text_job_free);
    a->selection.loading = TRUE;
    g_application_hold(G_APPLICATION(a->application));
    g_task_run_in_thread(task, text_worker); g_object_unref(task);
    return G_SOURCE_REMOVE;
}
static void selection_schedule(App *a) {
    Selection *s = &a->selection;
    if (a->doc && !s->text && !s->loading && !s->load_idle)
        s->load_idle = g_idle_add(start_text, a);
}
static void selection_stop_sources(App *a) {
    Selection *s = &a->selection;
    if (s->scroll_timer) { g_source_remove(s->scroll_timer); s->scroll_timer = 0; }
    if (s->load_idle) { g_source_remove(s->load_idle); s->load_idle = 0; }
}
static void selection_clear(App *a) {
    Selection *s = &a->selection;
    s->start = s->end = 0; s->dragging = s->want_all = s->want_copy = FALSE;
    if (s->scroll_timer) { g_source_remove(s->scroll_timer); s->scroll_timer = 0; }
    gtk_widget_queue_draw(a->canvas);
}
static void selection_reset(App *a) {
    selection_clear(a); selection_stop_sources(a);
    g_clear_pointer(&a->selection.text, page_text_free);
}

static double text_distance(PopplerRectangle r, double x, double y) {
    double dx = MAX(0, MAX(r.x1 - x, x - r.x2));
    double dy = MAX(0, MAX(r.y1 - y, y - r.y2));
    return dx * dx + dy * dy;
}
static int selection_hit(App *a, double x, double y, gboolean nearest) {
    PageText *p = a->selection.text;
    if (!p || !p->box) return -1;
    Layout l = current_layout(a);
    if (l.scale <= 0) return -1;
    x = (x - l.x) / l.scale; y = (y - l.y) / l.scale;
    double best = nearest ? G_MAXDOUBLE : 0; int which = -1;
    for (int g = 0; g < p->groups; g++) {
        if (!glyph_rect(p->group[g]) || text_distance(p->group[g], x, y) > best) continue;
        int end = MIN(p->length, (g + 1) * TEXT_GROUP_SIZE);
        for (int i = g * TEXT_GROUP_SIZE; i < end; i++) {
            PopplerRectangle r = p->box[i];
            if (!glyph_rect(r)) continue;
            double distance = text_distance(r, x, y);
            if (distance == 0) return i;
            if (distance < best) { best = distance; which = i; }
        }
    }
    return which;
}
/* Expand one glyph to the requested semantic unit. Returned range is [lo,hi). */
static void text_unit(PageText *p, int hit, int clicks, int *lo, int *hi) {
    *lo = hit; *hi = hit + 1;
    if (clicks == 1) {
        /* Select complete grapheme clusters, including combining marks. */
        while (*lo > 0 && !p->attrs[*lo].is_cursor_position) (*lo)--;
        while (*hi < p->length && !p->attrs[*hi].is_cursor_position) (*hi)++;
    } else if (clicks == 2) {
        while (*lo > 0 && !p->attrs[*lo].is_word_boundary) (*lo)--;
        while (*hi < p->length && !p->attrs[*hi].is_word_boundary) (*hi)++;
    } else if (clicks >= 3) {
        while (*lo > 0 && p->utf8[p->offset[*lo - 1]] != '\n') (*lo)--;
        while (*hi < p->length && p->utf8[p->offset[*hi]] != '\n') (*hi)++;
    }
}
static void write_selection(App *a, GdkClipboard *clipboard) {
    Selection *s = &a->selection; PageText *p = s->text;
    if (!clipboard || !p || s->end <= s->start) return;
    if (s->start == 0 && s->end == p->length) {
        gdk_clipboard_set_text(clipboard, p->utf8);
    } else if (p->offset) {
        char *slice = g_strndup(p->utf8 + p->offset[s->start], p->offset[s->end] - p->offset[s->start]);
        gdk_clipboard_set_text(clipboard, slice); g_free(slice);
    }
}
static void set_primary(App *a) {
    write_selection(a, gdk_display_get_primary_clipboard(gtk_widget_get_display(a->canvas)));
}
static void selection_all(App *a) {
    Selection *s = &a->selection;
    if (!a->doc) return;
    if (!s->text) { s->want_all = TRUE; selection_schedule(a); return; }
    s->want_all = FALSE; s->start = 0; s->end = s->text->length;
    set_primary(a); gtk_widget_queue_draw(a->canvas);
}
static void selection_copy(App *a) {
    Selection *s = &a->selection;
    if (s->want_all && !s->text) { s->want_copy = TRUE; return; }
    s->want_copy = FALSE;
    if (s->start < s->end) write_selection(a, gtk_widget_get_clipboard(a->canvas));
    else if (s->text && s->text->length == 0) notice(a, "This page has no selectable text.");
    else notice(a, "Select some text to copy.");
}

static gboolean force_pan(GtkGestureSingle *gesture) {
    return gtk_gesture_single_get_current_button(gesture) == 2 ||
        (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)) & GDK_CONTROL_MASK);
}
static gboolean selection_press(App *a, int n, double x, double y) {
    int hit = selection_hit(a, x, y, FALSE);
    if (hit < 0) return FALSE;
    Selection *s = &a->selection;
    selection_clear(a);
    s->press_x = x; s->press_y = y; s->dragging = TRUE; s->clicks = n;
    text_unit(s->text, hit, n, &s->anchor_start, &s->anchor_end);
    s->start = s->end = s->anchor_start;
    if (n > 1) { s->end = s->anchor_end; set_primary(a); }
    gtk_widget_queue_draw(a->canvas); return TRUE;
}
static void extend_selection(App *a) {
    Selection *s = &a->selection;
    int hit = selection_hit(a, s->pointer_x, s->pointer_y, TRUE);
    if (hit < 0) return;
    int lo, hi; text_unit(s->text, hit, s->clicks, &lo, &hi);
    s->start = MIN(s->anchor_start, lo); s->end = MAX(s->anchor_end, hi);
    gtk_widget_queue_draw(a->canvas);
}
static int scroll_speed(double at, int dimension) {
    const double margin = 18;
    if (at < margin) return -(int)MIN(36, ceil((margin-at)/2));
    if (at > dimension-margin) return (int)MIN(36, ceil((at-dimension+margin)/2));
    return 0;
}
static gboolean selection_scroll(gpointer data) {
    App *a = data; Selection *s = &a->selection;
    double x = a->offset_x, y = a->offset;
    a->offset_x -= scroll_speed(s->pointer_x, a->width);
    a->offset += scroll_speed(s->pointer_y, a->height);
    clamp_offsets(a);
    if (!s->dragging || (x == a->offset_x && y == a->offset)) {
        s->scroll_timer = 0; return G_SOURCE_REMOVE;
    }
    extend_selection(a); return G_SOURCE_CONTINUE;
}
static void selection_drag_to(App *a, double x, double y) {
    Selection *s = &a->selection; s->pointer_x = x; s->pointer_y = y;
    if (fabs(x - s->press_x) + fabs(y - s->press_y) < 3) return;
    extend_selection(a);
    if (!s->scroll_timer && (scroll_speed(x,a->width) || scroll_speed(y,a->height)))
        s->scroll_timer = g_timeout_add(30, selection_scroll, a);
}
static void selection_release(App *a) {
    Selection *s = &a->selection; s->dragging = FALSE;
    if (s->scroll_timer) { g_source_remove(s->scroll_timer); s->scroll_timer = 0; }
    set_primary(a);
}
static void selection_draw(App *a, cairo_t *cr, Layout l) {
    Selection *s = &a->selection; PageText *p = s->text;
    if (s->start >= s->end || !p || !p->box) return;
    cairo_save(cr);
    cairo_rectangle(cr, l.x, l.y, l.w, l.h); cairo_clip(cr);
    cairo_translate(cr, l.x, l.y); cairo_scale(cr, l.scale, l.scale);
    double x1, y1, x2, y2; cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
    for (int group = s->start/TEXT_GROUP_SIZE; group <= (s->end-1)/TEXT_GROUP_SIZE; group++) {
        PopplerRectangle bounds = p->group[group];
        if (bounds.x2<x1 || bounds.x1>x2 || bounds.y2<y1 || bounds.y1>y2) continue;
        int end = MIN(s->end, (group+1)*TEXT_GROUP_SIZE);
        for (int i = MAX(s->start, group*TEXT_GROUP_SIZE); i < end; i++) {
            PopplerRectangle r = p->box[i];
            if (glyph_rect(r)) cairo_rectangle(cr, r.x1, r.y1, r.x2-r.x1, r.y2-r.y1);
        }
    }
    cairo_set_source_rgba(cr, .18, .48, .91, .32); cairo_fill(cr); cairo_restore(cr);
}
