/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zen-ui.h"
#include <math.h>
#include <string.h>

typedef enum { FIT_IMAGE, FIT_WIDTH, ACTUAL_PIXELS } ImageFit;

typedef struct {
    GtkWidget parent_instance;
    GdkTexture *texture;
    int image_width, image_height, turns;
    ImageFit fit;
    double zoom, pan_x, pan_y, drag_x, drag_y, pointer_x, pointer_y;
    gboolean hover;
} ImageCanvas;

typedef struct { GtkWidgetClass parent_class; } ImageCanvasClass;
G_DEFINE_TYPE(ImageCanvas, image_canvas, GTK_TYPE_WIDGET)

static double image_scale(ImageCanvas *self) {
    double w = gtk_widget_get_width(GTK_WIDGET(self)), h = gtk_widget_get_height(GTK_WIDGET(self));
    if (!self->texture || w < 1 || h < 1) return 1;
    double iw = self->turns % 2 ? self->image_height : self->image_width;
    double ih = self->turns % 2 ? self->image_width : self->image_height;
    double base = w / iw;
    if (self->fit == FIT_IMAGE) base = MIN(base, h / ih);
    if (self->fit == ACTUAL_PIXELS) base = 1. / gtk_widget_get_scale_factor(GTK_WIDGET(self));
    return base * self->zoom;
}

static void pan_limits(ImageCanvas *self, double *x, double *y) {
    double scale = image_scale(self);
    double iw = self->turns % 2 ? self->image_height : self->image_width;
    double ih = self->turns % 2 ? self->image_width : self->image_height;
    double dx = MAX(0., (iw * scale - gtk_widget_get_width(GTK_WIDGET(self))) / 2);
    double dy = MAX(0., (ih * scale - gtk_widget_get_height(GTK_WIDGET(self))) / 2);
    *x = CLAMP(*x, -dx, dx); *y = CLAMP(*y, -dy, dy);
}

static void image_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    ImageCanvas *self = (ImageCanvas *)widget;
    float w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    const GdkRGBA background = {.085, .095, .095, 1};
    graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, w, h);
    gtk_snapshot_append_color(snapshot, &background, &viewport);
    if (!self->texture) return;

    double scale = image_scale(self), dx = self->pan_x, dy = self->pan_y;
    pan_limits(self, &dx, &dy);
    float iw = self->image_width * scale, ih = self->image_height * scale;
    graphene_rect_t image = GRAPHENE_RECT_INIT(-iw / 2, -ih / 2, iw, ih);
    graphene_point_t center = GRAPHENE_POINT_INIT(w / 2 + dx, h / 2 + dy);
    gtk_snapshot_push_clip(snapshot, &viewport);
    gtk_snapshot_translate(snapshot, &center);
    gtk_snapshot_rotate(snapshot, self->turns * 90.f);
    gtk_snapshot_append_texture(snapshot, self->texture, &image);
    gtk_snapshot_pop(snapshot);
}

static void image_canvas_dispose(GObject *obj) {
    ImageCanvas *self = (ImageCanvas *)obj;
    g_clear_object(&self->texture);
    G_OBJECT_CLASS(image_canvas_parent_class)->dispose(obj);
}

static void image_canvas_class_init(ImageCanvasClass *klass) {
    GTK_WIDGET_CLASS(klass)->snapshot = image_snapshot;
    G_OBJECT_CLASS(klass)->dispose = image_canvas_dispose;
}

