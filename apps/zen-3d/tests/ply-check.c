/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ply.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GCancellable *cancel;
    int cancel_at;
    unsigned calls, last, previews, preview_vertices;
    gboolean returned;
} Feedback;

static void validate(const Zen3dModel *model) {
    g_assert_nonnull(model);
    g_assert_cmpuint(model->vertex_count, >, 0);
    for (uint32_t i = 0; i < model->vertex_count; i++) {
        double radius = 0;
        for (int a = 0; a < 3; a++) {
            g_assert_true(isfinite(model->vertices[i].position[a]));
            g_assert_true(isfinite(model->vertices[i].normal[a]));
            radius += (double)model->vertices[i].position[a] * model->vertices[i].position[a];
        }
        g_assert_cmpfloat(radius, <=, 1.00001);
    }
    for (uint32_t i = 0; i < model->index_count; i++)
        g_assert_cmpuint(model->indices[i], <, model->vertex_count);
    for (uint32_t i = 0; i < model->part_count; i++) {
        g_assert_cmpuint(model->parts[i].first_index + model->parts[i].index_count, <=, model->index_count);
        g_assert_cmpuint(model->parts[i].material, <, model->material_count);
    }
}

static void progress(unsigned percent, gpointer data) {
    Feedback *f = data;
    g_assert_false(f->returned);
    g_assert_cmpuint(percent, <=, 90);
    if (f->calls) g_assert_cmpuint(percent, >=, f->last);
    f->last = percent;
    f->calls++;
    if (f->cancel_at >= 0 && percent >= (unsigned)f->cancel_at)
        g_cancellable_cancel(f->cancel);
}

static void preview(Zen3dModel *model, gpointer data) {
    Feedback *f = data;
    g_assert_false(f->returned);
    validate(model);
    g_assert_cmpuint(model->vertex_count, <=, 65536);
    g_assert_cmpuint(model->index_count, ==, model->vertex_count);
    g_assert_cmpuint(model->part_count, ==, 1);
    g_assert_cmpuint(model->parts[0].primitive, ==, ZEN3D_POINTS);
    f->previews++;
    f->preview_vertices = model->vertex_count;
    /* Ownership is transferred to the callback, including on later failure. */
    zen3d_model_free(model);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    g_autoptr(GFile) file = g_file_new_for_path(argv[2]);
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    Feedback f = {.cancel=cancel, .cancel_at=-1};
    if (!strcmp(argv[1], "cancel")) g_cancellable_cancel(cancel);
    if (!strcmp(argv[1], "cancel-progress")) f.cancel_at = 40;
    const Zen3dLoadFeedback callbacks = {progress, preview, &f};
    gboolean handled = TRUE;
    GError *error = NULL;
    Zen3dModel *model = !strcmp(argv[1], "fallback") ? zen3d_model_load(file, cancel, &error) :
        zen3d_ply_load(file, cancel, &handled, &callbacks, &error);
    f.returned = TRUE;
    if (!handled) {
        g_assert_null(model); g_assert_no_error(error);
        g_print("{\"handled\":false,\"previews\":%u,\"progress_calls\":%u}\n", f.previews, f.calls);
        return 0;
    }
    if (!model) {
        g_assert_nonnull(error);
        g_print("{\"handled\":true,\"cancelled\":%s,\"progress_calls\":%u,\"previews\":%u}\n",
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? "true" : "false", f.calls, f.previews);
        g_printerr("%s\n", error->message);
        g_error_free(error);
        return 1;
    }
    g_assert_no_error(error);
    validate(model);
    g_print("{\"handled\":true,\"vertices\":%u,\"indices\":%u,\"progress_calls\":%u,"
        "\"progress_last\":%u,\"previews\":%u,\"preview_vertices\":%u,\"dimensions\":[%.9g,%.9g,%.9g],\"parts\":[",
        model->vertex_count, model->index_count, f.calls, f.last, f.previews, f.preview_vertices,
        model->dimensions[0], model->dimensions[1], model->dimensions[2]);
    for (uint32_t i = 0; i < model->part_count; i++) g_print("%s%u", i ? "," : "", model->parts[i].primitive);
    g_print("],\"data\":[");
    for (uint32_t i = 0; i < MIN(model->vertex_count, 32); i++) {
        const Zen3dVertex *v = &model->vertices[i];
        g_print("%s[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u]", i ? "," : "",
            v->position[0],v->position[1],v->position[2],v->normal[0],v->normal[1],v->normal[2],
            v->color[0],v->color[1],v->color[2],v->color[3]);
    }
    g_print("],\"index_data\":[");
    for (uint32_t i = 0; i < MIN(model->index_count, 32); i++) g_print("%s%u", i ? "," : "", model->indices[i]);
    g_print("]}\n");
    zen3d_model_free(model);
    return 0;
}
