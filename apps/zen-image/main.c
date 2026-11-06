/* SPDX-License-Identifier: GPL-3.0-or-later */
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_6
#include "zen-ui.h"
#include "thumbnail.h"
#include <math.h>
#include <string.h>
#ifdef HAVE_LIBEXIF
#include <libexif/exif-loader.h>
#endif

typedef enum { FIT_IMAGE, FIT_WIDTH, ACTUAL_PIXELS } ImageFit;

typedef struct {
    GtkWidget parent_instance;
    GdkTexture *texture, *fit_texture, *thumbnail;
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
    if (self->thumbnail) {
        float tw = gdk_texture_get_width(self->thumbnail), th = gdk_texture_get_height(self->thumbnail);
        float scale = MIN(w / tw, h / th);
        graphene_rect_t bounds = GRAPHENE_RECT_INIT((w - tw * scale) / 2, (h - th * scale) / 2,
                                                    tw * scale, th * scale);
        gtk_snapshot_append_texture(snapshot, self->thumbnail, &bounds);
        return;
    }
    if (!self->texture) return;

    double scale = image_scale(self), dx = self->pan_x, dy = self->pan_y;
    pan_limits(self, &dx, &dy);
    float iw = self->image_width * scale, ih = self->image_height * scale;
    graphene_rect_t image = GRAPHENE_RECT_INIT(-iw / 2, -ih / 2, iw, ih);
    graphene_point_t center = GRAPHENE_POINT_INIT(w / 2 + dx, h / 2 + dy);
    gtk_snapshot_push_clip(snapshot, &viewport);
    gtk_snapshot_translate(snapshot, &center);
    gtk_snapshot_rotate(snapshot, self->turns * 90.f);
    GdkTexture *texture = self->texture;
    if (self->fit_texture) {
        int device_scale = gtk_widget_get_scale_factor(widget);
        if (iw * device_scale <= gdk_texture_get_width(self->fit_texture) &&
            ih * device_scale <= gdk_texture_get_height(self->fit_texture))
            texture = self->fit_texture;
    }
    gtk_snapshot_append_texture(snapshot, texture, &image);
    gtk_snapshot_pop(snapshot);
}