static void image_canvas_init(ImageCanvas *self) {
    self->zoom = 1;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

static void set_fit(ImageCanvas *self, ImageFit fit) {
    self->fit = fit; self->zoom = 1;
    self->pan_x = self->pan_y = 0;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void zoom_image(ImageCanvas *self, double factor, gboolean at_pointer) {
    if (!self->texture) return;
    double old = self->zoom;
    self->zoom = CLAMP(old * factor, .05, 32.);
    double ratio = self->zoom / old;
    double dx = 0., dy = 0.;
    if (at_pointer && self->hover) {
        dx = self->pointer_x - gtk_widget_get_width(GTK_WIDGET(self)) / 2.;
        dy = self->pointer_y - gtk_widget_get_height(GTK_WIDGET(self)) / 2.;
    }
    self->pan_x = dx + (self->pan_x - dx) * ratio;
    self->pan_y = dy + (self->pan_y - dy) * ratio;
    pan_limits(self, &self->pan_x, &self->pan_y);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void rotate_image(ImageCanvas *self) {
    self->turns = (self->turns + 1) % 4;
    self->pan_x = self->pan_y = 0.;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void drag_start(GtkGestureDrag *gesture, double x, double y, gpointer user) {
    (void)gesture; (void)x; (void)y;
    ImageCanvas *self = user;
    pan_limits(self, &self->pan_x, &self->pan_y);
    self->drag_x = self->pan_x; self->drag_y = self->pan_y;
}

static void drag_update(GtkGestureDrag *gesture, double x, double y, gpointer user) {
    (void)gesture;
    ImageCanvas *self = user;
    self->pan_x = self->drag_x + x; self->pan_y = self->drag_y + y;
    pan_limits(self, &self->pan_x, &self->pan_y);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void motion(GtkEventControllerMotion *control, double x, double y, gpointer user) {
    (void)control; ImageCanvas *self = user;
    self->hover = TRUE; self->pointer_x = x; self->pointer_y = y;
}

static void leave(GtkEventControllerMotion *control, gpointer user) {
    (void)control; ((ImageCanvas *)user)->hover = FALSE;
}

static gboolean scroll(GtkEventControllerScroll *control, double dx, double dy, gpointer user) {
    ImageCanvas *self = user;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(control));
    if (state & GDK_CONTROL_MASK) zoom_image(self, exp(-dy * .14), TRUE);
    else {
        self->pan_x -= (state & GDK_SHIFT_MASK ? dy : dx) * 45;
        if (!(state & GDK_SHIFT_MASK)) self->pan_y -= dy * 45;
        pan_limits(self, &self->pan_x, &self->pan_y);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
    return TRUE;
}

/* The worker owns this decoded buffer until the main thread creates a texture. */
typedef struct { GdkPixbuf *pixels; int width, height; } Decoded;

static void decoded_free(gpointer data) {
    Decoded *result = data; g_clear_object(&result->pixels); g_free(result);
}

static void image_size(GdkPixbufLoader *loader, int width, int height, gpointer user) {
    Decoded *result = user; result->width = width; result->height = height;
    double ratio = MIN(1., MIN(16384. / MAX(width, height), sqrt((32. * 1024 * 1024) / ((double)width * height))));
    if (ratio < 1.) gdk_pixbuf_loader_set_size(loader, MAX(1, (int)(width * ratio)), MAX(1, (int)(height * ratio)));
}

static void decode(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    GFile *file = data;
    GError *error = NULL;
    GFileInputStream *in = g_file_read(file, cancel, &error);
    if (!in) { g_task_return_error(task, error); return; }
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    Decoded *result = g_new0(Decoded, 1);
    g_signal_connect(loader, "size-prepared", G_CALLBACK(image_size), result);
    guchar buffer[65536];
    gsize total = 0;
    for (;;) {
        gssize n = g_input_stream_read(G_INPUT_STREAM(in), buffer, sizeof(buffer), cancel, &error);
        if (n <= 0) break;
        total += (gsize)n;
        if (total > 128u * 1024 * 1024) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This image exceeds the 128 MiB file limit.");
            break;
        }
        if (!gdk_pixbuf_loader_write(loader, buffer, (gsize)n, &error)) break;
    }
    g_object_unref(in);
    gdk_pixbuf_loader_close(loader, error ? NULL : &error);
    if (!error) {
        GdkPixbuf *pix = gdk_pixbuf_loader_get_pixbuf(loader);
        if (pix) {
            const char *orientation = gdk_pixbuf_get_option(pix, "orientation");
            if (orientation && orientation[0] >= '5' && orientation[0] <= '8') {
                int swap = result->width; result->width = result->height; result->height = swap;
            }
            result->pixels = gdk_pixbuf_apply_embedded_orientation(pix);
        }
        if (!result->pixels || result->width <= 0 || result->height <= 0)
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The image contains no readable pixels.");
    }
    g_object_unref(loader);
    if (error) { decoded_free(result); g_task_return_error(task, error); }
    else g_task_return_pointer(task, result, decoded_free);
}

typedef struct {
    ZenWindow *window;
    ImageCanvas *canvas;
    GtkWidget *dimensions;
    GCancellable *loading;
} ImageView;

static void decoded(GObject *source, GAsyncResult *async_result, gpointer unused) {
    (void)unused;
    ImageView *view = g_object_get_data(source, "zen-image-view");
    GTask *task = G_TASK(async_result);
    GError *error = NULL;
    Decoded *result = g_task_propagate_pointer(task, &error);
    if (view->loading != g_task_get_cancellable(task) || zen_window_is_closed(view->window)) {
        if (result) decoded_free(result);
        g_clear_error(&error);
        return;
    }
    g_clear_object(&view->loading);
    if (!result) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            zen_window_notice(view->window, error->message);
        g_clear_error(&error);
        return;
    }
    ImageCanvas *canvas = view->canvas;
    /* This wraps the decoded pixel buffer. GSK retains the same texture across frames. */
    GdkPixbuf *pixels = result->pixels;
    GBytes *bytes = g_bytes_new_with_free_func(gdk_pixbuf_read_pixels(pixels),
        gdk_pixbuf_get_byte_length(pixels), g_object_unref, g_object_ref(pixels));
    GdkMemoryFormat format = gdk_pixbuf_get_has_alpha(pixels) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
    GdkTexture *texture = gdk_memory_texture_new(gdk_pixbuf_get_width(pixels), gdk_pixbuf_get_height(pixels),
                                                 format, bytes, gdk_pixbuf_get_rowstride(pixels));
    g_bytes_unref(bytes);
    g_clear_object(&canvas->texture); canvas->texture = texture;
    canvas->image_width = result->width; canvas->image_height = result->height;
    canvas->turns = 0; set_fit(canvas, FIT_IMAGE);
    char *dimensions = g_strdup_printf("%d × %d", result->width, result->height);
    gtk_label_set_text(GTK_LABEL(view->dimensions), dimensions); g_free(dimensions);
    char *name = g_file_get_basename(g_task_get_task_data(task));
    zen_window_set_title(view->window, name); g_free(name);
    zen_window_clear_notice(view->window); zen_window_set_ready(view->window, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(canvas)); decoded_free(result);
}

static void open_image(ZenWindow *window, GFile *file, gpointer user) {
    ImageView *view = user;
    if (view->loading) g_cancellable_cancel(view->loading);
    g_clear_object(&view->loading); view->loading = g_cancellable_new();
    zen_window_notice(window, "Opening image…");
    GTask *task = g_task_new(zen_window_get_window(window), view->loading, decoded, NULL);
    g_task_set_task_data(task, g_object_ref(file), g_object_unref);
    g_task_run_in_thread(task, decode);
    g_object_unref(task);
}

static void release_view(gpointer user) {
    ImageView *view = user;
    g_clear_object(&view->loading);
    g_free(view);
}

static gboolean closing(GtkWindow *window, gpointer user) {
    (void)window;
    ImageView *view = user;
    if (view->loading) g_cancellable_cancel(view->loading);
    return FALSE;
}

static gboolean image_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer user) {
    (void)controller; (void)code;
    ImageCanvas *canvas = user;
    if (!canvas->texture || (state & (GDK_ALT_MASK | GDK_SUPER_MASK))) return FALSE;
    switch (key) {
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add: zoom_image(canvas, 1.25, FALSE); return TRUE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract: zoom_image(canvas, .8, FALSE); return TRUE;
    case GDK_KEY_w: if (state & GDK_CONTROL_MASK) return FALSE; set_fit(canvas, FIT_WIDTH); return TRUE;
    case GDK_KEY_p: case GDK_KEY_0: set_fit(canvas, FIT_IMAGE); return TRUE;
    case GDK_KEY_1: set_fit(canvas, ACTUAL_PIXELS); return TRUE;
    case GDK_KEY_r: rotate_image(canvas); return TRUE;
    default: return FALSE;
    }
}

static void click_fit(GtkButton *b, gpointer data) { (void)b; set_fit(data, FIT_IMAGE); }
static void click_actual(GtkButton *b, gpointer data) { (void)b; set_fit(data, ACTUAL_PIXELS); }
static void click_in(GtkButton *b, gpointer data) { (void)b; zoom_image(data, 1.25, FALSE); }
static void click_out(GtkButton *b, gpointer data) { (void)b; zoom_image(data, .8, FALSE); }
static void click_rotate(GtkButton *b, gpointer data) { (void)b; rotate_image(data); }

static void new_view(GtkApplication *application, GFile *file) {
    ImageView *view = g_new0(ImageView, 1);
    ZenWindow *window = zen_window_new(application, "Zen Image", 790, 620);
    view->window = window;
    ImageCanvas *canvas = g_object_new(image_canvas_get_type(), NULL);
    view->canvas = canvas;
    GtkWindow *native = zen_window_get_window(window);
    g_object_set_data_full(G_OBJECT(native), "zen-image-view", view, release_view);
    g_signal_connect(native, "close-request", G_CALLBACK(closing), view);

    zen_window_set_child(window, GTK_WIDGET(canvas));
    zen_window_set_open_handler(window, open_image, view);
    zen_window_set_empty_text(window, "Zen Image", "Drop an image here, or open one to view it.");
    const char *const mimes[] = {"image/*", NULL};
    zen_window_add_file_filter(window, "Images", mimes, NULL);

    GtkWidget *controls = zen_window_get_controls(window);
    view->dimensions = gtk_label_new(NULL);
    gtk_box_append(GTK_BOX(controls), view->dimensions);
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-out-symbolic", "Zoom out (−)", G_CALLBACK(click_out), canvas));
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-in-symbolic", "Zoom in (+)", G_CALLBACK(click_in), canvas));
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-fit-best-symbolic", "Fit image (P)", G_CALLBACK(click_fit), canvas));
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-original-symbolic", "Actual pixels (1)", G_CALLBACK(click_actual), canvas));
    gtk_box_append(GTK_BOX(controls), zen_button_new("object-rotate-right-symbolic", "Rotate clockwise (R)", G_CALLBACK(click_rotate), canvas));

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(image_key), canvas);
    gtk_widget_add_controller(GTK_WIDGET(native), keys);
    GtkEventController *pointer = gtk_event_controller_motion_new();
    g_signal_connect(pointer, "motion", G_CALLBACK(motion), canvas);
    g_signal_connect(pointer, "leave", G_CALLBACK(leave), canvas);
    gtk_widget_add_controller(GTK_WIDGET(canvas), pointer);
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_start), canvas);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), canvas);
    gtk_widget_add_controller(GTK_WIDGET(canvas), GTK_EVENT_CONTROLLER(drag));
    GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    g_signal_connect(wheel, "scroll", G_CALLBACK(scroll), canvas);
    gtk_widget_add_controller(GTK_WIDGET(canvas), wheel);

    zen_window_set_ready(window, FALSE);
    zen_window_present(window);
    if (file) open_image(window, file, view);
}

static void start(GtkApplication *application, gpointer data) { (void)application; (void)data; zen_ui_init(); }
static void activate(GtkApplication *application, gpointer data) { (void)data; new_view(application, NULL); }
static void opened(GtkApplication *application, GFile **files, int n, const char *hint, gpointer data) {
    (void)hint; (void)data;
    for (int i = 0; i < n; i++) new_view(application, files[i]);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { g_print("zen-image 0.2.0\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        g_print("Usage: zen-image [IMAGE …]\nP fit · W fit width · 1 actual size · +/− zoom · R rotate · drag to pan\n");
        return 0;
    }
    g_set_prgname("zen-image"); g_set_application_name("Zen Image");
    GtkApplication *app = gtk_application_new("io.github.zenimage", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(start), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(opened), NULL);
    int code = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return code;
}
