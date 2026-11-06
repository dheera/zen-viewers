/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <libraw/libraw.h>
#pragma GCC diagnostic pop

#define RAW_FILE_LIMIT (256u * 1024 * 1024)
#define RAW_PREVIEW_EDGE 4096
#define RAW_PREVIEW_PIXELS (16u * 1024 * 1024)
#define RAW_SOURCE_PIXELS (4u * RAW_PREVIEW_PIXELS)

static const char *const raw_extensions[] = {
    "cr2", "cr3", "crw", "arw", "sr2", "srf", "dng", "nef", "nrw", "raf", "orf",
    "rw2", "rwl", "pef", "srw", "3fr", "iiq", "erf", "kdc", "mef", "mos", "mrw", NULL
};

typedef struct {
    int width, height;
    char make[64], model[64];
    gboolean embedded;
} RawInfo;

static const char *raw_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (dot) for (guint i = 0; raw_extensions[i]; i++)
        if (!g_ascii_strcasecmp(dot + 1, raw_extensions[i])) return raw_extensions[i];
    return NULL;
}

static int raw_progress(void *data, enum LibRaw_progress stage, int iteration, int expected) {
    (void)stage; (void)iteration; (void)expected;
    return g_cancellable_is_cancelled(data);
}

static void raw_pixels_free(guchar *pixels, gpointer image) {
    (void)pixels;
    libraw_dcraw_clear_mem(image);
}

/* On success the pixbuf owns the LibRaw buffer. */
static GdkPixbuf *raw_bitmap(libraw_processed_image_t *image) {
    gsize pixels = (gsize)image->width * image->height;
    if (image->type != LIBRAW_IMAGE_BITMAP || image->bits != 8 || image->colors != 3 ||
        !pixels || pixels > RAW_PREVIEW_PIXELS || image->width > 16384 || image->height > 16384 ||
        image->data_size != pixels * 3) return NULL;
    return gdk_pixbuf_new_from_data(image->data, GDK_COLORSPACE_RGB, FALSE, 8,
        image->width, image->height, image->width * 3, raw_pixels_free, image);
}

/* Inspect the JPEG frame before the loader allocates decode buffers. Some RAW
 * containers omit the embedded preview's dimensions from their metadata. */
static gboolean raw_jpeg_size_allowed(const guchar *data, gsize length) {
    if (length < 4 || data[0] != 0xff || data[1] != 0xd8) return FALSE;
    gsize pos = 2;
    while (pos + 3 < length && data[pos++] == 0xff) {
        while (pos < length && data[pos] == 0xff) pos++;
        if (pos + 2 >= length) break;
        guint marker = data[pos++];
        if (marker == 0xda || marker == 0xd9) break;
        guint size = ((guint)data[pos] << 8) | data[pos + 1];
        if (size < 2 || size > length - pos) break;
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (size < 8) break;
            guint height = ((guint)data[pos + 3] << 8) | data[pos + 4];
            guint width = ((guint)data[pos + 5] << 8) | data[pos + 6];
            return width && height && width <= 16384 && height <= 16384 &&
                (gsize)width * height <= RAW_SOURCE_PIXELS;
        }
        pos += size;
    }
    return FALSE;
}

static void raw_preview_size(GdkPixbufLoader *loader, int width, int height, gpointer unused) {
    (void)unused;
    int edge = MAX(width, height);
    if (edge > RAW_PREVIEW_EDGE) {
        double scale = (double)RAW_PREVIEW_EDGE / edge;
        gdk_pixbuf_loader_set_size(loader, MAX(1, (int)(width * scale)), MAX(1, (int)(height * scale)));
    }
}

static GdkPixbuf *raw_embedded_preview(libraw_data_t *raw, GCancellable *cancel, int *status) {
    libraw_thumbnail_t *thumb = &raw->thumbnail;
    if (thumb->tlength > RAW_PREVIEW_PIXELS * 3 ||
        (gsize)thumb->twidth * thumb->theight > RAW_SOURCE_PIXELS ||
        g_cancellable_is_cancelled(cancel)) return NULL;
    *status = libraw_unpack_thumb(raw);
    if (*status || g_cancellable_is_cancelled(cancel) || thumb->tlength > RAW_PREVIEW_PIXELS * 3) return NULL;

    libraw_processed_image_t *image = libraw_dcraw_make_mem_thumb(raw, NULL);
    if (!image) return NULL;
    g_autoptr(GdkPixbuf) pixels = NULL;
    if (image->type == LIBRAW_IMAGE_JPEG) {
        g_autoptr(GdkPixbufLoader) loader = raw_jpeg_size_allowed(image->data, image->data_size) ?
            gdk_pixbuf_loader_new_with_type("jpeg", NULL) : NULL;
        if (loader) {
            g_signal_connect(loader, "size-prepared", G_CALLBACK(raw_preview_size), NULL);
            gboolean written = gdk_pixbuf_loader_write(loader, image->data, image->data_size, NULL);
            if (gdk_pixbuf_loader_close(loader, NULL) && written) {
                GdkPixbuf *decoded = gdk_pixbuf_loader_get_pixbuf(loader);
                if (decoded && gdk_pixbuf_get_width(decoded) <= RAW_PREVIEW_EDGE &&
                    gdk_pixbuf_get_height(decoded) <= RAW_PREVIEW_EDGE) pixels = g_object_ref(decoded);
            }
        }
        libraw_dcraw_clear_mem(image);
    } else {
        pixels = raw_bitmap(image);
        if (!pixels) libraw_dcraw_clear_mem(image);
    }
    if (!pixels) return NULL;

    /* LibRaw adds orientation to JPEG previews that lack EXIF. Bitmap previews
     * have no orientation tag, so use the camera's flip in that case. */
    int flip = raw->sizes.flip;
    if (!gdk_pixbuf_get_option(pixels, "orientation") && flip > 0 && flip < 8) {
        char orientation[] = {"12435867"[flip], '\0'};
        gdk_pixbuf_set_option(pixels, "orientation", orientation);
    }
    return gdk_pixbuf_apply_embedded_orientation(pixels);
}