static void image_canvas_dispose(GObject *obj) {
    ImageCanvas *self = (ImageCanvas *)obj;
    g_clear_object(&self->texture);
    g_clear_object(&self->fit_texture);
    g_clear_object(&self->thumbnail);
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

/* Large neighbors keep their fit texture under the per-image cache budget. */
#define NEIGHBOR_BYTES ((gsize)80 * 1024 * 1024)

typedef struct {
    GdkTexture *texture, *fit_texture;
    int width, height;
    gsize bytes;
    char *metadata;
} Decoded;
typedef struct {
    GFile *file;
    GCancellable *cancel;
    GdkTexture *thumbnail;
    int fit_edge;
    gboolean changed;
} ImageLoad;
typedef struct {
    GFile *file;
    char *name, *sort_key;
    gboolean unreadable, preload_failed;
    Decoded *cached;
} ImageEntry;

static void decoded_free(gpointer data) {
    Decoded *result = data;
    g_clear_object(&result->texture); g_clear_object(&result->fit_texture);
    g_free(result->metadata); g_free(result);
}
static void image_load_free(gpointer data) {
    ImageLoad *load = data;
    g_object_unref(load->file); g_object_unref(load->cancel); g_clear_object(&load->thumbnail); g_free(load);
}
static void image_entry_free(gpointer data) {
    ImageEntry *entry = data;
    g_clear_pointer(&entry->cached, decoded_free);
    g_object_unref(entry->file); g_free(entry->name); g_free(entry->sort_key); g_free(entry);
}
static void metadata_field(GString *text, const char *name, const char *value) {
    if (!value || !*value) return;
    char *utf8 = g_utf8_make_valid(value, -1);
    g_string_append_printf(text, "%s\n%s\n\n", name, utf8); g_free(utf8);
}
static GString *image_metadata(GFile *file, const char *format, int width, int height,
                               GFileInfo *info, guint64 size, gboolean alpha) {
    GString *text = g_string_new(NULL);
    g_autofree char *name = g_file_get_basename(file);
    metadata_field(text, "File", name);
    metadata_field(text, "Format", format);
    g_autofree char *dimensions = g_strdup_printf("%d × %d pixels", width, height);
    metadata_field(text, "Dimensions", dimensions);
    g_autofree char *file_size = g_format_size_full(size, G_FORMAT_SIZE_LONG_FORMAT);
    metadata_field(text, "File size", file_size);
    metadata_field(text, "Transparency", alpha ? "Yes" : "No");
    if (info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
        g_autoptr(GDateTime) time = g_date_time_new_from_unix_local((gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED));
        if (time) {
            g_autofree char *formatted = g_date_time_format(time, "%Y-%m-%d %H:%M:%S");
            metadata_field(text, "Modified", formatted);
        }
    }
    return text;
}
#ifdef HAVE_LIBEXIF
static void exif_field(GString *text, ExifData *exif, ExifIfd ifd, ExifTag tag, const char *title) {
    ExifEntry *entry = exif_content_get_entry(exif->ifd[ifd], tag);
    if (!entry) return;
    char buffer[512] = {0};
    exif_entry_get_value(entry, buffer, sizeof(buffer));
    metadata_field(text, title, buffer);
}
static void exif_metadata(GString *text, ExifLoader *loader) {
    ExifData *exif = exif_loader_get_data(loader);
    if (!exif) return;
    exif_field(text, exif, EXIF_IFD_0, EXIF_TAG_MAKE, "Camera make");
    exif_field(text, exif, EXIF_IFD_0, EXIF_TAG_MODEL, "Camera model");
    exif_field(text, exif, EXIF_IFD_EXIF, EXIF_TAG_DATE_TIME_ORIGINAL, "Captured");
    exif_field(text, exif, EXIF_IFD_EXIF, EXIF_TAG_EXPOSURE_TIME, "Exposure");
    exif_field(text, exif, EXIF_IFD_EXIF, EXIF_TAG_FNUMBER, "Aperture");
    exif_field(text, exif, EXIF_IFD_EXIF, EXIF_TAG_ISO_SPEED_RATINGS, "ISO");
    exif_field(text, exif, EXIF_IFD_EXIF, EXIF_TAG_FOCAL_LENGTH, "Focal length");
    exif_field(text, exif, EXIF_IFD_0, EXIF_TAG_ARTIST, "Artist");
    exif_field(text, exif, EXIF_IFD_0, EXIF_TAG_COPYRIGHT, "Copyright");
    exif_data_unref(exif);
}
#endif
static void image_size(GdkPixbufLoader *loader, int width, int height, gpointer user) {
    Decoded *result = user; result->width = width; result->height = height;
    double ratio = MIN(1., MIN(16384. / MAX(width, height), sqrt((32. * 1024 * 1024) / ((double)width * height))));
    if (ratio < 1.) gdk_pixbuf_loader_set_size(loader, MAX(1, (int)(width * ratio)), MAX(1, (int)(height * ratio)));
}
static GdkTexture *texture_from_pixels(GdkPixbuf *pixels) {
    GBytes *bytes = g_bytes_new_with_free_func(gdk_pixbuf_read_pixels(pixels),
        gdk_pixbuf_get_byte_length(pixels), g_object_unref, g_object_ref(pixels));
    GdkMemoryFormat format = gdk_pixbuf_get_has_alpha(pixels) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
    GdkTexture *texture = gdk_memory_texture_new(gdk_pixbuf_get_width(pixels), gdk_pixbuf_get_height(pixels),
                                                 format, bytes, gdk_pixbuf_get_rowstride(pixels));
    g_bytes_unref(bytes);
    return texture;
}
static void prepare_textures(Decoded *result, GdkPixbuf *pixels, int fit_edge) {
    result->texture = texture_from_pixels(pixels);
    result->bytes = gdk_pixbuf_get_byte_length(pixels);
    int w = gdk_pixbuf_get_width(pixels), h = gdk_pixbuf_get_height(pixels);
    if (MAX(w, h) <= fit_edge * 1.5 && result->bytes <= NEIGHBOR_BYTES) return;
    double ratio = (double)fit_edge / MAX(w, h);
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixels, MAX(1, (int)(w * ratio)),
        MAX(1, (int)(h * ratio)), GDK_INTERP_BILINEAR);
    if (scaled) {
        result->fit_texture = texture_from_pixels(scaled);
        result->bytes += gdk_pixbuf_get_byte_length(scaled);
        g_object_unref(scaled);
    }
}
static void publish_thumbnail(GTask *task, GdkPixbuf *pixels);
#ifdef HAVE_LIBRAW
#include "raw.h"

static void decode_raw(GTask *task, ImageLoad *load, const char *extension, GCancellable *cancel) {
    GError *error = NULL;
    g_autoptr(GFileInfo) info = g_file_query_info(load->file, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_QUERY_INFO_NONE, cancel, &error);
    RawInfo raw = {0};
    g_autoptr(GdkPixbuf) pixels = info ? raw_decode(task, load->file, info, &raw, cancel, &error) : NULL;
    if (!pixels) { g_task_return_error(task, error); return; }

    Decoded *result = g_new0(Decoded, 1);
    result->width = gdk_pixbuf_get_width(pixels);
    result->height = gdk_pixbuf_get_height(pixels);
    g_autofree char *suffix = g_ascii_strup(extension, -1);
    g_autofree char *format = g_strdup_printf("Camera RAW (%s)", suffix);
    GString *text = image_metadata(load->file, format, result->width, result->height,
        info, (guint64)g_file_info_get_size(info), FALSE);
    metadata_field(text, "Rendering", raw.embedded ? "Embedded camera preview" : "Quick RAW preview");
    if (raw.width && raw.height) {
        g_autofree char *dimensions = g_strdup_printf("%d × %d pixels", raw.width, raw.height);
        metadata_field(text, "Sensor image", dimensions);
    }
    metadata_field(text, "Camera make", raw.make);
    metadata_field(text, "Camera model", raw.model);
    g_autofree char *location = g_file_get_parse_name(load->file);
    metadata_field(text, "Location", location);
    result->metadata = g_string_free(text, FALSE);
    prepare_textures(result, pixels, load->fit_edge);
    g_task_return_pointer(task, result, decoded_free);
}
#endif

