/* SPDX-License-Identifier: GPL-3.0-or-later */
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#include "zen-ui.h"
#include "renderer.h"
#include <math.h>
#include <string.h>

#define VIEW_FOV 38.
#define MAX_ZOOM 80.
#define MIN_ZOOM .055

typedef struct {
    double yaw, pitch, zoom, center[3];
    gboolean orthographic;
} Camera;

typedef struct {
    GFile *file;
    Zen3dModel *preview;
    gint percent;
    gboolean allow_preview;
} LoadRequest;

typedef struct {
    ZenWindow *shell;
    GtkGLArea *area;
    Zen3dRenderer *renderer;
    GtkWidget *info, *info_label;
    GtkWidget *grid_toggle, *wire_toggle, *info_toggle;
    GtkPopover *views;
    GCancellable *loading;
    LoadRequest *request; /* Owned by the current GTask. */
    GFile *queued, *uploading, *current;
    char *next_details, *render_error;
    guint upload_tick, motion_timeout;
    gint64 last_progress;
    gboolean ready, gl_ready, draw_failed, showing_preview;
    gboolean grid, wireframe, panning, dragging, pinching;
    Camera camera, drag_camera;
    double pinch_zoom;
} Viewer;

static void open_model(ZenWindow *shell, GFile *file, gpointer data);

static double view_height(Viewer *v) {
    return MAX(1, gtk_widget_get_height(GTK_WIDGET(v->area)));
}
static double view_distance(Viewer *v) {
    double aspect = MAX(1, gtk_widget_get_width(GTK_WIDGET(v->area))) / view_height(v);
    double half_fov = VIEW_FOV * G_PI / 360.;
    double limiting_fov = MIN(half_fov, atan(tan(half_fov) * aspect));
    return v->camera.zoom * 1.12 / sin(limiting_fov);
}
static double view_span(Viewer *v) {
    return 2. * view_distance(v) * tan(VIEW_FOV * G_PI / 360.);
}
static void redraw(Viewer *v) {
    gtk_gl_area_queue_render(v->area);
}
static gboolean finish_motion(gpointer data) {
    Viewer *v = data;
    v->motion_timeout = 0;
    redraw(v);
    return G_SOURCE_REMOVE;
}
static void keep_motion(Viewer *v) {
    const Zen3dModel *model = zen3d_renderer_get_model(v->renderer);
    if (!model || !model->preview_index_count) return;
    g_clear_handle_id(&v->motion_timeout, g_source_remove);
    v->motion_timeout = g_timeout_add(150, finish_motion, v);
}
static void frame_model(Viewer *v) {
    v->camera.zoom = 1.;
    memset(v->camera.center, 0, sizeof v->camera.center);
    redraw(v);
}
static void set_angle(Viewer *v, double yaw, double pitch) {
    v->camera.yaw = yaw;
    v->camera.pitch = pitch;
    frame_model(v);
}
static void reset_view(Viewer *v) {
    v->camera.orthographic = FALSE;
    set_angle(v, .62, .48);
}
static void zoom_to(Viewer *v, double zoom) {
    v->camera.zoom = CLAMP(zoom, MIN_ZOOM, MAX_ZOOM);
    redraw(v);
}
static void orbit_from(Viewer *v, Camera from, double dx, double dy) {
    v->camera.yaw = remainder(from.yaw - dx * .009, 2. * G_PI);
    v->camera.pitch = CLAMP(from.pitch + dy * .009, -G_PI_2, G_PI_2);
    redraw(v);
}
static void pan_from(Viewer *v, Camera from, double dx, double dy) {
    double sy = sin(from.yaw), cy = cos(from.yaw), sp = sin(from.pitch), cp = cos(from.pitch);
    const double right[3] = {cy, 0., -sy}, up[3] = {-sy * sp, cp, -cy * sp};
    double per_pixel = view_span(v) / view_height(v);
    for (int i = 0; i < 3; i++)
        v->camera.center[i] = CLAMP(from.center[i] + per_pixel * (-dx * right[i] + dy * up[i]), -10000., 10000.);
    redraw(v);
}
static Zen3dView render_view(Viewer *v) {
    Camera *c = &v->camera;
    double sy = sin(c->yaw), cy = cos(c->yaw), sp = sin(c->pitch), cp = cos(c->pitch);
    double distance = view_distance(v);
    return (Zen3dView){
        .eye = {c->center[0] + distance * sy * cp, c->center[1] + distance * sp,
                c->center[2] + distance * cy * cp},
        .center = {c->center[0], c->center[1], c->center[2]},
        .up = {-sy * sp, cp, -cy * sp},
        .orthographic = c->orthographic, .ortho_height = view_span(v), .vertical_fov = VIEW_FOV,
        .wireframe = v->wireframe, .grid = v->grid,
        .interacting = v->dragging || v->pinching || v->motion_timeout
    };
}