static GdkPixbuf *raw_quick_preview(libraw_data_t *raw, int *status) {
    gsize sensor_pixels = (gsize)raw->sizes.raw_width * raw->sizes.raw_height;
    if (!sensor_pixels) {
        *status = LIBRAW_DATA_ERROR;
        return NULL;
    }
    if (sensor_pixels > (raw->idata.filters ? RAW_SOURCE_PIXELS : RAW_PREVIEW_PIXELS)) {
        *status = LIBRAW_TOO_BIG;
        return NULL;
    }
    raw->params.half_size = 1;
    raw->params.user_qual = 0;
    raw->params.use_camera_wb = 1;
    raw->params.output_color = 1; /* sRGB */
    raw->params.output_bps = 8;
    *status = libraw_unpack(raw);
    if (!*status) *status = libraw_dcraw_process(raw);
    if (*status) return NULL;

    libraw_processed_image_t *image = libraw_dcraw_make_mem_image(raw, status);
    if (!image) return NULL;
    GdkPixbuf *pixels = raw_bitmap(image);
    if (!pixels) {
        libraw_dcraw_clear_mem(image);
        *status = LIBRAW_UNSUPPORTED_THUMBNAIL;
    }
    return pixels;
}

static GByteArray *raw_read_stream(GFile *file, GCancellable *cancel, GError **error) {
    g_autoptr(GFileInputStream) input = g_file_read(file, cancel, error);
    if (!input) return NULL;
    g_autoptr(GByteArray) buffer = g_byte_array_new();
    guchar chunk[65536];
    for (;;) {
        gssize n = g_input_stream_read(G_INPUT_STREAM(input), chunk, sizeof(chunk), cancel, error);
        if (n < 0) return NULL;
        if (!n) return g_steal_pointer(&buffer);
        if ((gsize)n > RAW_FILE_LIMIT - buffer->len) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This RAW image exceeds the 256 MiB file limit.");
            return NULL;
        }
        g_byte_array_append(buffer, chunk, (guint)n);
    }
}
static int raw_open(libraw_data_t *raw, const char *path, GByteArray *buffer) {
    return path ? libraw_open_file(raw, path) : libraw_open_buffer(raw, buffer->data, buffer->len);
}

static GdkPixbuf *raw_decode(GTask *task, GFile *file, GFileInfo *file_info, RawInfo *info,
                             GCancellable *cancel, GError **error) {
    if (g_cancellable_set_error_if_cancelled(cancel, error)) return NULL;
    if (g_file_info_get_size(file_info) > RAW_FILE_LIMIT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This RAW image exceeds the 256 MiB file limit.");
        return NULL;
    }
    if (g_file_info_get_file_type(file_info) != G_FILE_TYPE_REGULAR) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE, "Open a regular RAW image file.");
        return NULL;
    }
    g_autofree char *path = g_file_get_path(file);
    g_autoptr(GByteArray) buffer = path ? NULL : raw_read_stream(file, cancel, error);
    if (!path && !buffer) return NULL;

    libraw_data_t *raw = libraw_init(LIBRAW_OPIONS_NO_DATAERR_CALLBACK);
    if (!raw) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not initialize the RAW decoder.");
        return NULL;
    }
    libraw_set_progress_handler(raw, raw_progress, cancel);
#if LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0, 21)
    raw->rawparams.max_raw_memory_mb = 256;
#else
    raw->params.max_raw_memory_mb = 256;
#endif
    int status = raw_open(raw, path, buffer);
    GdkPixbuf *pixels = NULL;
    if (!status && !g_cancellable_is_cancelled(cancel)) {
        gboolean rotated = raw->sizes.flip & 4;
        info->width = rotated ? raw->sizes.height : raw->sizes.width;
        info->height = rotated ? raw->sizes.width : raw->sizes.height;
        g_snprintf(info->make, sizeof(info->make), "%.*s", (int)sizeof(raw->idata.make), raw->idata.make);
        g_snprintf(info->model, sizeof(info->model), "%.*s", (int)sizeof(raw->idata.model), raw->idata.model);
        int thumbnail_status = LIBRAW_SUCCESS;
        pixels = raw_embedded_preview(raw, cancel, &thumbnail_status);
        info->embedded = pixels != NULL;
        /* LibRaw can recycle its input when a damaged preview fails to unpack. */
        if (LIBRAW_FATAL_ERROR(thumbnail_status) && !g_cancellable_is_cancelled(cancel))
            status = raw_open(raw, path, buffer);
        if (pixels && MAX(gdk_pixbuf_get_width(pixels), gdk_pixbuf_get_height(pixels)) < 640) {
            publish_thumbnail(task, pixels);
            GdkPixbuf *larger = g_cancellable_is_cancelled(cancel) ? NULL : raw_quick_preview(raw, &status);
            if (larger) {
                g_object_unref(pixels);
                pixels = larger;
                info->embedded = FALSE;
            }
        } else if (!pixels && !status && !g_cancellable_is_cancelled(cancel)) pixels = raw_quick_preview(raw, &status);
    }
    libraw_close(raw);
    if (g_cancellable_set_error_if_cancelled(cancel, error)) g_clear_object(&pixels);
    else if (!pixels) g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "Could not open a preview for this RAW image (%s).", libraw_strerror(status));
    return pixels;
}