static void decode(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    ImageLoad *load = data;
    GdkPixbuf *thumbnail = load_cached_thumbnail(load->file, cancel);
    if (thumbnail) {
        publish_thumbnail(task, thumbnail);
        g_object_unref(thumbnail);
    }
#ifdef HAVE_LIBRAW
    g_autofree char *basename = g_file_get_basename(load->file);
    const char *extension = basename ? raw_extension(basename) : NULL;
    if (extension) { decode_raw(task, load, extension, cancel); return; }
#endif
    GError *error = NULL;
    GFileInputStream *in = g_file_read(load->file, cancel, &error);
    if (!in) { g_task_return_error(task, error); return; }
    GFileInfo *info = g_file_input_stream_query_info(in, G_FILE_ATTRIBUTE_STANDARD_SIZE ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED, cancel, NULL);
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    Decoded *result = g_new0(Decoded, 1);
    g_signal_connect(loader, "size-prepared", G_CALLBACK(image_size), result);
#ifdef HAVE_LIBEXIF
    ExifLoader *exif = exif_loader_new();
    gboolean want_exif = exif != NULL;
#endif
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
#ifdef HAVE_LIBEXIF
        if (want_exif) want_exif = exif_loader_write(exif, buffer, (unsigned int)n);
#endif
        if (!gdk_pixbuf_loader_write(loader, buffer, (gsize)n, &error)) break;
    }
    g_object_unref(in);
    gdk_pixbuf_loader_close(loader, error ? NULL : &error);
    g_autoptr(GdkPixbuf) pixels = NULL;
    if (!error) {
        GdkPixbuf *pix = gdk_pixbuf_loader_get_pixbuf(loader);
        if (pix) {
            const char *orientation = gdk_pixbuf_get_option(pix, "orientation");
            if (orientation && orientation[0] >= '5' && orientation[0] <= '8') {
                int swap = result->width; result->width = result->height; result->height = swap;
            }
            pixels = gdk_pixbuf_apply_embedded_orientation(pix);
        }
        if (!pixels || result->width <= 0 || result->height <= 0)
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The image contains no readable pixels.");
    }
    if (!error) g_cancellable_set_error_if_cancelled(cancel, &error);
    if (!error) {
        GdkPixbufFormat *format = gdk_pixbuf_loader_get_format(loader);
        g_autofree char *description = format ? gdk_pixbuf_format_get_description(format) : NULL;
        GString *text = image_metadata(load->file, description, result->width, result->height,
            info, info ? (guint64)g_file_info_get_size(info) : total, gdk_pixbuf_get_has_alpha(pixels));
        if (gdk_pixbuf_get_width(pixels) != result->width || gdk_pixbuf_get_height(pixels) != result->height) {
            char *preview = g_strdup_printf("%d × %d pixels", gdk_pixbuf_get_width(pixels), gdk_pixbuf_get_height(pixels));
            metadata_field(text, "Preview resolution", preview); g_free(preview);
        }
#ifdef HAVE_LIBEXIF
        if (exif) exif_metadata(text, exif);
#endif
        char *location = g_file_get_parse_name(load->file);
        metadata_field(text, "Location", location); g_free(location);
        result->metadata = g_string_free(text, FALSE);
        prepare_textures(result, pixels, load->fit_edge);
    }
#ifdef HAVE_LIBEXIF
    if (exif) exif_loader_unref(exif);
#endif
    g_clear_object(&info); g_object_unref(loader);
    if (error) { decoded_free(result); g_task_return_error(task, error); }
    else g_task_return_pointer(task, result, decoded_free);
}

/* Collation keys implement the desktop's natural numeric ordering (2 before 10). */
static gint compare_entries(gconstpointer a, gconstpointer b) {
    const ImageEntry *left = *(ImageEntry * const *)a, *right = *(ImageEntry * const *)b;
    int order = strcmp(left->sort_key, right->sort_key);
    return order ? order : strcmp(left->name, right->name);
}
static GHashTable *image_extensions(void) {
    GHashTable *extensions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GSList *formats = gdk_pixbuf_get_formats();
    for (GSList *item = formats; item; item = item->next) {
        GdkPixbufFormat *format = item->data;
        if (gdk_pixbuf_format_is_disabled(format)) continue;
        char **names = gdk_pixbuf_format_get_extensions(format);
        for (int i = 0; names[i]; i++) g_hash_table_add(extensions, g_ascii_strdown(names[i], -1));
        g_strfreev(names);
    }
#ifdef HAVE_LIBRAW
    for (guint i = 0; raw_extensions[i]; i++) g_hash_table_add(extensions, g_strdup(raw_extensions[i]));
#endif
    g_slist_free(formats); return extensions;
}
static void scan_directory(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    GFile *current = data, *parent = g_file_get_parent(current);
    GPtrArray *entries = g_ptr_array_new_with_free_func(image_entry_free);
    if (!parent) { g_task_return_pointer(task, entries, (GDestroyNotify)g_ptr_array_unref); return; }
    GError *error = NULL;
    GFileEnumerator *files = g_file_enumerate_children(parent, G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
        G_FILE_QUERY_INFO_NONE, cancel, &error);
    if (!files) { g_object_unref(parent); g_ptr_array_unref(entries); g_task_return_error(task, error); return; }
    GHashTable *extensions = image_extensions();
    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(files, cancel, &error))) {
        const char *name = g_file_info_get_name(info);
        GFile *file = g_file_get_child(parent, name);
        const char *extension = strrchr(name, '.');
        char *lower = extension ? g_ascii_strdown(extension + 1, -1) : NULL;
        const char *type = g_file_info_get_content_type(info);
        gboolean image = (lower && g_hash_table_contains(extensions, lower)) ||
            (type && g_str_has_prefix(type, "image/")) || g_file_equal(file, current);
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR && image) {
            ImageEntry *entry = g_new0(ImageEntry, 1);
            entry->file = g_object_ref(file); entry->name = g_filename_display_name(name);
            entry->sort_key = g_utf8_collate_key_for_filename(entry->name, -1);
            g_ptr_array_add(entries, entry);
        }
        g_free(lower); g_object_unref(file); g_object_unref(info);
    }
    g_hash_table_unref(extensions); g_object_unref(files); g_object_unref(parent);
    if (error) { g_ptr_array_unref(entries); g_task_return_error(task, error); return; }
    g_ptr_array_sort(entries, compare_entries);
    g_task_return_pointer(task, entries, (GDestroyNotify)g_ptr_array_unref);
}