static char *model_details(GFile *file, const Zen3dModel *model) {
    guint64 triangles = 0, lines = 0, points = 0;
    guint textures = 0;
    for (uint32_t i = 0; i < model->part_count; i++) {
        const Zen3dPart *part = &model->parts[i];
        if (part->primitive == ZEN3D_TRIANGLES) triangles += part->index_count / 3;
        else if (part->primitive == ZEN3D_LINES) lines += part->index_count / 2;
        else points += part->index_count;
    }
    for (uint32_t i = 0; i < model->material_count; i++) textures += model->materials[i].texture != NULL;
    g_autofree char *base = g_file_get_basename(file);
    g_autofree char *name = g_filename_display_name(base);
    return g_strdup_printf("%s\n\nVertices    %u\nTriangles  %" G_GUINT64_FORMAT
        "\nLines          %" G_GUINT64_FORMAT "\nPoints        %" G_GUINT64_FORMAT
        "\nMaterials   %u\nTextures     %u\n\nSize in source units\n%.5g × %.5g × %.5g",
        name, model->vertex_count, triangles, lines, points, model->material_count, textures,
        model->dimensions[0], model->dimensions[1], model->dimensions[2]);
}
static void clear_model(Viewer *v) {
    zen3d_renderer_set_model(v->renderer, NULL);
    g_clear_object(&v->current);
    g_clear_object(&v->uploading);
    g_clear_pointer(&v->next_details, g_free);
    v->ready = v->showing_preview = FALSE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->info_toggle), FALSE);
    zen_window_set_title(v->shell, NULL);
    zen_window_set_ready(v->shell, FALSE);
    redraw(v);
}
static void show_loaded_preview(Viewer *v) {
    if (!v->request || g_cancellable_is_cancelled(v->loading)) return;
    Zen3dModel *model = g_atomic_pointer_get(&v->request->preview);
    if (!model || !g_atomic_pointer_compare_and_exchange(&v->request->preview, model, NULL)) return;
    reset_view(v);
    zen3d_renderer_set_model(v->renderer, model);
    g_autofree char *name = g_file_get_basename(v->request->file);
    zen_window_set_title(v->shell, name);
    gtk_label_set_text(GTK_LABEL(v->info_label), "Preparing the full mesh…");
    zen_window_set_ready(v->shell, TRUE);
    v->ready = v->showing_preview = TRUE;
    gtk_widget_grab_focus(GTK_WIDGET(v->area));
}
static gboolean upload_frame(GtkWidget *area, GdkFrameClock *clock, gpointer data) {
    Viewer *v = data;
    if (v->draw_failed) {
        clear_model(v);
        v->draw_failed = FALSE;
        zen_window_notice(v->shell, v->render_error);
        g_clear_pointer(&v->render_error, g_free);
    }
    show_loaded_preview(v);
    gboolean pending = v->gl_ready && zen3d_renderer_pending(v->renderer);
    if (pending) redraw(v);
    else if (v->uploading) {
        if (v->gl_ready) {
            g_set_object(&v->current, v->uploading);
            g_autofree char *name = g_file_get_basename(v->current);
            zen_window_set_title(v->shell, name);
            gtk_label_set_text(GTK_LABEL(v->info_label), v->next_details);
            zen_window_set_ready(v->shell, TRUE);
            v->ready = TRUE;
            v->showing_preview = FALSE;
            if (!gtk_widget_get_visible(v->info)) gtk_widget_grab_focus(area);
        }
        g_clear_object(&v->uploading);
        g_clear_pointer(&v->next_details, g_free);
    }
    if (v->loading || pending) {
        gint64 now = gdk_frame_clock_get_frame_time(clock);
        if (now - v->last_progress >= 65000) {
            v->last_progress = now;
            if (v->request) {
                int percent = g_atomic_int_get(&v->request->percent);
                zen_window_show_progress(v->shell, percent < 0 ? -1. : percent * .008);
            } else {
                zen_window_show_progress(v->shell, .8 + .2 * zen3d_renderer_progress(v->renderer));
            }
        }
        return G_SOURCE_CONTINUE;
    }
    zen_window_hide_progress(v->shell);
    v->upload_tick = 0;
    return G_SOURCE_REMOVE;
}
static void queue_upload(Viewer *v) {
    if (!v->upload_tick)
        v->upload_tick = gtk_widget_add_tick_callback(GTK_WIDGET(v->area), upload_frame, v, NULL);
    redraw(v);
}
static void realize(GtkGLArea *area, gpointer data) {
    Viewer *v = data;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) return;
    g_autoptr(GError) error = NULL;
    v->gl_ready = zen3d_renderer_initialize(v->renderer, &error);
    if (!v->gl_ready) {
        gtk_gl_area_set_error(area, error);
        return;
    }
    if (zen3d_renderer_needs_reload(v->renderer) && v->current && !v->loading)
        open_model(v->shell, v->current, v);
    else if (zen3d_renderer_pending(v->renderer))
        queue_upload(v);
}
static void unrealize(GtkGLArea *area, gpointer data) {
    Viewer *v = data;
    if (v->gl_ready) {
        gtk_gl_area_make_current(area);
        zen3d_renderer_release_gl(v->renderer);
    }
    v->gl_ready = FALSE;
}
static gboolean render(GtkGLArea *area, GdkGLContext *context, gpointer data) {
    (void)context;
    Viewer *v = data;
    if (!v->gl_ready || v->draw_failed) return FALSE;
    GtkWidget *widget = GTK_WIDGET(area);
    int scale = gtk_widget_get_scale_factor(widget);
    Zen3dView view = render_view(v);
    g_autoptr(GError) error = NULL;
    if (zen3d_renderer_render(v->renderer, &view, gtk_widget_get_width(widget) * scale,
                             gtk_widget_get_height(widget) * scale, &error)) return TRUE;
    v->draw_failed = TRUE;
    v->render_error = g_strdup(error ? error->message : "The model could not be drawn.");
    if (!v->upload_tick)
        v->upload_tick = gtk_widget_add_tick_callback(widget, upload_frame, v, NULL);
    return FALSE;
}

