/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <stdint.h>

typedef struct {
    float position[3], normal[3], uv[2];
    uint8_t color[4];
} Zen3dVertex;

typedef enum {
    ZEN3D_TRIANGLES,
    ZEN3D_LINES,
    ZEN3D_POINTS
} Zen3dPrimitive;

typedef struct {
    uint32_t first_index, index_count, material;
    Zen3dPrimitive primitive;
} Zen3dPart;

typedef struct {
    float color[4];
    /* Optional sRGB base-color image. UV (0, 0) refers to its first row. */
    GdkPixbuf *texture;
} Zen3dMaterial;

typedef struct {
    Zen3dVertex *vertices;
    uint32_t *indices;
    /* Optional lower-detail triangles using the same vertices. */
    uint32_t *preview_indices;
    Zen3dPart *parts;
    Zen3dMaterial *materials;
    uint32_t vertex_count, index_count, preview_index_count, part_count, material_count;
    /* Positions are centered and fit in a unit sphere. Sizes use source units. */
    float bounds_min[3], bounds_max[3];
    double dimensions[3];
} Zen3dModel;

/* Callbacks run on the loading thread. The preview callback owns the model. */
typedef struct {
    void (*progress)(unsigned percent, gpointer data);
    void (*preview)(Zen3dModel *model, gpointer data);
    gpointer data;
} Zen3dLoadFeedback;

Zen3dModel *zen3d_model_load(GFile *file, GCancellable *cancel, GError **error);
Zen3dModel *zen3d_model_load_with_feedback(GFile *file, GCancellable *cancel,
                                          const Zen3dLoadFeedback *feedback, GError **error);
void zen3d_model_free(Zen3dModel *model);
/* Semicolon-separated file globs supported by the installed importer. */
char *zen3d_model_formats(void);