typedef struct {
    ZenWindow *window;
    ImageCanvas *canvas;
    GtkWidget *counter, *previous, *next, *metadata, *metadata_text;
    GtkWidget *progress, *spinner;
    guint progress_timer, load_idle;
    GCancellable *listing;
    GFileMonitor *monitor;
    ImageLoad *loads[2];  /* Owned by their tasks until the completion callback. */
    ImageEntry *neighbors[2];
    Decoded *displayed;
    GFile *requested, *current;
    GPtrArray *files;
    int cursor, pending_steps, skipped, direction;
    gboolean waiting, cache_current;
} ImageView;

static void request_image(ImageView *view, GFile *file, int direction);
static void schedule_loads(ImageView *view);
static void show_thumbnail(ImageView *view, GdkTexture *texture);
static int find_image(ImageView *view, GFile *file) {
    if (view->files && file) for (guint i = 0; i < view->files->len; i++) {
        ImageEntry *entry = g_ptr_array_index(view->files, i);
        if (g_file_equal(entry->file, file)) return (int)i;
    }
    return -1;
}
static int adjacent_image(ImageView *view, int index, int direction) {
    if (!view->files || index < 0) return -1;
    int candidate = index + direction;
    while (candidate >= 0 && candidate < (int)view->files->len) {
        if (!((ImageEntry *)g_ptr_array_index(view->files, candidate))->unreadable) return candidate;
        candidate += direction;
    }
    return -1;
}
static ImageEntry *neighbor_at(ImageView *view, int direction) {
    int index = adjacent_image(view, view->cursor, direction);
    return index >= 0 ? g_ptr_array_index(view->files, index) : NULL;
}
static ImageEntry *find_neighbor(ImageView *view, GFile *file) {
    for (int direction = -1; direction <= 1; direction += 2) {
        ImageEntry *entry = neighbor_at(view, direction);
        if (entry && g_file_equal(entry->file, file)) return entry;
    }
    return NULL;
}
static void update_neighbors(ImageView *view) {
    ImageEntry *previous = neighbor_at(view, -1), *next = neighbor_at(view, 1);
    for (guint i = 0; i < G_N_ELEMENTS(view->neighbors); i++) {
        ImageEntry *old = view->neighbors[i];
        if (old && old != previous && old != next) g_clear_pointer(&old->cached, decoded_free);
    }
    view->neighbors[0] = previous; view->neighbors[1] = next;
}
static void cache_image(ImageEntry *entry, Decoded *result) {
    if (result->bytes > NEIGHBOR_BYTES) g_clear_object(&result->texture);
    g_clear_pointer(&entry->cached, decoded_free);
    entry->cached = result;
}
static void refresh_navigation(ImageView *view) {
    int count = view->files ? (int)view->files->len : 0;
    gtk_widget_set_sensitive(view->previous, adjacent_image(view, view->cursor, -1) >= 0);
    gtk_widget_set_sensitive(view->next, adjacent_image(view, view->cursor, 1) >= 0);
    char *label = view->cursor >= 0 ? g_strdup_printf("%d / %d", view->cursor + 1, count) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(view->counter), label); g_free(label);
}
static gboolean navigate(ImageView *view, int delta) {
    if (view->listing) {
        view->pending_steps = CLAMP(view->pending_steps + delta, -1000000, 1000000);
        return TRUE;
    }
    if (!view->files || view->cursor < 0) return FALSE;
    int target = view->cursor, direction = delta > 0 ? 1 : -1;
    for (int step = 0; step < ABS(delta); step++) {
        int candidate = adjacent_image(view, target, direction);
        if (candidate < 0) break;
        target = candidate;
    }
    if (target == view->cursor) return FALSE;
    view->cursor = target;
    ImageEntry *entry = g_ptr_array_index(view->files, target);
    request_image(view, entry->file, delta > 0 ? 1 : -1);
    refresh_navigation(view);
    return TRUE;
}
static void invalidate_image(ImageView *view, GFile *file) {
    if (!file) return;
    if (view->current && g_file_equal(view->current, file)) view->cache_current = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(view->neighbors); i++) {
        ImageEntry *entry = view->neighbors[i];
        if (entry && g_file_equal(entry->file, file)) g_clear_pointer(&entry->cached, decoded_free);
    }
    for (guint i = 0; i < G_N_ELEMENTS(view->loads); i++) {
        ImageLoad *load = view->loads[i];
        if (load && g_file_equal(load->file, file)) {
            load->changed = TRUE;
            g_clear_object(&load->thumbnail);
        }
    }
    if (view->waiting && g_file_equal(view->requested, file)) show_thumbnail(view, NULL);
}
static void folder_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                           GFileMonitorEvent event, gpointer data) {
    (void)monitor; (void)event;
    ImageView *view = data;
    if (zen_window_is_closed(view->window)) return;
    invalidate_image(view, file);
    invalidate_image(view, other);
}
static void stop_monitor(ImageView *view) {
    if (!view->monitor) return;
    g_signal_handlers_disconnect_by_data(view->monitor, view);
    g_file_monitor_cancel(view->monitor);
    g_clear_object(&view->monitor);
}
static void directory_ready(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    ImageView *view = g_object_get_data(source, "zen-image-view");
    GTask *task = G_TASK(result); GError *error = NULL;
    GPtrArray *files = g_task_propagate_pointer(task, &error);
    if (view->listing == g_task_get_cancellable(task) && !zen_window_is_closed(view->window)) {
        g_clear_object(&view->listing);
        view->files = g_steal_pointer(&files);
        if (view->files && g_file_is_native(view->requested)) {
            g_autoptr(GFile) parent = g_file_get_parent(view->requested);
            if (parent) view->monitor = g_file_monitor_directory(parent, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
            if (view->monitor) g_signal_connect(view->monitor, "changed", G_CALLBACK(folder_changed), view);
        }
        view->cursor = find_image(view, view->requested);
        refresh_navigation(view);
        int steps = view->pending_steps; view->pending_steps = 0;
        if (steps) navigate(view, steps);
        schedule_loads(view);
        if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            zen_window_notice(view->window, "This folder could not be listed. You can still open individual images.");
    }
    g_clear_pointer(&files, g_ptr_array_unref); g_clear_error(&error);
}

static gboolean show_progress(gpointer data) {
    ImageView *view = data;
    view->progress_timer = 0;
    gtk_widget_set_visible(view->progress, TRUE);
    gtk_spinner_start(GTK_SPINNER(view->spinner));
    return G_SOURCE_REMOVE;
}
static void hide_progress(ImageView *view) {
    g_clear_handle_id(&view->progress_timer, g_source_remove);
    gtk_spinner_stop(GTK_SPINNER(view->spinner));
    gtk_widget_set_visible(view->progress, FALSE);
}
static void show_thumbnail(ImageView *view, GdkTexture *texture) {
    if (g_set_object(&view->canvas->thumbnail, texture)) {
        gtk_widget_queue_draw(GTK_WIDGET(view->canvas));
        if (!view->displayed) zen_window_set_ready(view->window, texture != NULL);
    }
}
static void present_image(ImageView *view, GFile *file, Decoded *result) {
    view->waiting = FALSE;
    g_clear_handle_id(&view->load_idle, g_source_remove);
    hide_progress(view);
    show_thumbnail(view, NULL);
    update_neighbors(view);
    if (view->displayed != result) {
        ImageEntry *previous = view->cache_current ? find_neighbor(view, view->current) : NULL;
        if (previous) cache_image(previous, g_steal_pointer(&view->displayed));
        g_clear_pointer(&view->displayed, decoded_free);
        view->displayed = result;
    }
    view->cache_current = TRUE;
    ImageCanvas *canvas = view->canvas;
    g_set_object(&canvas->texture, result->texture);
    g_set_object(&canvas->fit_texture, result->fit_texture);
    canvas->image_width = result->width; canvas->image_height = result->height;
    canvas->turns = 0; set_fit(canvas, FIT_IMAGE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view->metadata_text)), result->metadata, -1);
    g_set_object(&view->current, file);
    char *name = g_file_get_basename(file);
    zen_window_set_title(view->window, name); g_free(name);
    zen_window_clear_notice(view->window); zen_window_set_ready(view->window, TRUE);
    if (view->skipped) {
        char *message = g_strdup_printf("Skipped %d unreadable image%s.", view->skipped, view->skipped == 1 ? "" : "s");
        zen_window_notice(view->window, message); g_free(message); view->skipped = 0;
    }
    if (!gtk_widget_get_visible(view->metadata)) gtk_widget_grab_focus(GTK_WIDGET(canvas));
}