static void load_progress(unsigned percent, gpointer data) {
    LoadRequest *request = data;
    g_atomic_int_set(&request->percent, (int)percent);
}
static void load_preview(Zen3dModel *model, gpointer data) {
    LoadRequest *request = data;
    if (!g_atomic_pointer_compare_and_exchange(&request->preview, NULL, model))
        zen3d_model_free(model);
}
static void load_request_free(gpointer data) {
    LoadRequest *request = data;
    zen3d_model_free(g_atomic_pointer_get(&request->preview));
    g_object_unref(request->file);
    g_free(request);
}
static void load_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    LoadRequest *request = data;
    Zen3dLoadFeedback feedback = {
        .progress = load_progress, .preview = request->allow_preview ? load_preview : NULL,
        .data = request
    };
    GError *error = NULL;
    Zen3dModel *model = zen3d_model_load_with_feedback(request->file, cancel, &feedback, &error);
    if (model) g_task_return_pointer(task, model, (GDestroyNotify)zen3d_model_free);
    else g_task_return_error(task, error);
}
static void start_load(Viewer *v, GFile *file);
static void model_loaded(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    Viewer *v = g_object_get_data(source, "zen-3d-view");
    GTask *task = G_TASK(result);
    LoadRequest *request = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    Zen3dModel *model = g_task_propagate_pointer(task, &error);
    v->request = NULL;
    g_clear_object(&v->loading);
    if (!zen_window_is_closed(v->shell)) {
        if (v->queued) {
            if (v->showing_preview && !v->uploading) clear_model(v);
            g_autoptr(GFile) queued = g_steal_pointer(&v->queued);
            start_load(v, queued);
        } else if (model) {
            g_set_object(&v->uploading, request->file);
            g_free(v->next_details);
            v->next_details = model_details(request->file, model);
            if (!v->showing_preview) reset_view(v);
            zen3d_renderer_set_model(v->renderer, model);
            model = NULL;
            zen_window_show_progress(v->shell, .8);
            queue_upload(v);
        } else {
            if (v->showing_preview && !v->uploading) clear_model(v);
            if (!v->uploading) zen_window_hide_progress(v->shell);
            if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                zen_window_notice(v->shell, error->message);
        }
    }
    zen3d_model_free(model);
}
static void start_load(Viewer *v, GFile *file) {
    v->loading = g_cancellable_new();
    zen_window_clear_notice(v->shell);
    zen_window_show_progress(v->shell, -1.);
    v->last_progress = 0;
    LoadRequest *request = g_new0(LoadRequest, 1);
    request->file = g_object_ref(file);
    request->percent = -1;
    request->allow_preview = !v->current && !v->uploading && !v->showing_preview;
    v->request = request;
    GTask *task = g_task_new(zen_window_get_window(v->shell), v->loading, model_loaded, NULL);
    g_task_set_task_data(task, request, load_request_free);
    g_task_run_in_thread(task, load_thread);
    g_object_unref(task);
    if (!v->upload_tick)
        v->upload_tick = gtk_widget_add_tick_callback(GTK_WIDGET(v->area), upload_frame, v, NULL);
}
static void open_model(ZenWindow *shell, GFile *file, gpointer data) {
    Viewer *v = data;
    const GError *gl_error = gtk_gl_area_get_error(v->area);
    if (gl_error) {
        zen_window_notice(shell, gl_error->message);
        return;
    }
    if (v->loading) {
        g_set_object(&v->queued, file);
        g_cancellable_cancel(v->loading);
    } else start_load(v, file);
}

