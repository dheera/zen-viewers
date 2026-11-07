/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZEN_TABLE_DATA_H
#define ZEN_TABLE_DATA_H
#include <gio/gio.h>
#define ZT_PAGE_SIZE 128
#define ZT_MAX_COLUMNS 512

typedef struct ZtSource ZtSource;
typedef struct { guint start; GPtrArray *rows; } ZtPage;
typedef void (*ZtProgress)(ZtSource *preview, double fraction, gpointer data);
ZtSource *zt_source_open(const char *path, GCancellable *cancel, ZtProgress progress,
                         gpointer user_data, GError **error);
ZtSource *zt_source_ref(ZtSource *source);
void zt_source_unref(ZtSource *source);
void zt_source_cancel(ZtSource *source);
guint zt_source_rows(ZtSource *source);
guint zt_source_columns(ZtSource *source);
const char *zt_source_header(ZtSource *source, guint column);
const char *zt_source_type(ZtSource *source, guint column);
gboolean zt_source_is_parquet(ZtSource *source);
ZtPage *zt_source_page(ZtSource *source, guint start, guint count, GCancellable *cancel, GError **error);
void zt_page_free(ZtPage *page);
gint64 zt_source_find(ZtSource *source, guint start, const char *text, GCancellable *cancel, GError **error);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ZtSource, zt_source_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ZtPage, zt_page_free)
#endif