static ImageLoad *find_load(ImageView *view, GFile *file) {
    for (guint i = 0; i < G_N_ELEMENTS(view->loads); i++) {
        ImageLoad *load = view->loads[i];
        if (load && !g_cancellable_is_cancelled(load->cancel) && g_file_equal(load->file, file)) return load;
    }
    return NULL;
}
typedef struct { GTask *task; GdkTexture *texture; } ThumbnailUpdate;
static gboolean thumbnail_ready(gpointer data) {
    ThumbnailUpdate *update = data;
    ImageLoad *load = g_task_get_task_data(update->task);
    ImageView *view = g_object_get_data(g_task_get_source_object(update->task), "zen-image-view");
    if (!zen_window_is_closed(view->window) && !load->changed && find_load(view, load->file) == load) {
        g_set_object(&load->thumbnail, update->texture);
        if (view->waiting && g_file_equal(view->requested, load->file) && !view->canvas->thumbnail)
            show_thumbnail(view, update->texture);
    }
    return G_SOURCE_REMOVE;
}
static void thumbnail_update_free(gpointer data) {
    ThumbnailUpdate *update = data;
    g_object_unref(update->task); g_object_unref(update->texture); g_free(update);
}
static void publish_thumbnail(GTask *task, GdkPixbuf *pixels) {
    ThumbnailUpdate *update = g_new(ThumbnailUpdate, 1);
    update->task = g_object_ref(task);
    update->texture = texture_from_pixels(pixels);
    GSource *idle = g_idle_source_new();
    g_source_set_priority(idle, G_PRIORITY_DEFAULT);
    g_source_set_callback(idle, thumbnail_ready, update, thumbnail_update_free);
    g_source_attach(idle, g_task_get_context(task));
    g_source_unref(idle);
}