static void drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)x;
    (void)y;
    Viewer *v = data;
    if (!v->ready) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    v->panning = button == GDK_BUTTON_MIDDLE || button == GDK_BUTTON_SECONDARY ||
                 (state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK));
    v->dragging = TRUE;
    v->drag_camera = v->camera;
    gtk_widget_grab_focus(GTK_WIDGET(v->area));
    gtk_widget_set_cursor_from_name(GTK_WIDGET(v->area), "grabbing");
}
static void drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
    (void)gesture;
    Viewer *v = data;
    if (!v->ready) return;
    if (v->panning) pan_from(v, v->drag_camera, dx, dy);
    else orbit_from(v, v->drag_camera, dx, dy);
}
static void drag_end(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)gesture;
    (void)x;
    (void)y;
    Viewer *v = data;
    gtk_widget_set_cursor(GTK_WIDGET(v->area), NULL);
    if (v->dragging) {
        v->dragging = FALSE;
        redraw(v);
    }
}
static void drag_cancel(GtkGesture *gesture, GdkEventSequence *sequence, gpointer data) {
    (void)sequence;
    drag_end(GTK_GESTURE_DRAG(gesture), 0., 0., data);
}
static void pinch_begin(GtkGesture *gesture, GdkEventSequence *sequence, gpointer data) {
    (void)gesture;
    (void)sequence;
    Viewer *v = data;
    v->pinching = v->ready;
    v->pinch_zoom = v->camera.zoom;
}
static void pinch_end(GtkGesture *gesture, GdkEventSequence *sequence, gpointer data) {
    (void)gesture;
    (void)sequence;
    Viewer *v = data;
    if (v->pinching) {
        v->pinching = FALSE;
        redraw(v);
    }
}
static void pinch_changed(GtkGestureZoom *gesture, double scale, gpointer data) {
    (void)gesture;
    Viewer *v = data;
    if (v->ready && scale > 0.) zoom_to(v, v->pinch_zoom / scale);
}
static gboolean scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer data) {
    (void)dx;
    Viewer *v = data;
    if (!v->ready || dy == 0.) return FALSE;
    double amount = dy;
#if GTK_CHECK_VERSION(4, 8, 0)
    if (gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_SURFACE)
        amount /= 45.;
#else
    (void)controller;
#endif
    keep_motion(v);
    zoom_to(v, v->camera.zoom * exp(CLAMP(amount, -4., 4.) * .16));
    return TRUE;
}
static gboolean content_press(double x, double y, gpointer data) {
    (void)x;
    Viewer *v = data;
    return v->ready && y >= 43.;
}

