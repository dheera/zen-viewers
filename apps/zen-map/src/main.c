/* SPDX-License-Identifier: GPL-3.0-or-later */
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_6
#include "zen-ui.h"
#include "map-data.h"
#include "tiles.h"
#include <curl/curl.h>
#include <math.h>
#include <string.h>

typedef struct MapView MapView;
typedef struct { GtkWidget parent; MapView *view; } MapCanvas;
typedef struct { GtkWidgetClass parent; } MapCanvasClass;
G_DEFINE_TYPE(MapCanvas, map_canvas, GTK_TYPE_WIDGET)

struct MapView {
    ZenWindow *window;
    MapCanvas *canvas;
    GtkWidget *credit, *hint, *map_button, *zoom_label, *summary;
    MapData *map;
    ZenTiles *tiles;
    GCancellable *load;
    guint serial, tick, fit_idle;
    gboolean basemap, closed, fit_pending, warned, dragging;
    double x, y, zoom, target_x, target_y, target_zoom;
    double drag_x, drag_y, pointer_x, pointer_y;
    double pan_dx, pan_dy;
    gint64 last_frame;
};

static double world_size(MapView *v) { return 256. * exp2(v->zoom); }
static void refresh(MapView *v) {
    char label[24]; g_snprintf(label, sizeof label, "%.1f", v->zoom);
    gtk_label_set_text(GTK_LABEL(v->zoom_label), label);
    gtk_widget_queue_draw(GTK_WIDGET(v->canvas));
}
static void stop_animation(MapView *v) {
    if (v->tick) { gtk_widget_remove_tick_callback(GTK_WIDGET(v->canvas), v->tick); v->tick = 0; }
    v->target_x = v->x; v->target_y = v->y; v->target_zoom = v->zoom;
}
static gboolean animate(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)widget; MapView *v = data;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double delta = CLAMP((now - v->last_frame) / 1000000., 0., .05); v->last_frame = now;
    double amount = 1. - exp(-delta * 20.);
    v->x += (v->target_x - v->x) * amount; v->y += (v->target_y - v->y) * amount;
    v->zoom += (v->target_zoom - v->zoom) * amount;
    if (fabs(v->zoom - v->target_zoom) < .002 && hypot(v->x - v->target_x, v->y - v->target_y) * world_size(v) < .1) {
        v->x = v->target_x; v->y = v->target_y; v->zoom = v->target_zoom; v->tick = 0; refresh(v); return G_SOURCE_REMOVE;
    }
    refresh(v); return G_SOURCE_CONTINUE;
}
static void zoom_at(MapView *v, double change, double px, double py) {
    v->pan_dx = v->pan_dy = 0;
    double width = gtk_widget_get_width(GTK_WIDGET(v->canvas)), height = gtk_widget_get_height(GTK_WIDGET(v->canvas));
    double target = CLAMP(v->target_zoom + change, 1., 19.);
    double scale = world_size(v), new_scale = 256. * exp2(target);
    v->target_x = v->x + (px - width / 2) * (1. / scale - 1. / new_scale);
    v->target_y = CLAMP(v->y + (py - height / 2) * (1. / scale - 1. / new_scale), 0., 1.);
    v->target_zoom = target;
    if (!v->tick) { v->last_frame = g_get_monotonic_time(); v->tick = gtk_widget_add_tick_callback(GTK_WIDGET(v->canvas), animate, v, NULL); }
}
static void fit(MapView *v) {
    int w = gtk_widget_get_width(GTK_WIDGET(v->canvas)), h = gtk_widget_get_height(GTK_WIDGET(v->canvas));
    if (w < 2 || h < 2) { v->fit_pending = TRUE; return; }
    stop_animation(v); v->fit_pending = FALSE;
    v->pan_dx = v->pan_dy = 0;
    if (v->map) {
        v->x = (v->map->x0 + v->map->x1) / 2; v->y = (v->map->y0 + v->map->y1) / 2;
        double sx = MAX(1., w - 200.) / MAX(1e-9, v->map->x1 - v->map->x0);
        double sy = MAX(1., h - 220.) / MAX(1e-9, v->map->y1 - v->map->y0);
        v->zoom = CLAMP(log2(MIN(sx, sy) / 256.), 1., v->map->points == 1 ? 14. : 18.);
    } else { v->x = .5; v->y = .5; v->zoom = 2.; }
    v->target_zoom = v->zoom; v->target_x = v->x; v->target_y = v->y; refresh(v);
}
static gboolean fit_later(gpointer data) {
    MapView *v = data; v->fit_idle = 0;
    if (!v->closed) fit(v);
    return G_SOURCE_REMOVE;
}
static void text(cairo_t *cr, const char *string, double x, double y, int size, gboolean bold) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new(); pango_font_description_set_family(font, "sans-serif");
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_MEDIUM : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, font); pango_layout_set_text(layout, string, -1);
    cairo_move_to(cr, x, y); pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font); g_object_unref(layout);
}
static void point_label(cairo_t *cr, const char *label, double x, double y, double width) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string("Sans Medium 10");
    pango_font_description_set_absolute_size(font, 13 * PANGO_SCALE);
    pango_layout_set_font_description(layout, font); pango_layout_set_text(layout, label, -1);
    int label_width; pango_layout_get_pixel_size(layout, &label_width, NULL);
    double left = x + 11;
    if (left + label_width > width - 16) left = MAX(16., x - 11 - label_width);
    cairo_set_source_rgba(cr, 1, 1, .97, .94); cairo_move_to(cr, left + 1, y - 7); pango_cairo_show_layout(cr, layout);
    cairo_set_source_rgb(cr, .13, .22, .18); cairo_move_to(cr, left, y - 8); pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font); g_object_unref(layout);
}
static void map_paths(MapView *v, cairo_t *cr, double w, double h) {
    if (!v->map) return;
    double scale = world_size(v), origin_x = w / 2 - v->x * scale, origin_y = h / 2 - v->y * scale;
    double repeat = round(v->x - (v->map->x0 + v->map->x1) / 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (guint i = 0; i < v->map->shapes->len; i++) {
        MapShape *shape = g_ptr_array_index(v->map->shapes, i);
        if (origin_x + (shape->x1 + repeat) * scale < -120 || origin_x + (shape->x0 + repeat) * scale > w + 120 ||
            origin_y + shape->y1 * scale < -40 || origin_y + shape->y0 * scale > h + 40) continue;
        cairo_new_path(cr); double last_x = -G_MAXDOUBLE, last_y = -G_MAXDOUBLE;
        for (guint j = 0; j < shape->points->len; j++) {
            MapPoint p = g_array_index(shape->points, MapPoint, j);
            double x = origin_x + (p.x + repeat) * scale, y = origin_y + p.y * scale;
            if (p.move) { if (shape->polygon && j) cairo_close_path(cr); cairo_move_to(cr, x, y); }
            else if (j + 1 == shape->points->len || fabs(x - last_x) + fabs(y - last_y) > .7) cairo_line_to(cr, x, y);
            else continue;
            last_x = x; last_y = y;
        }
        if (shape->polygon) {
            cairo_close_path(cr); cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
            cairo_set_source_rgba(cr, .13, .40, .34, .14); cairo_fill_preserve(cr);
        }
        cairo_set_source_rgba(cr, 1., 1., .97, .88); cairo_set_line_width(cr, 6.); cairo_stroke_preserve(cr);
        cairo_set_source_rgb(cr, .14, .37, .31); cairo_set_line_width(cr, 3.); cairo_stroke(cr);
        if (shape->points->len == 1) {
            MapPoint p = g_array_index(shape->points, MapPoint, 0); double x = origin_x + (p.x + repeat) * scale, y = origin_y + p.y * scale;
            cairo_arc(cr, x, y, 6., 0, 2 * G_PI); cairo_set_source_rgb(cr, .14, .37, .31); cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 1., 1., .97); cairo_set_line_width(cr, 2.5); cairo_stroke(cr);
            if (shape->name) {
                char *label = g_utf8_substring(shape->name, 0, MIN(g_utf8_strlen(shape->name, -1), 45));
                point_label(cr, label, x, y, w); g_free(label);
            }
        }
    }
}
static void nearby_tiles(MapView *v, int z, int first_x, int first_y, int last_x, int last_y,
                         double center_x, double center_y) {
    /* Browser-like short-range lookahead: never another zoom level, never more
     * than one tile past the visible bounds, and only four candidates total. */
    struct { int x, y; double priority; } candidates[4]; guint count = 0;
    double length = hypot(v->pan_dx, v->pan_dy);
    double dir_x = length > 0 ? v->pan_dx / length : 0, dir_y = length > 0 ? v->pan_dy / length : 0;
    for (int y = MAX(0, first_y - 1); y <= MIN((1 << z) - 1, last_y + 1); y++) {
        for (int x = first_x - 1; x <= last_x + 1; x++) {
            if (x >= first_x && x <= last_x && y >= first_y && y <= last_y) continue;
            double dx = x + .5 - center_x, dy = y + .5 - center_y;
            double priority = dx * dx + dy * dy - 4. * (dx * dir_x + dy * dir_y);
            guint at = 0; while (at < count && candidates[at].priority <= priority) at++;
            if (at >= G_N_ELEMENTS(candidates)) continue;
            guint end = MIN(count, G_N_ELEMENTS(candidates) - 1);
            for (guint i = end; i > at; i--) candidates[i] = candidates[i - 1];
            candidates[at].x = x; candidates[at].y = y; candidates[at].priority = priority;
            count = MIN(count + 1, G_N_ELEMENTS(candidates));
        }
    }
    for (guint i = 0; i < count; i++) zen_tiles_lookahead(v->tiles, z, candidates[i].x, candidates[i].y, candidates[i].priority);
}
static void map_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    MapView *v = ((MapCanvas *)widget)->view; if (!v || v->closed) return;
    double w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget), scale = world_size(v);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, w, h);
    GdkRGBA bg = {.94, .94, .90, 1}; gtk_snapshot_append_color(snapshot, &bg, &bounds);
    gtk_snapshot_push_clip(snapshot, &bounds);
    zen_tiles_begin(v->tiles);
    if (v->basemap) {
        int z = CLAMP((int)floor(v->zoom + .2), 1, 19), n = 1 << z;
        double tile_size = scale / n;
        int first_x = (int)floor((v->x * scale - w / 2) / tile_size);
        int first_y = MAX(0, (int)floor((v->y * scale - h / 2) / tile_size));
        int last_x = (int)floor((v->x * scale + w / 2) / tile_size);
        int last_y = MIN(n - 1, (int)floor((v->y * scale + h / 2) / tile_size));
        for (int y = first_y; y <= last_y; y++) for (int x = first_x; x <= last_x; x++) {
            graphene_rect_t r = GRAPHENE_RECT_INIT(w / 2 + (x * tile_size - v->x * scale),
                h / 2 + (y * tile_size - v->y * scale), tile_size, tile_size);
            double dx = x + .5 - v->x * n, dy = y + .5 - v->y * n;
            zen_tiles_snapshot(v->tiles, snapshot, z, x, y, &r, dx * dx + dy * dy);
        }
        nearby_tiles(v, z, first_x, first_y, last_x, last_y, v->x * n, v->y * n);
    }
    zen_tiles_end(v->tiles, v->basemap, !v->tick && !v->dragging);
    cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &bounds);
    if (!v->basemap) {
        double step = 120., ox = fmod(w / 2 - v->x * scale, step), oy = fmod(h / 2 - v->y * scale, step);
        cairo_set_source_rgba(cr, .25, .35, .29, .07); cairo_set_line_width(cr, 1);
        for (double x = ox; x < w; x += step) { cairo_move_to(cr, x, 0); cairo_line_to(cr, x, h); }
        for (double y = oy; y < h; y += step) { cairo_move_to(cr, 0, y); cairo_line_to(cr, w, y); }
        cairo_stroke(cr);
    }
    map_paths(v, cr, w, h);
    double meters_per_pixel = 40075016.686 * cos(map_y_latitude(v->y) * G_PI / 180.) / scale;
    double rough = meters_per_pixel * 100., unit = pow(10., floor(log10(MAX(rough, .001))));
    double meters = (rough / unit >= 5 ? 5 : rough / unit >= 2 ? 2 : 1) * unit, bar = meters / meters_per_pixel;
    if (isfinite(bar) && w > 320 && h > 220) {
        double x = w - 26 - bar, y = h - 64;
        cairo_set_source_rgba(cr, 1, 1, .98, .86); cairo_rectangle(cr, x - 8, y - 26, bar + 16, 40); cairo_fill(cr);
        cairo_set_source_rgb(cr, .23, .29, .25); cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, x, y - 5); cairo_line_to(cr, x, y); cairo_line_to(cr, x + bar, y); cairo_line_to(cr, x + bar, y - 5); cairo_stroke(cr);
        char label[48]; if (meters >= 1000) g_snprintf(label, sizeof label, "%g km", meters / 1000.); else g_snprintf(label, sizeof label, "%g m", meters);
        text(cr, label, x, y - 23, 11, FALSE);
    }
    cairo_destroy(cr); gtk_snapshot_pop(snapshot);
    if (v->fit_pending && !v->fit_idle) v->fit_idle = g_idle_add(fit_later, v);
}
static void map_canvas_class_init(MapCanvasClass *klass) { GTK_WIDGET_CLASS(klass)->snapshot = map_snapshot; }
static void map_canvas_init(MapCanvas *canvas) { gtk_widget_set_hexpand(GTK_WIDGET(canvas), TRUE); gtk_widget_set_vexpand(GTK_WIDGET(canvas), TRUE); }

