/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "table-data.h"
#include "parquet-native.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#define MAX_RECORD (64 * 1024 * 1024)
#define MAX_CELL (16 * 1024 * 1024)
#define MAX_PAGE (16 * 1024 * 1024)

typedef struct { guint64 start, length; } Record;
struct ZtSource {
    gint refs;
    int fd;
    char delimiter;
    GArray *records;
    char **headers, **types;
    guint columns, rows;
    gboolean parquet;
    ZtParquet *native;
};
static gboolean fail(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}
ZtSource *zt_source_ref(ZtSource *s) { g_atomic_int_inc(&s->refs); return s; }
void zt_source_unref(ZtSource *s) {
    if (!s || !g_atomic_int_dec_and_test(&s->refs)) return;
    zt_parquet_free(s->native);
    if (s->fd >= 0) close(s->fd);
    g_clear_pointer(&s->records, g_array_unref);
    g_strfreev(s->headers); g_strfreev(s->types); g_free(s);
}
void zt_source_cancel(ZtSource *s) { if (s) zt_parquet_cancel(s->native); }
guint zt_source_rows(ZtSource *s) { return s->rows; }
guint zt_source_columns(ZtSource *s) { return s->columns; }
const char *zt_source_header(ZtSource *s, guint i) { return i < s->columns ? s->headers[i] : ""; }
const char *zt_source_type(ZtSource *s, guint i) { return s->types && i < s->columns ? s->types[i] : "text"; }
gboolean zt_source_is_parquet(ZtSource *s) { return s->parquet; }
static ZtSource *source_new(void) {
    ZtSource *s = g_new0(ZtSource, 1); s->refs = 1; s->fd = -1; return s;
}
static char **parse_record(const char *data, gsize length, char delimiter, GError **error) {
    g_autoptr(GPtrArray) fields = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GString) value = g_string_new(NULL);
    gboolean quoted = FALSE, at_start = TRUE;
    for (gsize i = 0; i <= length; i++) {
        char c = i < length ? data[i] : delimiter;
        if (quoted) {
            if (c == '"') {
                if (i + 1 < length && data[i + 1] == '"') { g_string_append_c(value, '"'); i++; }
                else quoted = FALSE;
            } else if (i < length) g_string_append_c(value, c);
        } else if (c == delimiter) {
            if (fields->len >= ZT_MAX_COLUMNS) { fail(error, "This table has more than 512 columns."); return NULL; }
            if (value->len > MAX_CELL) { fail(error, "A cell exceeds the 16 MiB display limit."); return NULL; }
            /* UTF-8 replacement keeps malformed exported text safe for GTK. */
            g_ptr_array_add(fields, g_utf8_make_valid(value->str, value->len));
            g_string_truncate(value, 0); at_start = TRUE;
        } else if (c == '"' && at_start) { quoted = TRUE; at_start = FALSE; }
        else { if (i < length) g_string_append_c(value, c); at_start = FALSE; }
    }
    if (quoted) { fail(error, "Unclosed quoted field in the delimited file."); return NULL; }
    g_ptr_array_add(fields, NULL);
    return (char **)g_ptr_array_free(g_steal_pointer(&fields), FALSE);
}
static gboolean read_at(int fd, guint64 offset, char *buffer, gsize count, GError **error) {
    while (count) {
        ssize_t n = pread(fd, buffer, count, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", n < 0 ? g_strerror(errno) : "The file changed while it was being read.");
            return FALSE;
        }
        offset += n; buffer += n; count -= n;
    }
    return TRUE;
}
static char **csv_row(ZtSource *s, guint index, GError **error) {
    if (index >= s->records->len) return NULL;
    Record record = g_array_index(s->records, Record, index);
    g_autofree char *buffer = g_malloc(record.length + 1);
    if (!read_at(s->fd, record.start, buffer, record.length, error)) return NULL;
    buffer[record.length] = 0;
    char **values = parse_record(buffer, record.length, s->delimiter, error);
    if (!values) return NULL;
    guint count = g_strv_length(values);
    if (count < s->columns) {
        values = g_realloc_n(values, s->columns + 1, sizeof(char *));
        for (guint i = count; i < s->columns; i++) values[i] = g_strdup("");
        values[s->columns] = NULL;
    }
    return values;
}
static ZtSource *preview_copy(ZtSource *s) {
    ZtSource *p = source_new(); p->fd = dup(s->fd); p->delimiter = s->delimiter;
    p->columns = s->columns; p->rows = s->records->len - 1;
    p->headers = g_strdupv(s->headers);
    p->records = g_array_sized_new(FALSE, FALSE, sizeof(Record), s->records->len);
    g_array_append_vals(p->records, s->records->data, s->records->len);
    return p;
}
static gboolean add_record(ZtSource *s, guint64 start, guint64 length, guint columns, GError **error) {
    if (length > MAX_RECORD) return fail(error, "A record exceeds the 64 MiB display limit.");
    if (columns > ZT_MAX_COLUMNS) return fail(error, "This table has more than 512 columns.");
    if (s->records->len == G_MAXUINT) return fail(error, "This table has too many rows to index.");
    Record record = {start, length}; g_array_append_val(s->records, record);
    if (s->records->len == 1) {
        s->headers = csv_row(s, 0, error);
        if (!s->headers) return FALSE;
        s->columns = g_strv_length(s->headers);
        for (guint i = 0; i < s->columns; i++) if (!*s->headers[i]) {
            g_free(s->headers[i]); s->headers[i] = g_strdup_printf("Column %u", i + 1);
        }
    }
    if (columns > s->columns) {
        s->headers = g_realloc_n(s->headers, columns + 1, sizeof(char *));
        for (guint i = s->columns; i < columns; i++) s->headers[i] = g_strdup_printf("Column %u", i + 1);
        s->headers[columns] = NULL; s->columns = columns;
    }
    return TRUE;
}
static gboolean load_csv(ZtSource *s, const char *path, GCancellable *cancel,
                         ZtProgress progress, gpointer data, GError **error) {
    s->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (s->fd < 0) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", g_strerror(errno)); return FALSE; }
    struct stat st;
    if (fstat(s->fd, &st) || !S_ISREG(st.st_mode)) return fail(error, "Open a regular CSV, TSV, or Parquet file.");
    if (!st.st_size) return fail(error, "This table is empty.");
    s->records = g_array_new(FALSE, FALSE, sizeof(Record));
    g_autofree char *lower = g_ascii_strdown(path, -1);
    s->delimiter = g_str_has_suffix(lower, ".tsv") || g_str_has_suffix(lower, ".tab") ? '\t' : ',';
    char buffer[65536];
    guint64 position = 0, start = 0;
    guint columns = 1;
    enum {FIELD_START, FIELD, QUOTED, AFTER_QUOTE} state = FIELD_START;
    gboolean cr = FALSE, preview_sent = FALSE;
    gint64 last_progress = 0;
    /* UTF-8 BOM belongs to the encoding, not the first header. */
    if (st.st_size >= 3 && read_at(s->fd, 0, buffer, 3, NULL) && !memcmp(buffer, "\xef\xbb\xbf", 3)) start = position = 3;
    while (position < (guint64)st.st_size) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) return FALSE;
        gsize count = MIN(sizeof(buffer), (guint64)st.st_size - position);
        if (!read_at(s->fd, position, buffer, count, error)) return FALSE;
        for (gsize i = 0; i < count; i++) {
            char c = buffer[i]; guint64 offset = position + i;
            if (!c) return fail(error, "The file is not UTF-8 text. Convert UTF-16 exports to UTF-8 first.");
            if (cr && c == '\n') { start = offset + 1; cr = FALSE; continue; }
            cr = FALSE;
            if (state == QUOTED) { if (c == '"') state = AFTER_QUOTE; }
            else if (state == AFTER_QUOTE && c == '"') state = QUOTED;
            else if (c == '\r' || c == '\n') {
                if (!add_record(s, start, offset - start, columns, error)) return FALSE;
                start = offset + 1; columns = 1; state = FIELD_START; cr = c == '\r';
                if (!preview_sent && s->records->len == 513 && progress) {
                    ZtSource *preview = preview_copy(s); progress(preview, (double)offset / st.st_size, data); zt_source_unref(preview); preview_sent = TRUE;
                }
            } else if (c == s->delimiter) { columns++; state = FIELD_START; }
            else if (state == FIELD_START && c == '"') state = QUOTED;
            else state = FIELD;
            if (offset >= start && offset - start > MAX_RECORD) return fail(error, "A record exceeds the 64 MiB display limit.");
        }
        position += count;
        gint64 now = g_get_monotonic_time();
        if (progress && now - last_progress > 150000) { progress(NULL, (double)position / st.st_size, data); last_progress = now; }
    }
    if (state == QUOTED) return fail(error, "Unclosed quoted field in the delimited file.");
    if (start < position && !add_record(s, start, position - start, columns, error)) return FALSE;
    if (!s->records->len) return fail(error, "This table has no records.");
    s->rows = s->records->len - 1; return TRUE;
}
static gboolean load_parquet(ZtSource *s, const char *path, GCancellable *cancel, GError **error) {
    s->native = zt_parquet_open(path, cancel, error);
    if (!s->native) return FALSE;
    s->parquet = TRUE; s->rows = zt_parquet_rows(s->native); s->columns = zt_parquet_columns(s->native);
    s->headers = g_new0(char *, s->columns + 1); s->types = g_new0(char *, s->columns + 1);
    for (guint i = 0; i < s->columns; i++) {
        s->headers[i] = g_strdup(zt_parquet_header(s->native, i));
        s->types[i] = g_strdup(zt_parquet_type(s->native, i));
    }
    return TRUE;
}
ZtSource *zt_source_open(const char *path, GCancellable *cancel, ZtProgress progress, gpointer data, GError **error) {
    ZtSource *s = source_new();
    g_autofree char *lower = g_ascii_strdown(path, -1);
    /* Magic also recognizes Parquet files whose extension was omitted. */
    char magic[4] = {0}; int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0) {
        struct stat info;
        if (fstat(fd, &info) || !S_ISREG(info.st_mode)) { close(fd); fail(error, "Open a regular CSV, TSV, or Parquet file."); zt_source_unref(s); return NULL; }
        ssize_t ignored = pread(fd, magic, 4, 0); (void)ignored; close(fd);
    }
    gboolean text_extension = g_str_has_suffix(lower, ".csv") || g_str_has_suffix(lower, ".tsv") || g_str_has_suffix(lower, ".tab");
    gboolean parquet = g_str_has_suffix(lower, ".parquet") || (!text_extension && !memcmp(magic, "PAR1", 4));
    gboolean ok = parquet ? load_parquet(s, path, cancel, error) : load_csv(s, path, cancel, progress, data, error);
    if (!ok) { zt_source_unref(s); return NULL; }
    return s;
}
void zt_page_free(ZtPage *page) { if (page) { g_ptr_array_unref(page->rows); g_free(page); } }
ZtPage *zt_source_page(ZtSource *s, guint start, guint count, GCancellable *cancel, GError **error) {
    if (s->parquet) return zt_parquet_page(s->native, start, count, cancel, error);
    g_autoptr(ZtPage) page = g_new0(ZtPage, 1); page->start = start;
    page->rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    if (start >= s->rows) return g_steal_pointer(&page);
    count = MIN(MIN(count, ZT_PAGE_SIZE), s->rows - start);
    gsize bytes = 0;
    for (guint i = 0; i < count; i++) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) return NULL;
        char **values = csv_row(s, start + i + 1, error); if (!values) return NULL;
        g_ptr_array_add(page->rows, values);
        for (guint j = 0; values[j]; j++) bytes += strlen(values[j]) + sizeof(char *) + 1;
        if (bytes > MAX_PAGE) { fail(error, "This page exceeds the 16 MiB display budget. Split exceptionally large cells or columns before viewing."); return NULL; }
    }
    return g_steal_pointer(&page);
}
gint64 zt_source_find(ZtSource *s, guint start, const char *text, GCancellable *cancel, GError **error) {
    start = MIN(start, s->rows);
    if (s->parquet) return zt_parquet_find(s->native, start, text, cancel, error);
    g_autofree char *needle = g_utf8_casefold(text, -1);
    for (guint64 n = 0; n < s->rows; n++) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) return -1;
        guint i = ((guint64)start + n) % s->rows;
        g_auto(GStrv) values = csv_row(s, i + 1, error); if (!values) return -1;
        for (guint j = 0; values[j]; j++) { g_autofree char *folded = g_utf8_casefold(values[j], -1); if (strstr(folded, needle)) return i; }
    }
    return -1;
}
