/* SPDX-License-Identifier: GPL-3.0-or-later */
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_6
#include "zen-ui.h"

struct ZenWindow {
    GtkWindow *window;
    GtkWidget *root, *child, *top_left, *top_center, *top_right, *top_fade;
    GtkWidget *bottom_fade, *controls, *title, *empty, *empty_title, *empty_subtitle, *status;
    GPtrArray *filters;
    GtkWidget *cursor_widget;
    GdkCursor *saved_cursor;
    char *app_name;
    ZenOpenFunc open;
    gpointer open_data;
    ZenPointerClaimFunc claims_pointer;
    gpointer pointer_data;
    guint hide_timer, notice_timer;
    guint hover_zones;
    gboolean closed, peek, ready;
};

static const char *style =
    "window.zen { background: transparent; box-shadow: none; }"
    ".zen-root { background: #171b19; border-radius: 16px; }"
    "window.maximized .zen-root, window.fullscreen .zen-root, window.zen-system-corners .zen-root, window.zen-tiled .zen-root { border-radius: 0; }"
    ".zen-top-fade { background-image: linear-gradient(to bottom, rgba(12,16,15,0.9), rgba(12,16,15,0.61) 43%, rgba(12,16,15,0)); }"
    ".zen-bottom-fade { background-image: linear-gradient(to top, rgba(12,16,15,0.92), rgba(12,16,15,0.65) 45%, rgba(12,16,15,0)); }"
    ".chrome { color: #f4f6f1; }"
    ".chrome.zen-edge-controls { opacity: 0; transition: opacity 160ms ease-out; }"
    ".chrome.zen-edge-controls.zen-shown { opacity: 1; }"
    ".chrome button { background: transparent; color: #f4f6f1; border: 0; box-shadow: none; min-width: 32px; min-height: 32px; padding: 0 3px; border-radius: 99px; }"
    ".chrome button:hover, .chrome button:checked { background: alpha(white, 0.16); }"
    ".chrome button:active { background: alpha(white, 0.24); }"
    ".chrome button:disabled { opacity: 0.38; }"
    ".chrome button:focus-visible, .chrome entry:focus { outline: 2px solid #c4decd; outline-offset: -2px; }"
    ".chrome .close:hover { background: rgba(231,155,143,0.32); }"
    ".chrome entry { color: #f4f6f1; background: alpha(white, 0.12); border: 0; box-shadow: none; border-radius: 10px; min-height: 26px; padding: 2px 5px; }"
    ".doc-name { color: #f4f6f1; font-size: 12px; font-weight: 500; padding: 0 6px; }"
    ".muted { color: #b0bdb5; font-size: 12px; }"
    ".chrome .muted { color: #d1d9d1; }"
    ".empty { color: #eef1e9; }"
    ".empty-title { font-size: 30px; font-weight: 300; letter-spacing: 1px; }"
    ".empty button { color: #18281f; background: #bdd3c4; border: 0; border-radius: 24px; padding: 12px 26px; font-weight: 600; }"
    ".toast { color: #f1f2ed; background: alpha(#1b211e, 0.96); border: 1px solid alpha(white, 0.1); padding: 12px 20px; border-radius: 14px; }";

void zen_ui_init(void) {
    GdkDisplay *display = gdk_display_get_default();
    if (!display || g_object_get_data(G_OBJECT(display), "zen-ui-style")) return;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, style, -1);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_object_set_data(G_OBJECT(display), "zen-ui-style", GINT_TO_POINTER(1));
}
GtkWidget *zen_button_new(const char *icon, const char *tip, GCallback clicked, gpointer data) {
    GtkWidget *button = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_widget_set_tooltip_text(button, tip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, tip, -1);
    if (clicked) g_signal_connect(button, "clicked", clicked, data);
    return button;
}
GtkWindow *zen_window_get_window(ZenWindow *z) { return z->window; }
GtkWidget *zen_window_get_root(ZenWindow *z) { return z->root; }
GtkWidget *zen_window_get_controls(ZenWindow *z) { return z->controls; }
gboolean zen_window_is_closed(ZenWindow *z) { return z->closed; }