static void tiles_changed(gpointer data, const char *error) {
    MapView *v = data; if (v->closed) return;
    if (error && !v->warned && v->basemap) { zen_window_notice(v->window, error); v->warned = TRUE; }
    gtk_widget_queue_draw(GTK_WIDGET(v->canvas));
}
static void toggle_map(GtkToggleButton *button, gpointer data) {
    MapView *v = data; v->basemap = gtk_toggle_button_get_active(button); v->warned = FALSE;
    gtk_widget_set_visible(v->credit, v->basemap); gtk_widget_set_visible(v->hint, !v->map && !v->basemap);
    if (!v->basemap) { zen_tiles_begin(v->tiles); zen_tiles_end(v->tiles, FALSE, FALSE); zen_window_clear_notice(v->window); }
    refresh(v);
}
static void click_map(GtkButton *button, gpointer data) { (void)button; MapView *v = data; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->map_button), TRUE); }
static void click_fit(GtkButton *button, gpointer data) { (void)button; fit(data); }
static void click_plus(GtkButton *button, gpointer data) { (void)button; MapView *v = data; zoom_at(v, 1., gtk_widget_get_width(GTK_WIDGET(v->canvas)) / 2., gtk_widget_get_height(GTK_WIDGET(v->canvas)) / 2.); }
static void click_minus(GtkButton *button, gpointer data) { (void)button; MapView *v = data; zoom_at(v, -1., gtk_widget_get_width(GTK_WIDGET(v->canvas)) / 2., gtk_widget_get_height(GTK_WIDGET(v->canvas)) / 2.); }
static void motion(GtkEventControllerMotion *controller, double x, double y, gpointer data) { (void)controller; MapView *v = data; v->pointer_x = x; v->pointer_y = y; }
static gboolean scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer data) {
    MapView *v = data;
    if (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller)) & GDK_SHIFT_MASK) {
        stop_animation(v); v->pan_dx = dx + dy; v->pan_dy = 0; v->x += (dx + dy) * 60 / world_size(v); refresh(v);
    } else zoom_at(v, -CLAMP(dy, -4., 4.) * .45, v->pointer_x, v->pointer_y);
    return TRUE;
}
static void drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)x; (void)y; MapView *v = data; stop_animation(v); v->drag_x = v->x; v->drag_y = v->y; v->dragging = TRUE;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED); gtk_widget_set_cursor_from_name(GTK_WIDGET(v->canvas), "grabbing");
}
static void drag_update(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)gesture; MapView *v = data;
    double next_x = v->drag_x - x / world_size(v), next_y = CLAMP(v->drag_y - y / world_size(v), 0., 1.);
    v->pan_dx = next_x - v->x; v->pan_dy = next_y - v->y; v->x = next_x; v->y = next_y;
    v->target_x = v->x; v->target_y = v->y; refresh(v);
}
static void drag_end(GtkGestureDrag *gesture, double x, double y, gpointer data) { (void)gesture; (void)x; (void)y; MapView *v = data; v->dragging = FALSE; gtk_widget_set_cursor_from_name(GTK_WIDGET(v->canvas), "grab"); refresh(v); }
static gboolean claim(double x, double y, gpointer data) { (void)data; (void)x; return y > 58; }
static gboolean key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
    (void)controller; (void)keycode; MapView *v = data;
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return FALSE;
    switch (keyval) {
        case GDK_KEY_m: case GDK_KEY_M: gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->map_button), !v->basemap); break;
        case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add: click_plus(NULL, v); break;
        case GDK_KEY_minus: case GDK_KEY_KP_Subtract: click_minus(NULL, v); break;
        case GDK_KEY_f: case GDK_KEY_F: case GDK_KEY_Home: fit(v); break;
        case GDK_KEY_Left: stop_animation(v); v->pan_dx = -1; v->pan_dy = 0; v->x -= 100 / world_size(v); refresh(v); break;
        case GDK_KEY_Right: stop_animation(v); v->pan_dx = 1; v->pan_dy = 0; v->x += 100 / world_size(v); refresh(v); break;
        case GDK_KEY_Up: stop_animation(v); v->pan_dx = 0; v->pan_dy = -1; v->y = MAX(0., v->y - 100 / world_size(v)); refresh(v); break;
        case GDK_KEY_Down: stop_animation(v); v->pan_dx = 0; v->pan_dy = 1; v->y = MIN(1., v->y + 100 / world_size(v)); refresh(v); break;
        default: return FALSE;
    }
    return TRUE;
}

