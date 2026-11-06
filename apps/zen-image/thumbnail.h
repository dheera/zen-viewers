/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Read-only support for the freedesktop per-user thumbnail cache:
 * https://specifications.freedesktop.org/thumbnail/latest/
 * No source image bytes, shared repositories, legacy caches, or failed entries
 * are read. NULL is a cache miss (including cancellation or corrupt input).
 */

static inline gboolean zen_thumbnail_cancelled(GCancellable *cancellable) {
    return cancellable && g_cancellable_is_cancelled(cancellable);
}

static inline gboolean zen_thumbnail_decimal(const char *text, gboolean signed_value) {
    if (!text || !*text) return FALSE;
    if (signed_value && *text == '-') text++;
    if (!*text) return FALSE;
    for (; *text; text++) if (*text < '0' || *text > '9') return FALSE;
    return TRUE;
}

static inline gboolean zen_thumbnail_matches(GdkPixbuf *pixels, const char *uri,
                                             GFileInfo *source) {
    const char *saved_uri = gdk_pixbuf_get_option(pixels, "tEXt::Thumb::URI");
    const char *saved_time = gdk_pixbuf_get_option(pixels, "tEXt::Thumb::MTime");
    const char *saved_size = gdk_pixbuf_get_option(pixels, "tEXt::Thumb::Size");
    if (!saved_uri || strcmp(saved_uri, uri) || !zen_thumbnail_decimal(saved_time, TRUE)) return FALSE;
    errno = 0;
    gint64 time = g_ascii_strtoll(saved_time, NULL, 10);
    if (errno || time != (gint64)g_file_info_get_attribute_uint64(source, G_FILE_ATTRIBUTE_TIME_MODIFIED)) return FALSE;
    if (saved_size) {
        if (!zen_thumbnail_decimal(saved_size, FALSE) ||
            !g_file_info_has_attribute(source, G_FILE_ATTRIBUTE_STANDARD_SIZE)) return FALSE;
        errno = 0;
        guint64 size = g_ascii_strtoull(saved_size, NULL, 10);
        if (errno || size != (guint64)g_file_info_get_size(source)) return FALSE;
    }
    return TRUE;
}

static inline guint32 zen_thumbnail_big_endian(const guchar *bytes) {
    guint32 value;
    memcpy(&value, bytes, sizeof(value));
    return GUINT32_FROM_BE(value);
}

static inline gssize zen_thumbnail_read(int fd, void *buffer, gsize size,
                                        GCancellable *cancellable) {
    for (;;) {
        if (zen_thumbnail_cancelled(cancellable)) return -1;
        ssize_t count = read(fd, buffer, size);
        if (count >= 0 || errno != EINTR) return (gssize)count;
    }
}

static inline GdkPixbuf *zen_thumbnail_decode(const char *path, unsigned edge,
                                              GCancellable *cancellable) {
    const gsize byte_limit = 8u * 1024 * 1024;
    if (zen_thumbnail_cancelled(cancellable)) return NULL;
    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = g_open(path, flags, 0);
    if (fd < 0) return NULL;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    GdkPixbufLoader *loader = NULL;
    GdkPixbuf *result = NULL;
    GError *error = NULL;
    struct stat status;
    guchar header[33];
    if (fstat(fd, &status) || !S_ISREG(status.st_mode) || status.st_size < (off_t)sizeof(header) ||
        (guint64)status.st_size > byte_limit ||
        zen_thumbnail_read(fd, header, sizeof(header), cancellable) != sizeof(header)) goto done;
    /* Check IHDR before invoking a decoder: even a compressed cache file must
     * fit its advertised bucket, with at most 1024x1024 decoded pixels. */
    if (memcmp(header, "\211PNG\r\n\032\n", 8) || zen_thumbnail_big_endian(header+8) != 13 ||
        memcmp(header+12, "IHDR", 4)) goto done;
    guint32 width = zen_thumbnail_big_endian(header+16), height = zen_thumbnail_big_endian(header+20);
    if (!width || !height || width > edge || height > edge || header[24] != 8 || header[28] != 0) goto done;
    loader = gdk_pixbuf_loader_new_with_type("png", &error);
    if (!loader) goto done;
    gboolean okay = gdk_pixbuf_loader_write(loader, header, sizeof(header), &error);
    gsize total = sizeof(header);
    guchar buffer[32768];
    while (okay) {
        gssize count = zen_thumbnail_read(fd, buffer, sizeof(buffer), cancellable);
        if (count < 0) { okay = FALSE; break; }
        if (!count) break;
        if ((gsize)count > byte_limit-total || total+(gsize)count > (guint64)status.st_size) { okay = FALSE; break; }
        total += (gsize)count;
        okay = gdk_pixbuf_loader_write(loader, buffer, (gsize)count, &error);
    }
    g_clear_error(&error);
    if (!gdk_pixbuf_loader_close(loader, &error)) okay = FALSE;
    GdkPixbuf *pixels = gdk_pixbuf_loader_get_pixbuf(loader);
    if (okay && total == (guint64)status.st_size && pixels && !zen_thumbnail_cancelled(cancellable) &&
        gdk_pixbuf_get_width(pixels) <= (int)edge && gdk_pixbuf_get_height(pixels) <= (int)edge)
        result = g_object_ref(pixels);
done:
    g_clear_error(&error); g_clear_object(&loader); close(fd);
    return result;
}