static void load_failed(ImageView *view, GFile *file, const GError *error) {
    view->waiting = FALSE;
    show_thumbnail(view, NULL);
    if (view->direction) {
        view->skipped++;
        int failed = find_image(view, file);
        if (failed >= 0) ((ImageEntry *)g_ptr_array_index(view->files, failed))->unreadable = TRUE;
        if (navigate(view, view->direction)) return;
        view->cursor = find_image(view, view->current);
        refresh_navigation(view);
        zen_window_notice(view->window, "No more readable images in this direction.");
    } else {
        /* Some optional loaders return a remote traceback. Keep it out of the canvas. */
        const char *reason = error->message;
        if (strlen(reason) > 200 || strchr(reason, '\n'))
            reason = "The image could not be decoded. It may be damaged or need an additional image loader.";
        char *message = g_strdup_printf("%s\nUse zen-image --formats to see installed image formats.", reason);
        zen_window_notice(view->window, message); g_free(message);
    }
    hide_progress(view);
}

static void decoded(GObject *source, GAsyncResult *async_result, gpointer unused) {
    (void)unused;
    ImageView *view = g_object_get_data(source, "zen-image-view");
    GTask *task = G_TASK(async_result);
    ImageLoad *load = g_task_get_task_data(task);
    for (guint i = 0; i < G_N_ELEMENTS(view->loads); i++)
        if (view->loads[i] == load) view->loads[i] = NULL;
    g_autoptr(GError) error = NULL;
    Decoded *result = g_task_propagate_pointer(task, &error);
    if (!zen_window_is_closed(view->window)) {
        if (!g_cancellable_is_cancelled(load->cancel)) {
            if (view->waiting && g_file_equal(view->requested, load->file)) {
                if (result) {
                    present_image(view, load->file, g_steal_pointer(&result));
                    if (load->changed) view->cache_current = FALSE;
                } else load_failed(view, load->file, error);
            } else {
                ImageEntry *entry = find_neighbor(view, load->file);
                if (entry && result && !load->changed) cache_image(entry, g_steal_pointer(&result));
                if (entry && error) entry->preload_failed = TRUE;
            }
        }
        schedule_loads(view);
    }
    g_clear_pointer(&result, decoded_free);
}