enum { CHROME_TOP = 1, CHROME_BOTTOM = 2 };

static gboolean contains_widget(GtkWidget *container, GtkWidget *widget) {
    return widget && (widget == container || gtk_widget_is_ancestor(widget, container));
}
static guint chrome_zone_at(ZenWindow *z, GtkWidget *widget) {
    if (contains_widget(z->top_left, widget) || contains_widget(z->top_right, widget)) return CHROME_TOP;
    if (contains_widget(z->controls, widget)) return CHROME_BOTTOM;
    return 0;
}
static void fade_controls(GtkWidget *widget, gboolean shown) {
    if (shown) gtk_widget_add_css_class(widget, "zen-shown");
    else gtk_widget_remove_css_class(widget, "zen-shown");
}
static void update_chrome(ZenWindow *z) {
    if (z->closed) return;
    guint zones = z->hover_zones | chrome_zone_at(z, gtk_window_get_focus(z->window));
    if (z->peek) zones |= CHROME_TOP | CHROME_BOTTOM;
    gboolean top = (zones & CHROME_TOP) != 0;
    gboolean bottom = z->ready && (zones & CHROME_BOTTOM) != 0;
    fade_controls(z->top_left, top);
    fade_controls(z->top_right, top);
    fade_controls(z->controls, bottom);
    gtk_revealer_set_reveal_child(GTK_REVEALER(z->top_center), top);
    gtk_revealer_set_reveal_child(GTK_REVEALER(z->top_fade), top);
    gtk_revealer_set_reveal_child(GTK_REVEALER(z->bottom_fade), bottom);
}
static void focus_changed(GObject *window, GParamSpec *param, gpointer data) {
    (void)window; (void)param; update_chrome(data);
}
static gboolean hide_controls(gpointer data) {
    ZenWindow *z = data; z->hide_timer = 0; z->peek = FALSE; update_chrome(z);
    return G_SOURCE_REMOVE;
}
void zen_window_reveal_controls(ZenWindow *z) {
    if (z->closed) return;
    z->peek = TRUE; update_chrome(z);
    if (z->hide_timer) g_source_remove(z->hide_timer);
    z->hide_timer = g_timeout_add(1800, hide_controls, z);
}
static void restore_cursor(ZenWindow *z) {
    if (z->cursor_widget) gtk_widget_set_cursor(z->cursor_widget, z->saved_cursor);
    g_clear_object(&z->cursor_widget); g_clear_object(&z->saved_cursor);
}
static const char *edge_cursor(int edge) {
    switch (edge) {
    case GDK_SURFACE_EDGE_NORTH_WEST: return "nw-resize";
    case GDK_SURFACE_EDGE_NORTH_EAST: return "ne-resize";
    case GDK_SURFACE_EDGE_SOUTH_WEST: return "sw-resize";
    case GDK_SURFACE_EDGE_SOUTH_EAST: return "se-resize";
    case GDK_SURFACE_EDGE_WEST: return "w-resize";
    case GDK_SURFACE_EDGE_EAST: return "e-resize";
    case GDK_SURFACE_EDGE_NORTH: return "n-resize";
    case GDK_SURFACE_EDGE_SOUTH: return "s-resize";
    default: return "default";
    }
}
static void root_motion(GtkEventControllerMotion *controller, double x, double y, gpointer data) {
    (void)controller; ZenWindow *z = data;
    double height = gtk_widget_get_height(z->root);
    double depth = MIN(92., height * .24);
    guint zone = y >= 0 && y < depth ? CHROME_TOP :
        y >= height - depth && y < height ? CHROME_BOTTOM : 0;
    if (zone != z->hover_zones) { z->hover_zones = zone; update_chrome(z); }
    int edge = zen_window_edge_at(z, x, y);
    GtkWidget *target = edge < 0 ? NULL : gtk_widget_pick(z->root, x, y, GTK_PICK_DEFAULT);
    if (target != z->cursor_widget) {
        restore_cursor(z);
        if (target) {
            z->cursor_widget = g_object_ref(target);
            GdkCursor *cursor = gtk_widget_get_cursor(target);
            if (cursor) z->saved_cursor = g_object_ref(cursor);
        }
    }
    if (target) gtk_widget_set_cursor_from_name(target, edge_cursor(edge));
}
static void root_leave(GtkEventControllerMotion *controller, gpointer data) {
    (void)controller; ZenWindow *z = data; z->hover_zones = 0; restore_cursor(z); update_chrome(z);
}
static void add_overlay(ZenWindow *z, GtkWidget *widget, GtkAlign horizontal,
                        GtkAlign vertical, int inset) {
    gtk_widget_set_halign(widget, horizontal); gtk_widget_set_valign(widget, vertical);
    if (vertical == GTK_ALIGN_START) gtk_widget_set_margin_top(widget, inset);
    else gtk_widget_set_margin_bottom(widget, inset);
    if (inset) {
        gtk_widget_set_margin_start(widget, 10); gtk_widget_set_margin_end(widget, 10);
    }
    gtk_overlay_add_overlay(GTK_OVERLAY(z->root), widget);
}
static GtkWidget *make_controls(ZenWindow *z, GtkAlign horizontal, GtkAlign vertical, int inset, int spacing) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
    gtk_widget_add_css_class(box, "chrome");
    /* Keep pointer targets in place during fades so the first click reaches a button. */
    gtk_widget_add_css_class(box, "zen-edge-controls");
    add_overlay(z, box, horizontal, vertical, inset);
    return box;
}
static GtkWidget *make_revealer(ZenWindow *z, GtkWidget *child, GtkAlign horizontal,
                                GtkAlign vertical, int inset) {
    GtkWidget *r = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(r), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(r), 160);
    gtk_revealer_set_child(GTK_REVEALER(r), child);
    gtk_widget_set_can_target(r, FALSE);
    add_overlay(z, r, horizontal, vertical, inset);
    return r;
}
static GtkWidget *make_fade(ZenWindow *z, const char *class_name, GtkAlign vertical) {
    GtkWidget *shade = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(shade, class_name);
    gtk_widget_set_size_request(shade, -1, 100);
    return make_revealer(z, shade, GTK_ALIGN_FILL, vertical, 0);
}
static gboolean notice_timeout(gpointer data) {
    ZenWindow *z = data; z->notice_timer = 0;
    if (!z->closed) gtk_widget_set_visible(z->status, FALSE);
    return G_SOURCE_REMOVE;
}
void zen_window_clear_notice(ZenWindow *z) {
    if (z->notice_timer) { g_source_remove(z->notice_timer); z->notice_timer = 0; }
    if (!z->closed) gtk_widget_set_visible(z->status, FALSE);
}
void zen_window_notice(ZenWindow *z, const char *message) {
    if (z->closed) return;
    zen_window_clear_notice(z);
    if (!message || !*message) return;
    gtk_label_set_text(GTK_LABEL(z->status), message);
    gtk_widget_set_visible(z->status, TRUE);
    z->notice_timer = g_timeout_add_seconds(6, notice_timeout, z);
}
void zen_window_set_title(ZenWindow *z, const char *filename) {
    char *display_name = filename ? g_filename_display_name(filename) : g_strdup(z->app_name);
    gtk_label_set_text(GTK_LABEL(z->title), display_name);
    gtk_widget_set_tooltip_text(z->title, display_name);
    char *title = filename ? g_strdup_printf("%s — %s", display_name, z->app_name) : g_strdup(z->app_name);
    gtk_window_set_title(z->window, title); g_free(title); g_free(display_name);
}
void zen_window_set_empty_text(ZenWindow *z, const char *title, const char *subtitle) {
    gtk_label_set_text(GTK_LABEL(z->empty_title), title);
    gtk_label_set_text(GTK_LABEL(z->empty_subtitle), subtitle);
}
void zen_window_set_ready(ZenWindow *z, gboolean ready) {
    z->ready = ready;
    gtk_widget_set_visible(z->empty, !ready);
    gtk_widget_set_visible(z->controls, ready);
    gtk_widget_set_visible(z->bottom_fade, ready);
    update_chrome(z);
}
void zen_window_set_child(ZenWindow *z, GtkWidget *child) {
    z->child = child; gtk_overlay_set_child(GTK_OVERLAY(z->root), child);
}
void zen_window_set_open_handler(ZenWindow *z, ZenOpenFunc callback, gpointer data) { z->open = callback; z->open_data = data; }
void zen_window_set_pointer_claim_func(ZenWindow *z, ZenPointerClaimFunc callback, gpointer data) { z->claims_pointer = callback; z->pointer_data = data; }
void zen_window_add_file_filter(ZenWindow *z, const char *name, const char *const *mimes, const char *const *patterns) {
    GtkFileFilter *filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, name);
    if (mimes) for (int i = 0; mimes[i]; i++) gtk_file_filter_add_mime_type(filter, mimes[i]);
    if (patterns) for (int i = 0; patterns[i]; i++) gtk_file_filter_add_pattern(filter, patterns[i]);
    g_ptr_array_add(z->filters, filter);
}
static void chooser_response(GtkNativeDialog *dialog, int response, gpointer data) {
    ZenWindow *z = data;
    if (!z->closed && response == GTK_RESPONSE_ACCEPT && z->open) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        if (file) { z->open(z, file, z->open_data); g_object_unref(file); }
    }
    gtk_native_dialog_destroy(dialog); g_object_unref(dialog); g_object_unref(z->window);
}
void zen_window_choose_file(ZenWindow *z) {
    if (z->closed) return;
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new("Open file", z->window, GTK_FILE_CHOOSER_ACTION_OPEN, "Open", "Cancel");
    for (guint i = 0; i < z->filters->len; i++) gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), g_ptr_array_index(z->filters, i));
    if (!z->filters->len) {
        GtkFileFilter *filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, "All files");
        gtk_file_filter_add_pattern(filter, "*"); gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter); g_object_unref(filter);
    }
    g_object_ref(z->window);
    g_signal_connect(dialog, "response", G_CALLBACK(chooser_response), z);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}