static void fit_clicked(GtkButton *button, gpointer data) {
    (void)button;
    frame_model(data);
}
static void grid_toggled(GtkToggleButton *button, gpointer data) {
    Viewer *v = data;
    v->grid = gtk_toggle_button_get_active(button);
    redraw(v);
}
static void wire_toggled(GtkToggleButton *button, gpointer data) {
    Viewer *v = data;
    v->wireframe = gtk_toggle_button_get_active(button);
    redraw(v);
}
static void info_toggled(GtkToggleButton *button, gpointer data) {
    Viewer *v = data;
    gboolean show = gtk_toggle_button_get_active(button);
    gtk_widget_set_visible(v->info, show);
    if (!show) gtk_widget_grab_focus(GTK_WIDGET(v->area));
}
static void info_close(GtkButton *button, gpointer data) {
    (void)button;
    Viewer *v = data;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->info_toggle), FALSE);
}
static void toggle(GtkWidget *button) {
    GtkToggleButton *state = GTK_TOGGLE_BUTTON(button);
    gtk_toggle_button_set_active(state, !gtk_toggle_button_get_active(state));
}

typedef enum { VIEW_RESET, VIEW_FRONT, VIEW_SIDE, VIEW_TOP, VIEW_PROJECT } ViewAction;
static void view_action(Viewer *v, ViewAction action) {
    switch (action) {
    case VIEW_RESET: reset_view(v); break;
    case VIEW_FRONT: set_angle(v, 0., 0.); break;
    case VIEW_SIDE: set_angle(v, G_PI_2, 0.); break;
    case VIEW_TOP: set_angle(v, 0., G_PI_2); break;
    case VIEW_PROJECT: v->camera.orthographic = !v->camera.orthographic; redraw(v); break;
    }
}
static void view_clicked(GtkButton *button, gpointer data) {
    Viewer *v = data;
    ViewAction action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "zen-view-action"));
    view_action(v, action);
    gtk_popover_popdown(v->views);
    gtk_widget_grab_focus(GTK_WIDGET(v->area));
}
static gboolean key_pressed(GtkEventControllerKey *controller, guint key, guint code,
                            GdkModifierType state, gpointer data) {
    (void)controller;
    (void)code;
    Viewer *v = data;
    if (!v->ready || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) return FALSE;
    switch (gdk_keyval_to_lower(key)) {
    case GDK_KEY_f: case GDK_KEY_Home: frame_model(v); return TRUE;
    case GDK_KEY_r: view_action(v, VIEW_RESET); return TRUE;
    case GDK_KEY_1: view_action(v, VIEW_FRONT); return TRUE;
    case GDK_KEY_2: view_action(v, VIEW_SIDE); return TRUE;
    case GDK_KEY_3: view_action(v, VIEW_TOP); return TRUE;
    case GDK_KEY_o: view_action(v, VIEW_PROJECT); return TRUE;
    case GDK_KEY_g: toggle(v->grid_toggle); return TRUE;
    case GDK_KEY_w: toggle(v->wire_toggle); return TRUE;
    case GDK_KEY_i: toggle(v->info_toggle); return TRUE;
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
        keep_motion(v); zoom_to(v, v->camera.zoom / 1.18); return TRUE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        keep_motion(v); zoom_to(v, v->camera.zoom * 1.18); return TRUE;
    case GDK_KEY_Escape:
        if (!gtk_widget_get_visible(v->info)) return FALSE;
        info_close(NULL, v);
        return TRUE;
    case GDK_KEY_Left: case GDK_KEY_Right: case GDK_KEY_Up: case GDK_KEY_Down: {
        double dx = key == GDK_KEY_Left ? -14. : key == GDK_KEY_Right ? 14. : 0.;
        double dy = key == GDK_KEY_Up ? -14. : key == GDK_KEY_Down ? 14. : 0.;
        keep_motion(v);
        if (state & GDK_SHIFT_MASK) pan_from(v, v->camera, dx, dy);
        else orbit_from(v, v->camera, dx, dy);
        return TRUE;
    }
    default: return FALSE;
    }
}
static gboolean close_request(GtkWindow *window, gpointer data) {
    (void)window;
    Viewer *v = data;
    if (v->loading) g_cancellable_cancel(v->loading);
    g_clear_handle_id(&v->motion_timeout, g_source_remove);
    g_clear_object(&v->queued);
    return FALSE;
}
static void viewer_free(gpointer data) {
    Viewer *v = data;
    g_clear_handle_id(&v->motion_timeout, g_source_remove);
    g_clear_object(&v->loading);
    g_clear_object(&v->queued);
    g_clear_object(&v->uploading);
    g_clear_object(&v->current);
    g_free(v->next_details);
    g_free(v->render_error);
    zen3d_renderer_free(v->renderer);
    g_free(v);
}