static inline gboolean zen_thumbnail_readable(GFileInfo *info) {
    return info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR &&
        g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED) &&
        g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ) &&
        g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);
}

/* Worker-callable. Returns a new reference; the caller owns the returned pixels.
 * Freedesktop thumbnails already have source orientation applied to their pixels.
 * Use their own dimensions for preview layout, not optional original-image tags.
 */
static inline GdkPixbuf *load_cached_thumbnail(GFile *file, GCancellable *cancellable) {
    if (zen_thumbnail_cancelled(cancellable)) return NULL;
    char *native_path = g_file_get_path(file);
    if (!native_path) return NULL;
    g_free(native_path);
    const char *attributes = G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE ","
        G_FILE_ATTRIBUTE_ACCESS_CAN_READ "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC;
    GFileInfo *source = g_file_query_info(file, attributes, G_FILE_QUERY_INFO_NONE, cancellable, NULL);
    if (!zen_thumbnail_readable(source)) { g_clear_object(&source); return NULL; }
    char *uri = g_file_get_uri(file);
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    char *name = g_strconcat(digest, ".png", NULL); g_free(digest);
    const char *cache = g_get_user_cache_dir();
    char *fallback = !g_path_is_absolute(cache) ? g_build_filename(g_get_home_dir(), ".cache", NULL) : NULL;
    if (fallback) cache = fallback;
    const struct { const char *directory; unsigned edge; } sizes[] = {
        {"xx-large", 1024}, {"x-large", 512}, {"large", 256}, {"normal", 128}
    };
    GdkPixbuf *result = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(sizes) && !zen_thumbnail_cancelled(cancellable); i++) {
        char *path = g_build_filename(cache, "thumbnails", sizes[i].directory, name, NULL);
        GdkPixbuf *pixels = zen_thumbnail_decode(path, sizes[i].edge, cancellable); g_free(path);
        if (!pixels) continue;
        if (!zen_thumbnail_matches(pixels, uri, source)) { g_object_unref(pixels); continue; }
        /* Do not flash a cache entry if the original changed during decoding. */
        GFileInfo *current = g_file_query_info(file, attributes, G_FILE_QUERY_INFO_NONE, cancellable, NULL);
        gboolean unchanged = zen_thumbnail_readable(current) &&
            g_file_info_get_size(current) == g_file_info_get_size(source) &&
            g_file_info_get_attribute_uint64(current, G_FILE_ATTRIBUTE_TIME_MODIFIED) ==
                g_file_info_get_attribute_uint64(source, G_FILE_ATTRIBUTE_TIME_MODIFIED) &&
            g_file_info_get_attribute_uint32(current, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC) ==
                g_file_info_get_attribute_uint32(source, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
        g_clear_object(&current);
        if (unchanged && !zen_thumbnail_cancelled(cancellable)) {
            gdk_pixbuf_remove_option(pixels, "orientation");
            result = pixels;
        } else g_object_unref(pixels);
        break;
    }
    g_free(fallback); g_free(name); g_free(uri); g_object_unref(source);
    return result;
}