typedef struct { GFile *file; guint serial; } Load;
static void load_free(gpointer data) { Load *l = data; g_object_unref(l->file); g_free(l); }
static void load_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    (void)object; Load *l = data; GError *error = NULL; MapData *map = map_data_load(l->file, cancel, &error);
    if (map) g_task_return_pointer(task, map, (GDestroyNotify)map_data_free); else g_task_return_error(task, error);
}
static void loaded(GObject *object, GAsyncResult *result, gpointer data) {
    (void)data; MapView *v = g_object_get_data(object, "map-view"); GTask *task = G_TASK(result); Load *l = g_task_get_task_data(task);
    GError *error = NULL; MapData *map = g_task_propagate_pointer(task, &error);
    if (!v->closed && l->serial == v->serial) {
        g_clear_object(&v->load); zen_window_hide_progress(v->window);
        if (map) {
            map_data_free(v->map); v->map = map; map = NULL;
            char *name = g_file_get_basename(l->file); zen_window_set_title(v->window, name); g_free(name);
            char *status = g_strdup_printf("%u %s · %u points", v->map->shapes->len, v->map->shapes->len == 1 ? "feature" : "features", v->map->points);
            gtk_label_set_text(GTK_LABEL(v->summary), status); g_free(status);
            gtk_widget_set_visible(v->hint, FALSE); zen_window_clear_notice(v->window); fit(v);
        } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) zen_window_notice(v->window, error ? error->message : "This map could not be read.");
    }
    map_data_free(map); g_clear_error(&error);
}
static void open_map(ZenWindow *window, GFile *file, gpointer data) {
    MapView *v = data;
    if (v->load) { g_cancellable_cancel(v->load); g_clear_object(&v->load); }
    /* Opening another local file never sends its area to a tile provider implicitly. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->map_button), FALSE);
    v->load = g_cancellable_new(); v->serial++;
    zen_window_show_progress(window, -1.);
    Load *l = g_new0(Load, 1); l->file = g_object_ref(file); l->serial = v->serial;
    GTask *task = g_task_new(zen_window_get_window(window), v->load, loaded, NULL);
    g_task_set_task_data(task, l, load_free); g_task_run_in_thread(task, load_worker); g_object_unref(task);
}
static gboolean close_view(GtkWindow *window, gpointer data) {
    (void)window; MapView *v = data; v->closed = TRUE;
    if (v->load) g_cancellable_cancel(v->load);
    stop_animation(v); if (v->fit_idle) { g_source_remove(v->fit_idle); v->fit_idle = 0; }
    zen_tiles_free(v->tiles); v->tiles = NULL; return FALSE;
}
static void view_free(gpointer data) {
    MapView *v = data; if (v->tiles) zen_tiles_free(v->tiles);
    if (v->load) { g_cancellable_cancel(v->load); g_object_unref(v->load); }
    if (v->fit_idle) g_source_remove(v->fit_idle);
    map_data_free(v->map); g_free(v);
}
static void new_view(GtkApplication *application, GFile *file) {
    MapView *v = g_new0(MapView, 1); v->x = v->y = v->target_x = v->target_y = .5; v->zoom = v->target_zoom = 2.;
    v->window = zen_window_new(application, "Zen Map", 1080, 760); GtkWindow *native = zen_window_get_window(v->window);
    g_object_set_data_full(G_OBJECT(native), "map-view", v, view_free);
    g_signal_connect(native, "close-request", G_CALLBACK(close_view), v);
    v->tiles = zen_tiles_new(tiles_changed, v);
    v->canvas = g_object_new(map_canvas_get_type(), NULL); v->canvas->view = v;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(v->canvas), "grab");
    GtkWidget *overlay = gtk_overlay_new(); gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(v->canvas));
    v->hint = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20); gtk_widget_add_css_class(v->hint, "map-welcome");
    gtk_widget_set_halign(v->hint, GTK_ALIGN_CENTER); gtk_widget_set_valign(v->hint, GTK_ALIGN_CENTER);
    GtkWidget *heading = gtk_label_new("A little perspective"); gtk_widget_add_css_class(heading, "map-heading"); gtk_box_append(GTK_BOX(v->hint), heading);
    GtkWidget *intro = gtk_label_new("Drop a GPX, KML, or GeoJSON file to explore.\nLocal maps open without connecting to the internet.");
    gtk_label_set_justify(GTK_LABEL(intro), GTK_JUSTIFY_CENTER); gtk_box_append(GTK_BOX(v->hint), intro);
    GtkWidget *connect = gtk_button_new_with_label("Explore OpenStreetMap"); gtk_widget_set_halign(connect, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(connect, "Load live tiles for the visible area from OpenStreetMap"); g_signal_connect(connect, "clicked", G_CALLBACK(click_map), v);
    gtk_box_append(GTK_BOX(v->hint), connect); gtk_overlay_add_overlay(GTK_OVERLAY(overlay), v->hint);
    const char *credit = g_getenv("ZEN_MAP_TILE_ATTRIBUTION"), *credit_url = g_getenv("ZEN_MAP_TILE_CREDIT_URL");
    v->credit = gtk_link_button_new_with_label(credit_url ? credit_url : "https://www.openstreetmap.org/copyright", credit ? credit : "© OpenStreetMap contributors");
    gtk_widget_add_css_class(v->credit, "map-credit"); gtk_widget_set_halign(v->credit, GTK_ALIGN_START); gtk_widget_set_valign(v->credit, GTK_ALIGN_END);
    gtk_widget_set_margin_start(v->credit, 16); gtk_widget_set_margin_bottom(v->credit, 60); gtk_widget_set_visible(v->credit, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), v->credit);
    zen_window_set_child(v->window, overlay); zen_window_set_open_handler(v->window, open_map, v);
    zen_window_set_pointer_claim_func(v->window, claim, v);
    const char *mimes[] = {"application/gpx+xml", "application/vnd.google-earth.kml+xml", "application/geo+json", "application/json", NULL};
    const char *patterns[] = {"*.gpx", "*.GPX", "*.kml", "*.KML", "*.geojson", "*.GeoJSON", "*.json", NULL};
    zen_window_add_file_filter(v->window, "Maps and tracks", mimes, patterns);
    GtkWidget *controls = zen_window_get_controls(v->window);
    v->map_button = gtk_toggle_button_new_with_label("Map"); gtk_widget_set_tooltip_text(v->map_button, "Live basemap (M) · sends the visible area to the tile provider");
    g_signal_connect(v->map_button, "toggled", G_CALLBACK(toggle_map), v); gtk_box_append(GTK_BOX(controls), v->map_button);
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-fit-best-symbolic", "Fit map (F / Home)", G_CALLBACK(click_fit), v));
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-out-symbolic", "Zoom out (−)", G_CALLBACK(click_minus), v));
    v->zoom_label = gtk_label_new("2.0"); gtk_widget_set_size_request(v->zoom_label, 34, -1); gtk_box_append(GTK_BOX(controls), v->zoom_label);
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-in-symbolic", "Zoom in (+)", G_CALLBACK(click_plus), v));
    v->summary = gtk_label_new("Drag to pan · scroll to zoom"); gtk_widget_add_css_class(v->summary, "map-summary"); gtk_box_append(GTK_BOX(controls), v->summary);
    GtkEventController *keys = gtk_event_controller_key_new(); g_signal_connect(keys, "key-pressed", G_CALLBACK(key), v); gtk_widget_add_controller(GTK_WIDGET(native), keys);
    GtkEventController *pointer = gtk_event_controller_motion_new(); g_signal_connect(pointer, "motion", G_CALLBACK(motion), v); gtk_widget_add_controller(GTK_WIDGET(v->canvas), pointer);
    GtkGesture *drag = gtk_gesture_drag_new(); g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), v);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), v); g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), v);
    gtk_widget_add_controller(GTK_WIDGET(v->canvas), GTK_EVENT_CONTROLLER(drag));
    GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(wheel, "scroll", G_CALLBACK(scroll), v); gtk_widget_add_controller(GTK_WIDGET(v->canvas), wheel);
    zen_window_set_ready(v->window, TRUE); zen_window_present(v->window);
    if (file) open_map(v->window, file, v);
}
static void start(GtkApplication *application, gpointer data) {
    (void)application; (void)data; zen_ui_init();
    GtkCssProvider *css = gtk_css_provider_new(); gtk_css_provider_load_from_data(css,
        ".map-welcome { color: #40544a; padding: 36px; }"
        ".map-heading { font-family: serif; font-size: 34px; font-weight: 400; }"
        ".map-welcome label { margin: 0; }"
        ".map-welcome button { color: #eaf0e7; background: #365c4d; border: none; border-radius: 22px; padding: 10px 22px; box-shadow: none; }"
        ".map-credit { color: #334c3e; background: alpha(#fffff8, .9); padding: 3px 9px; border: none; border-radius: 7px; font-size: 11px; min-height: 20px; }"
        ".map-summary { font-size: 12px; padding: 0 8px; }", -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(css);
}
static void activate(GtkApplication *application, gpointer data) { (void)data; new_view(application, NULL); }
static void opened(GtkApplication *application, GFile **files, int n, const char *hint, gpointer data) {
    (void)hint; (void)data; for (int i = 0; i < n; i++) new_view(application, files[i]);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { g_print("zen-map %s\n", ZEN_VERSION); return 0; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_set_prgname("zen-map"); g_set_application_name("Zen Map");
    GtkApplication *app = gtk_application_new("io.github.zenviewers.Map", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(start), NULL); g_signal_connect(app, "activate", G_CALLBACK(activate), NULL); g_signal_connect(app, "open", G_CALLBACK(opened), NULL);
    int result = g_application_run(G_APPLICATION(app), argc, argv); g_object_unref(app);
    /* Worker cancellation can outlive the last window; libcurl global state stays valid until process exit. */
    return result;
}