static GtkWidget *toggle_button(const char *label, const char *name, GCallback callback, Viewer *v) {
    GtkWidget *button = gtk_toggle_button_new_with_label(label);
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_widget_set_tooltip_text(button, name);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_signal_connect(button, "toggled", callback, v);
    return button;
}
static GtkWidget *view_menu(Viewer *v) {
    static const char *const names[] = {"Reset view  ·  R", "Front  ·  1", "Right  ·  2",
                                       "Top  ·  3", "Perspective / orthographic  ·  O"};
    GtkWidget *menu = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(menu), "View");
    gtk_widget_set_tooltip_text(menu, "View presets and navigation");
    v->views = GTK_POPOVER(gtk_popover_new());
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), GTK_WIDGET(v->views));
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(list, "zen-3d-view-menu");
    gtk_popover_set_child(v->views, list);
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        GtkWidget *button = gtk_button_new_with_label(names[i]);
        gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
        g_object_set_data(G_OBJECT(button), "zen-view-action", GINT_TO_POINTER(i));
        g_signal_connect(button, "clicked", G_CALLBACK(view_clicked), v);
        gtk_box_append(GTK_BOX(list), button);
    }
    GtkWidget *hint = gtk_label_new("Drag to orbit  ·  Shift+drag to pan\nScroll or pinch to zoom");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
    gtk_widget_set_margin_top(hint, 8);
    gtk_box_append(GTK_BOX(list), hint);
    return menu;
}
static void make_overlays(Viewer *v, GtkOverlay *canvas) {
    v->info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(v->info, "zen-3d-details");
    gtk_widget_set_halign(v->info, GTK_ALIGN_END);
    gtk_widget_set_margin_end(v->info, 14);
    gtk_widget_set_margin_top(v->info, 70);
    gtk_widget_set_margin_bottom(v->info, 66);
    gtk_widget_set_size_request(v->info, 242, -1);
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title = gtk_label_new("Model details");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_add_css_class(title, "heading");
    gtk_box_append(GTK_BOX(heading), title);
    gtk_box_append(GTK_BOX(heading), zen_button_new("window-close-symbolic", "Hide details (I)",
                                                   G_CALLBACK(info_close), v));
    gtk_box_append(GTK_BOX(v->info), heading);
    GtkWidget *scroll_area = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_area), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll_area, TRUE);
    v->info_label = gtk_label_new(NULL);
    gtk_widget_set_valign(v->info_label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(v->info_label), 0.);
    gtk_label_set_wrap(GTK_LABEL(v->info_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(v->info_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(v->info_label), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_area), v->info_label);
    gtk_box_append(GTK_BOX(v->info), scroll_area);
    gtk_overlay_add_overlay(canvas, v->info);
    gtk_widget_set_visible(v->info, FALSE);
}
static void make_controls(Viewer *v) {
    GtkBox *controls = GTK_BOX(zen_window_get_controls(v->shell));
    gtk_box_append(controls, zen_button_new("zoom-fit-best-symbolic", "Frame model (F)", G_CALLBACK(fit_clicked), v));
    gtk_box_append(controls, view_menu(v));
    v->wire_toggle = toggle_button("Wire", "Wireframe (W)", G_CALLBACK(wire_toggled), v);
    gtk_box_append(controls, v->wire_toggle);
    v->grid_toggle = toggle_button("Grid", "Ground grid (G)", G_CALLBACK(grid_toggled), v);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->grid_toggle), TRUE);
    gtk_box_append(controls, v->grid_toggle);
    v->info_toggle = toggle_button("Info", "Model details (I)", G_CALLBACK(info_toggled), v);
    gtk_box_append(controls, v->info_toggle);
}
static Viewer *new_viewer(GtkApplication *app) {
    Viewer *v = g_new0(Viewer, 1);
    v->renderer = zen3d_renderer_new();
    v->shell = zen_window_new(app, "Zen 3D", 960, 740);
    gtk_widget_add_css_class(zen_window_get_root(v->shell), "zen-3d");
    GtkWindow *window = zen_window_get_window(v->shell);
    g_object_set_data_full(G_OBJECT(window), "zen-3d-view", v, viewer_free);
    g_signal_connect(window, "close-request", G_CALLBACK(close_request), v);
    zen_window_set_empty_text(v->shell, "Open a 3D model", "Drop an OBJ, GLB, STL, PLY or other 3D file here.");
    zen_window_set_open_handler(v->shell, open_model, v);
    zen_window_set_pointer_claim_func(v->shell, content_press, v);
    g_autofree char *formats = zen3d_model_formats();
    g_auto(GStrv) patterns = g_strsplit(formats, ";", -1);
    const char *all[] = {"*", NULL};
    zen_window_add_file_filter(v->shell, "3D models", NULL, (const char *const *)patterns);
    zen_window_add_file_filter(v->shell, "All files", NULL, all);

    v->area = GTK_GL_AREA(gtk_gl_area_new());
    GtkWidget *area = GTK_WIDGET(v->area);
    gtk_widget_set_focusable(area, TRUE);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_gl_area_set_required_version(v->area, 3, 3);
    gtk_gl_area_set_has_depth_buffer(v->area, TRUE);
    /* GTK can rotate its backing textures between snapshots. Paint each one
     * it requests so chrome and focus changes always show the current model. */
    g_signal_connect(area, "realize", G_CALLBACK(realize), v);
    g_signal_connect(area, "unrealize", G_CALLBACK(unrealize), v);
    g_signal_connect(area, "render", G_CALLBACK(render), v);

    GtkWidget *canvas = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(canvas), area);
    make_overlays(v, GTK_OVERLAY(canvas));
    zen_window_set_child(v->shell, canvas);
    make_controls(v);
    reset_view(v);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), v);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), v);
    g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), v);
    g_signal_connect(drag, "cancel", G_CALLBACK(drag_cancel), v);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));
    GtkGesture *pinch = gtk_gesture_zoom_new();
    g_signal_connect(pinch, "begin", G_CALLBACK(pinch_begin), v);
    g_signal_connect(pinch, "end", G_CALLBACK(pinch_end), v);
    g_signal_connect(pinch, "cancel", G_CALLBACK(pinch_end), v);
    g_signal_connect(pinch, "scale-changed", G_CALLBACK(pinch_changed), v);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(pinch));
    GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(wheel, "scroll", G_CALLBACK(scroll), v);
    gtk_widget_add_controller(area, wheel);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), v);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);
    zen_window_present(v->shell);
    return v;
}
static void activate(GtkApplication *application, gpointer data) {
    (void)data;
    new_viewer(application);
}
static void files_open(GtkApplication *application, GFile **files, int count, const char *hint, gpointer data) {
    (void)hint;
    (void)data;
    for (int i = 0; i < count; i++) {
        Viewer *v = new_viewer(application);
        open_model(v->shell, files[i], v);
    }
}
static void startup(GApplication *app, gpointer data) {
    (void)app;
    (void)data;
    zen_ui_init();
    GtkCssProvider *style = gtk_css_provider_new();
    gtk_css_provider_load_from_data(style,
        ".zen-3d .empty { color:#293a36; }"
        ".zen-3d .empty .muted { color:#41564e; }"
        ".zen-3d-details { background:rgba(25,33,31,0.96); color:#ecf3ee; border-radius:14px;"
        " border:1px solid rgba(210,230,218,0.23); padding:12px 14px; }"
        ".zen-3d-details button { background:transparent; color:#ecf3ee; box-shadow:none; }"
        ".zen-3d-view-menu { padding:5px 8px; }", -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(style);
}
static int inspect(const char *path) {
    g_autoptr(GFile) file = g_file_new_for_commandline_arg(path);
    g_autoptr(GError) error = NULL;
    Zen3dModel *model = zen3d_model_load(file, NULL, &error);
    if (!model) {
        g_printerr("%s\n", error->message);
        return 1;
    }
    g_autofree char *details = model_details(file, model);
    g_print("%s\n", details);
    zen3d_model_free(model);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        g_print("zen-3d " ZEN_VERSION "\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        g_print("Usage: zen-3d [MODEL …]\n"
                "Drag to orbit · Shift+drag to pan · Scroll or pinch to zoom\n"
                "F frame · R reset · 1/2/3 front/right/top · O projection\n"
                "W wireframe · G grid · I details · Ctrl+O open · F11 fullscreen\n\n"
                "--formats       List model extensions\n"
                "--inspect FILE  Read model statistics without opening a window\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--formats")) {
        g_autofree char *formats = zen3d_model_formats();
        g_auto(GStrv) globs = g_strsplit(formats, ";", -1);
        for (guint i = 0; globs[i]; i++) if (*globs[i]) g_print("%s\n", globs[i]);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--inspect")) {
        if (argc == 3) return inspect(argv[2]);
        g_printerr("Usage: zen-3d --inspect FILE\n");
        return 2;
    }
    g_set_prgname("zen-3d");
    g_set_application_name("Zen 3D");
    g_autoptr(GtkApplication) app = gtk_application_new("io.github.zenviewers.ThreeD",
        G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(files_open), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