void zen_window_toggle_fullscreen(ZenWindow *z) {
    if (gtk_window_is_fullscreen(z->window)) gtk_window_unfullscreen(z->window);
    else gtk_window_fullscreen(z->window);
}
static void open_clicked(GtkButton *button, gpointer data) { (void)button; zen_window_choose_file(data); }
static void full_clicked(GtkButton *button, gpointer data) { (void)button; zen_window_toggle_fullscreen(data); }
static void close_clicked(GtkButton *button, gpointer data) { (void)button; gtk_window_close(((ZenWindow *)data)->window); }
static gboolean key_pressed(GtkEventControllerKey *controller, guint key, guint keycode, GdkModifierType state, gpointer data) {
    (void)controller; (void)keycode; ZenWindow *z = data;
    if (state & GDK_CONTROL_MASK) {
        if (key == GDK_KEY_o || key == GDK_KEY_O) { zen_window_choose_file(z); return TRUE; }
        if (key == GDK_KEY_w || key == GDK_KEY_W || key == GDK_KEY_q || key == GDK_KEY_Q) { gtk_window_close(z->window); return TRUE; }
    }
    if (key == GDK_KEY_F11) { zen_window_toggle_fullscreen(z); return TRUE; }
    if (key == GDK_KEY_Escape && gtk_window_is_fullscreen(z->window)) { gtk_window_unfullscreen(z->window); zen_window_reveal_controls(z); return TRUE; }
    return FALSE;
}
static gboolean keyboard_peek(GtkEventControllerKey *controller, guint key, guint keycode,
                              GdkModifierType state, gpointer data) {
    (void)controller; (void)keycode; (void)state;
    if (key == GDK_KEY_Tab || key == GDK_KEY_ISO_Left_Tab) zen_window_reveal_controls(data);
    return FALSE;
}
int zen_window_edge_at(ZenWindow *z, double x, double y) {
    if (gtk_window_is_fullscreen(z->window) || gtk_window_is_maximized(z->window)) return -1;
    int w = gtk_widget_get_width(z->root), h = gtk_widget_get_height(z->root);
    if (w <= 14 || h <= 14) return -1;
    gboolean l = x < 7, r = x >= w - 7, t = y < 7, b = y >= h - 7;
    if (t && l) return GDK_SURFACE_EDGE_NORTH_WEST;
    if (t && r) return GDK_SURFACE_EDGE_NORTH_EAST;
    if (b && l) return GDK_SURFACE_EDGE_SOUTH_WEST;
    if (b && r) return GDK_SURFACE_EDGE_SOUTH_EAST;
    if (l) return GDK_SURFACE_EDGE_WEST;
    if (r) return GDK_SURFACE_EDGE_EAST;
    if (t) return GDK_SURFACE_EDGE_NORTH;
    if (b) return GDK_SURFACE_EDGE_SOUTH;
    return -1;
}
static void pressed(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
    ZenWindow *z = data;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(z->window));
    if (!event || !GDK_IS_TOPLEVEL(surface)) return;
    GdkDevice *device = gdk_event_get_device(event);
    int edge = zen_window_edge_at(z, x, y);
    if (edge >= 0) {
        gdk_toplevel_begin_resize(GDK_TOPLEVEL(surface), edge, device, 1, x, y, gdk_event_get_time(event));
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED); return;
    }
    GtkWidget *target = gtk_widget_pick(z->root, x, y, GTK_PICK_DEFAULT);
    if (chrome_zone_at(z, target)) return;
    if (y < 72 && !gtk_window_is_fullscreen(z->window)) {
        if (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)) & GDK_CONTROL_MASK) return;
        if (z->claims_pointer && z->claims_pointer(x, y, z->pointer_data)) return;
        if (n == 2) {
            if (gtk_window_is_maximized(z->window)) gtk_window_unmaximize(z->window);
            else gtk_window_maximize(z->window);
        } else gdk_toplevel_begin_move(GDK_TOPLEVEL(surface), device, 1, x, y, gdk_event_get_time(event));
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }
}
static gboolean dropped(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data) {
    (void)target; (void)x; (void)y; ZenWindow *z = data;
    if (z->closed || !z->open) return FALSE;
    GFile *file = NULL;
    if (G_VALUE_HOLDS(value, G_TYPE_FILE)) file = g_value_get_object(value);
    else if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
        if (files) file = files->data;
    }
    if (!file) return FALSE;
    z->open(z, file, z->open_data); return TRUE;
}
static gboolean close_request(GtkWindow *window, gpointer data) {
    (void)window; ZenWindow *z = data; restore_cursor(z); z->closed = TRUE;
    if (z->hide_timer) { g_source_remove(z->hide_timer); z->hide_timer = 0; }
    if (z->notice_timer) { g_source_remove(z->notice_timer); z->notice_timer = 0; }
    return FALSE;
}
static void free_window(gpointer data) {
    ZenWindow *z = data;
    if (z->hide_timer) g_source_remove(z->hide_timer);
    if (z->notice_timer) g_source_remove(z->notice_timer);
    g_clear_object(&z->cursor_widget); g_clear_object(&z->saved_cursor);
    g_ptr_array_unref(z->filters); g_free(z->app_name); g_free(z);
}
static gboolean in_niri(void) {
    const char *socket = g_getenv("NIRI_SOCKET");
    if (socket && *socket) return TRUE;
    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    if (!desktop) return FALSE;
    g_auto(GStrv) names = g_strsplit(desktop, ":", -1);
    for (guint i = 0; names[i]; i++)
        if (g_ascii_strcasecmp(g_strstrip(names[i]), "niri") == 0) return TRUE;
    return FALSE;
}
static void tiled_state_changed(GObject *surface, GParamSpec *param, GtkWidget *window) {
    (void)param;
    GdkToplevelState mask = GDK_TOPLEVEL_STATE_TILED | GDK_TOPLEVEL_STATE_TOP_TILED |
        GDK_TOPLEVEL_STATE_RIGHT_TILED | GDK_TOPLEVEL_STATE_BOTTOM_TILED | GDK_TOPLEVEL_STATE_LEFT_TILED;
    if (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & mask) gtk_widget_add_css_class(window, "zen-tiled");
    else gtk_widget_remove_css_class(window, "zen-tiled");
}
static void watch_tiling(GtkWidget *window, gpointer data) {
    (void)data;
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (GDK_IS_TOPLEVEL(surface)) {
        g_signal_connect_object(surface, "notify::state", G_CALLBACK(tiled_state_changed), window, 0);
        tiled_state_changed(G_OBJECT(surface), NULL, window);
    }
}
static void configure_corners(GtkWidget *window) {
    const char *mode = g_getenv("ZEN_CORNERS");
    gboolean rounded = g_strcmp0(mode, "rounded") == 0;
    if (g_strcmp0(mode, "system") == 0 || (!rounded && in_niri()))
        gtk_widget_add_css_class(window, "zen-system-corners");
    else if (!rounded)
        g_signal_connect(window, "realize", G_CALLBACK(watch_tiling), NULL);
}
ZenWindow *zen_window_new(GtkApplication *application, const char *app_name, int width, int height) {
    zen_ui_init();
    ZenWindow *z = g_new0(ZenWindow, 1); z->app_name = g_strdup(app_name);
    z->filters = g_ptr_array_new_with_free_func(g_object_unref);
    z->window = GTK_WINDOW(gtk_application_window_new(application));
    g_object_set_data_full(G_OBJECT(z->window), "zen-window", z, free_window);
    gtk_widget_add_css_class(GTK_WIDGET(z->window), "zen"); gtk_window_set_decorated(z->window, FALSE);
    configure_corners(GTK_WIDGET(z->window));
    gtk_window_set_default_size(z->window, width, height); gtk_widget_set_size_request(GTK_WIDGET(z->window), 340, 260);
    g_signal_connect(z->window, "close-request", G_CALLBACK(close_request), z);
    z->root = gtk_overlay_new(); gtk_widget_add_css_class(z->root, "zen-root"); gtk_widget_set_overflow(z->root, GTK_OVERFLOW_HIDDEN);
    gtk_window_set_child(z->window, z->root);
    z->empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(z->empty, "empty"); gtk_widget_set_halign(z->empty, GTK_ALIGN_CENTER); gtk_widget_set_valign(z->empty, GTK_ALIGN_CENTER);
    z->empty_title = gtk_label_new("Open a file"); gtk_widget_add_css_class(z->empty_title, "empty-title"); gtk_box_append(GTK_BOX(z->empty), z->empty_title);
    z->empty_subtitle = gtk_label_new("Drop a file here, or choose one to begin."); gtk_widget_add_css_class(z->empty_subtitle, "muted");
    gtk_label_set_wrap(GTK_LABEL(z->empty_subtitle), TRUE); gtk_label_set_justify(GTK_LABEL(z->empty_subtitle), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(z->empty), z->empty_subtitle);
    GtkWidget *open = gtk_button_new_with_label("Open file"); gtk_widget_set_halign(open, GTK_ALIGN_CENTER);
    g_signal_connect(open, "clicked", G_CALLBACK(open_clicked), z); gtk_box_append(GTK_BOX(z->empty), open);
    gtk_overlay_add_overlay(GTK_OVERLAY(z->root), z->empty);
    z->top_fade = make_fade(z, "zen-top-fade", GTK_ALIGN_START);
    z->bottom_fade = make_fade(z, "zen-bottom-fade", GTK_ALIGN_END);
    z->top_left = make_controls(z, GTK_ALIGN_START, GTK_ALIGN_START, 10, 2);
    gtk_box_append(GTK_BOX(z->top_left), zen_button_new("document-open-symbolic", "Open file (Ctrl+O)", G_CALLBACK(open_clicked), z));
    z->title = gtk_label_new(app_name); gtk_label_set_ellipsize(GTK_LABEL(z->title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(z->title), 32); gtk_widget_add_css_class(z->title, "doc-name");
    gtk_widget_set_valign(z->title, GTK_ALIGN_CENTER); gtk_widget_set_size_request(z->title, -1, 34);
    z->top_center = make_revealer(z, z->title, GTK_ALIGN_CENTER, GTK_ALIGN_START, 10);
    gtk_widget_set_margin_start(z->top_center, 92); gtk_widget_set_margin_end(z->top_center, 92);
    z->top_right = make_controls(z, GTK_ALIGN_END, GTK_ALIGN_START, 10, 2);
    gtk_box_append(GTK_BOX(z->top_right), zen_button_new("view-fullscreen-symbolic", "Fullscreen (F11)", G_CALLBACK(full_clicked), z));
    GtkWidget *close = zen_button_new("window-close-symbolic", "Close (Ctrl+W)", G_CALLBACK(close_clicked), z);
    gtk_widget_add_css_class(close, "close"); gtk_box_append(GTK_BOX(z->top_right), close);
    z->controls = make_controls(z, GTK_ALIGN_CENTER, GTK_ALIGN_END, 12, 4);
    z->status = gtk_label_new(""); gtk_label_set_wrap(GTK_LABEL(z->status), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(z->status), 48);
    gtk_widget_add_css_class(z->status, "toast"); gtk_widget_set_halign(z->status, GTK_ALIGN_CENTER); gtk_widget_set_valign(z->status, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(z->status, 92); gtk_widget_set_margin_start(z->status, 16); gtk_widget_set_margin_end(z->status, 16);
    gtk_widget_set_visible(z->status, FALSE); gtk_overlay_add_overlay(GTK_OVERLAY(z->root), z->status);
    GtkEventController *motion = gtk_event_controller_motion_new(); gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
    g_signal_connect(motion, "motion", G_CALLBACK(root_motion), z); g_signal_connect(motion, "enter", G_CALLBACK(root_motion), z);
    g_signal_connect(motion, "leave", G_CALLBACK(root_leave), z); gtk_widget_add_controller(z->root, motion);
    GtkEventController *keys = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_BUBBLE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), z); gtk_widget_add_controller(GTK_WIDGET(z->window), keys);
    GtkEventController *peek = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(peek, GTK_PHASE_CAPTURE);
    g_signal_connect(peek, "key-pressed", G_CALLBACK(keyboard_peek), z); gtk_widget_add_controller(GTK_WIDGET(z->window), peek);
    g_signal_connect(z->window, "notify::focus-widget", G_CALLBACK(focus_changed), z);
    GtkGesture *click = gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(pressed), z); gtk_widget_add_controller(z->root, GTK_EVENT_CONTROLLER(click));
    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
    GType types[] = {G_TYPE_FILE, GDK_TYPE_FILE_LIST}; gtk_drop_target_set_gtypes(drop, types, G_N_ELEMENTS(types));
    g_signal_connect(drop, "drop", G_CALLBACK(dropped), z); gtk_widget_add_controller(z->root, GTK_EVENT_CONTROLLER(drop));
    zen_window_set_ready(z, FALSE); zen_window_set_title(z, NULL);
    return z;
}
void zen_window_present(ZenWindow *z) { gtk_window_present(z->window); }
