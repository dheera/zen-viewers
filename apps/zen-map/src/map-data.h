/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZEN_MAP_DATA_H
#define ZEN_MAP_DATA_H
#include <gio/gio.h>

typedef struct { double x, y; gboolean move; } MapPoint;
typedef struct {
    GArray *points;
    char *name;
    gboolean polygon;
    double x0, y0, x1, y1;
} MapShape;
typedef struct {
    GPtrArray *shapes;
    guint points;
    double x0, y0, x1, y1, anchor;
} MapData;

MapData *map_data_load(GFile *file, GCancellable *cancel, GError **error);
MapData *map_data_parse(const char *contents, gsize size, GCancellable *cancel, GError **error);
void map_data_free(MapData *data);
double map_latitude_y(double latitude);
double map_y_latitude(double y);
#endif