static void start_load(ImageView *view, GFile *file, gboolean preload) {
    guint slot = 0;
    while (slot < G_N_ELEMENTS(view->loads) && view->loads[slot]) slot++;
    if (slot == G_N_ELEMENTS(view->loads)) return;
    ImageLoad *load = g_new0(ImageLoad, 1);
    load->file = g_object_ref(file); load->cancel = g_cancellable_new();
    GtkWidget *canvas = GTK_WIDGET(view->canvas);
    int edge = MAX(gtk_widget_get_width(canvas), gtk_widget_get_height(canvas)) * gtk_widget_get_scale_factor(canvas);
    /* Round up for small window resizes without uploading the entire source. */
    load->fit_edge = CLAMP(((edge + 255) / 256) * 256, 1024, 3072);
    view->loads[slot] = load;
    GTask *task = g_task_new(zen_window_get_window(view->window), load->cancel, decoded, NULL);
    if (preload) g_task_set_priority(task, G_PRIORITY_DEFAULT_IDLE);
    g_task_set_task_data(task, load, image_load_free);
    g_task_run_in_thread(task, decode); g_object_unref(task);
}
static void schedule_loads(ImageView *view) {
    if (view->waiting) {
        if (!view->load_idle && !find_load(view, view->requested)) start_load(view, view->requested, FALSE);
        return;
    }
    update_neighbors(view);
    /* Only one speculative decode at a time. A second slot lets foreground work
     * begin while a superseded decoder is still unwinding. */
    if (view->loads[0] || view->loads[1]) return;
    guint first = view->direction < 0 ? 0 : 1;
    for (guint i = 0; i < G_N_ELEMENTS(view->neighbors); i++) {
        ImageEntry *entry = view->neighbors[(first + i) % G_N_ELEMENTS(view->neighbors)];
        if (entry && !entry->cached && !entry->preload_failed) {
            start_load(view, entry->file, TRUE);
            return;
        }
    }
}
static gboolean start_latest_load(gpointer data) {
    ImageView *view = data;
    view->load_idle = 0;
    schedule_loads(view);
    return G_SOURCE_REMOVE;
}
static void cancel_loads(ImageView *view, ImageLoad *keep, gboolean keep_neighbors) {
    for (guint i = 0; i < G_N_ELEMENTS(view->loads); i++) {
        ImageLoad *load = view->loads[i];
        if (load && load != keep && (!keep_neighbors || !find_neighbor(view, load->file)))
            g_cancellable_cancel(load->cancel);
    }
}
static void request_image(ImageView *view, GFile *file, int direction) {
    g_set_object(&view->requested, file);
    view->direction = direction;
    ImageEntry *entry = direction ? g_ptr_array_index(view->files, view->cursor) : NULL;
    Decoded *ready = NULL;
    if (direction && view->cache_current && g_file_equal(view->current, file)) ready = view->displayed;
    else if (entry && entry->cached && entry->cached->texture) ready = g_steal_pointer(&entry->cached);
    ImageLoad *load = direction ? find_load(view, file) : NULL;
    cancel_loads(view, load, ready != NULL);
    if (ready) present_image(view, file, ready);
    else {
        view->waiting = TRUE;
        zen_window_clear_notice(view->window);
        GdkTexture *preview = entry && entry->cached ? entry->cached->fit_texture : load ? load->thumbnail : NULL;
        show_thumbnail(view, preview);
        if (!view->progress_timer && !gtk_widget_get_visible(view->progress))
            view->progress_timer = g_timeout_add(100, show_progress, view);
        /* Let queued key repeats settle on a target before starting disk work. */
        if (!load && !view->load_idle)
            view->load_idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE, start_latest_load, view, NULL);
    }
    schedule_loads(view);
}
static void open_image(ZenWindow *window, GFile *file, gpointer user) {
    ImageView *view = user;
    if (view->listing) g_cancellable_cancel(view->listing);
    g_clear_object(&view->listing);
    stop_monitor(view);
    memset(view->neighbors, 0, sizeof(view->neighbors));
    g_clear_pointer(&view->files, g_ptr_array_unref);
    view->cache_current = FALSE;
    view->cursor = -1; view->pending_steps = 0; view->skipped = 0;
    refresh_navigation(view);
    request_image(view, file, 0);
    view->listing = g_cancellable_new();
    GTask *task = g_task_new(zen_window_get_window(window), view->listing, directory_ready, NULL);
    g_task_set_task_data(task, g_object_ref(file), g_object_unref);
    g_task_run_in_thread(task, scan_directory); g_object_unref(task);
}
static void release_view(gpointer user) {
    ImageView *view = user;
    g_clear_handle_id(&view->progress_timer, g_source_remove);
    g_clear_handle_id(&view->load_idle, g_source_remove);
    g_clear_object(&view->listing);
    stop_monitor(view);
    g_clear_object(&view->requested); g_clear_object(&view->current);
    g_clear_pointer(&view->displayed, decoded_free);
    g_clear_pointer(&view->files, g_ptr_array_unref); g_free(view);
}
static gboolean closing(GtkWindow *window, gpointer user) {
    (void)window;
    ImageView *view = user;
    cancel_loads(view, NULL, FALSE);
    stop_monitor(view);
    g_clear_handle_id(&view->load_idle, g_source_remove);
    hide_progress(view);
    if (view->listing) g_cancellable_cancel(view->listing);
    return FALSE;
}
static void toggle_metadata(ImageView *view) {
    gboolean shown = !gtk_widget_get_visible(view->metadata);
    gtk_widget_set_visible(view->metadata, shown);
    gtk_widget_grab_focus(shown ? view->metadata_text : GTK_WIDGET(view->canvas));
}
static void click_metadata(GtkButton *button, gpointer user) { (void)button; toggle_metadata(user); }
static void click_previous(GtkButton *button, gpointer user) {
    (void)button; ImageView *view = user; view->skipped = 0; navigate(view, -1);
}
static void click_next(GtkButton *button, gpointer user) {
    (void)button; ImageView *view = user; view->skipped = 0; navigate(view, 1);
}

