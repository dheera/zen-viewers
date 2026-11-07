/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZEN_MAP_TILES_H
#define ZEN_MAP_TILES_H
#ifndef GDK_VERSION_MIN_REQUIRED
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_6
#endif
#include <gtk/gtk.h>
typedef struct ZenTiles ZenTiles;
typedef void (*ZenTilesChanged)(gpointer data, const char *error);
ZenTiles *zen_tiles_new(ZenTilesChanged changed, gpointer data);
void zen_tiles_free(ZenTiles *tiles);
void zen_tiles_begin(ZenTiles *tiles);
/* Snapshot the exact tile, or reproject already cached levels underneath it.
 * Fallback levels never cause network requests. Priority is distance to center. */
void zen_tiles_snapshot(ZenTiles *tiles, GtkSnapshot *snapshot, int zoom, int x, int y,
                        const graphene_rect_t *bounds, double priority);
void zen_tiles_lookahead(ZenTiles *tiles, int zoom, int x, int y, double priority);
void zen_tiles_end(ZenTiles *tiles, gboolean enabled, gboolean settled);
gboolean zen_tiles_busy(ZenTiles *tiles);
#endif
