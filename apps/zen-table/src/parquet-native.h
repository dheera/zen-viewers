/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZEN_PARQUET_NATIVE_H
#define ZEN_PARQUET_NATIVE_H
#include "table-data.h"
typedef struct ZtParquet ZtParquet;
ZtParquet *zt_parquet_open(const char *path, GCancellable *cancel, GError **error);
void zt_parquet_free(ZtParquet *parquet);
void zt_parquet_cancel(ZtParquet *parquet);
guint zt_parquet_rows(ZtParquet *parquet);
guint zt_parquet_columns(ZtParquet *parquet);
const char *zt_parquet_header(ZtParquet *parquet, guint column);
const char *zt_parquet_type(ZtParquet *parquet, guint column);
ZtPage *zt_parquet_page(ZtParquet *parquet, guint start, guint count, GCancellable *cancel, GError **error);
gint64 zt_parquet_find(ZtParquet *parquet, guint start, const char *text, GCancellable *cancel, GError **error);
#endif