static gboolean image_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer user) {
    (void)controller; (void)code;
    ImageView *view = user;
    ImageCanvas *canvas = view->canvas;
    if (state & (GDK_ALT_MASK | GDK_SUPER_MASK)) return FALSE;
    if (key == GDK_KEY_i || key == GDK_KEY_I) { toggle_metadata(view); return TRUE; }
    if (key == GDK_KEY_Escape && gtk_widget_get_visible(view->metadata)) {
        toggle_metadata(view); return TRUE;
    }
    GtkWidget *focus = gtk_window_get_focus(zen_window_get_window(view->window));
    if (focus && GTK_IS_TEXT_VIEW(focus) && (state & GDK_CONTROL_MASK)) return FALSE;
    if (key == GDK_KEY_Left || key == GDK_KEY_less || key == GDK_KEY_comma) {
        view->skipped = 0; navigate(view, -1); return TRUE;
    }
    if (key == GDK_KEY_Right || key == GDK_KEY_greater || key == GDK_KEY_period) {
        view->skipped = 0; navigate(view, 1); return TRUE;
    }
    if (!canvas->texture) return FALSE;
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
static void click_in(GtkButton *b, gpointer data) { (void)b; zoom_image(data, 1.25, FALSE); }
static void click_out(GtkButton *b, gpointer data) { (void)b; zoom_image(data, .8, FALSE); }
static void click_rotate(GtkButton *b, gpointer data) { (void)b; rotate_image(data); }

static void new_view(GtkApplication *application, GFile *file) {
    ImageView *view = g_new0(ImageView, 1);
    ZenWindow *window = zen_window_new(application, "Zen Image", 790, 620);
    view->window = window; view->cursor = -1;
    ImageCanvas *canvas = g_object_new(image_canvas_get_type(), NULL);
    view->canvas = canvas;
    GtkWindow *native = zen_window_get_window(window);
    g_object_set_data_full(G_OBJECT(native), "zen-image-view", view, release_view);
    g_signal_connect(native, "close-request", G_CALLBACK(closing), view);

    GtkWidget *content = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(content), GTK_WIDGET(canvas));
    view->progress = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view->spinner = gtk_spinner_new();
    gtk_box_append(GTK_BOX(view->progress), view->spinner);
    gtk_box_append(GTK_BOX(view->progress), gtk_label_new("Opening image…"));
    gtk_widget_add_css_class(view->progress, "toast");
    gtk_widget_set_halign(view->progress, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(view->progress, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(view->progress, 92);
    gtk_widget_set_can_target(view->progress, FALSE);
    gtk_widget_set_visible(view->progress, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(content), view->progress);
    view->metadata = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(view->metadata, "image-metadata");
    gtk_widget_set_halign(view->metadata, GTK_ALIGN_END);
    gtk_widget_set_size_request(view->metadata, 280, -1);
    gtk_widget_set_margin_top(view->metadata, 66); gtk_widget_set_margin_bottom(view->metadata, 66);
    gtk_widget_set_margin_start(view->metadata, 16); gtk_widget_set_margin_end(view->metadata, 16);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(header, "chrome");
    GtkWidget *title = gtk_label_new("Image details");
    gtk_widget_set_hexpand(title, TRUE); gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_set_margin_start(title, 12); gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), zen_button_new("window-close-symbolic", "Hide image details (I)", G_CALLBACK(click_metadata), view));
    gtk_box_append(GTK_BOX(view->metadata), header);
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);
    view->metadata_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view->metadata_text), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view->metadata_text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view->metadata_text), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view->metadata_text), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view->metadata_text), 12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view->metadata_text), 12);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view->metadata_text)), "Open an image to view its details.", -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view->metadata_text);
    gtk_box_append(GTK_BOX(view->metadata), scroller);
    gtk_overlay_add_overlay(GTK_OVERLAY(content), view->metadata);
    gtk_widget_set_visible(view->metadata, FALSE);
    zen_window_set_child(window, content);
    zen_window_set_open_handler(window, open_image, view);
    zen_window_set_empty_text(window, "Zen Image", "Drop an image here, or open one to view it.");
    const char *const mimes[] = {"image/*", NULL};
    zen_window_add_file_filter(window, "Images", mimes, NULL);

    GtkWidget *controls = zen_window_get_controls(window);
    view->previous = zen_button_new("go-previous-symbolic", "Previous image (← or <)", G_CALLBACK(click_previous), view);
    view->next = zen_button_new("go-next-symbolic", "Next image (→ or >)", G_CALLBACK(click_next), view);
    view->counter = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(view->counter), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(view->counter), 10);
    gtk_box_append(GTK_BOX(controls), view->previous);
    gtk_box_append(GTK_BOX(controls), view->counter);
    gtk_box_append(GTK_BOX(controls), view->next);
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-out-symbolic", "Zoom out (−)", G_CALLBACK(click_out), canvas));
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-in-symbolic", "Zoom in (+)", G_CALLBACK(click_in), canvas));
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-fit-best-symbolic", "Fit image (P)", G_CALLBACK(click_fit), canvas));
    gtk_box_append(GTK_BOX(controls), zen_button_new("object-rotate-right-symbolic", "Rotate clockwise (R)", G_CALLBACK(click_rotate), canvas));

    gtk_box_append(GTK_BOX(controls), zen_button_new("dialog-information-symbolic", "Image details (I)", G_CALLBACK(click_metadata), view));
    refresh_navigation(view);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(image_key), view);
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

static void start(GtkApplication *application, gpointer data) {
    (void)application; (void)data; zen_ui_init();
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".image-metadata { background: #252c2a; color: #eff4ee; border-radius: 12px; border: 1px solid #58645b; }"
        ".image-metadata textview, .image-metadata textview text { background: #252c2a; color: #eff4ee; font-size: 13px; }"
        ".image-metadata textview text selection { background: #6c8c7c; color: white; }", -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}
static void activate(GtkApplication *application, gpointer data) { (void)data; new_view(application, NULL); }
static void opened(GtkApplication *application, GFile **files, int n, const char *hint, gpointer data) {
    (void)hint; (void)data;
    for (int i = 0; i < n; i++) new_view(application, files[i]);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--formats")) {
        GSList *formats = gdk_pixbuf_get_formats();
        for (GSList *item = formats; item; item = item->next) {
            GdkPixbufFormat *format = item->data;
            if (gdk_pixbuf_format_is_disabled(format)) continue;
            char *name = gdk_pixbuf_format_get_description(format);
            char **extensions = gdk_pixbuf_format_get_extensions(format);
            char *joined = g_strjoinv(", ", extensions);
            g_print("%s (%s)\n", name, joined);
            g_free(name); g_free(joined); g_strfreev(extensions);
        }
        g_slist_free(formats);
#ifdef HAVE_LIBRAW
        g_autofree char *extensions = g_strjoinv(", ", (char **)raw_extensions);
        g_print("Camera RAW previews (%s)\n", extensions);
#endif
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--version")) { g_print("zen-image " ZEN_VERSION "\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        g_print("Usage: zen-image [IMAGE …]\n←/→ images · I image details · P fit · W fit width · 1 actual size · +/− zoom · R rotate · drag to pan\n");
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
