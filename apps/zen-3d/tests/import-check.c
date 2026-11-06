/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "model.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { const char *path; gboolean succeeds; } ThreadLoad;
static gpointer thread_load(gpointer data) {
    ThreadLoad *task = data;
    GFile *file = g_file_new_for_path(task->path);
    char *name = g_path_get_basename(task->path);
    for (int i = 0; i < 12; i++) {
        GError *error = NULL;
        Zen3dModel *model = zen3d_model_load(file, NULL, &error);
        if (task->succeeds) { g_assert_nonnull(model); g_assert_no_error(error); }
        else { g_assert_null(model); g_assert_nonnull(error); g_assert_nonnull(strstr(error->message, name)); }
        zen3d_model_free(model); g_clear_error(&error);
        char *formats = zen3d_model_formats();
        g_assert_nonnull(strstr(formats, "*.obj")); g_free(formats);
    }
    g_free(name); g_object_unref(file); return NULL;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--concurrent")) {
        char *first = g_strconcat(argv[2], ".missing-one.obj", NULL);
        char *second = g_strconcat(argv[2], ".missing-two.obj", NULL);
        ThreadLoad tasks[] = {{argv[2], TRUE}, {first, FALSE}, {second, FALSE}, {argv[2], TRUE}};
        GThread *threads[G_N_ELEMENTS(tasks)];
        for (guint i = 0; i < G_N_ELEMENTS(tasks); i++) threads[i] = g_thread_new("import-test", thread_load, &tasks[i]);
        for (guint i = 0; i < G_N_ELEMENTS(tasks); i++) g_thread_join(threads[i]);
        g_free(first); g_free(second); return 0;
    }
    if (argc != 2) return 2;
    GFile *file = g_file_new_for_path(argv[1]);
    GCancellable *cancel = g_cancellable_new();
    g_cancellable_cancel(cancel);
    GError *error = NULL;
    Zen3dModel *model = zen3d_model_load(file, cancel, &error);
    g_assert_null(model);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error); g_object_unref(cancel);
    model = zen3d_model_load(file, NULL, &error);
    g_object_unref(file);
    if (!model) { g_printerr("%s\n", error ? error->message : "No model"); g_clear_error(&error); return 1; }
    guint primitives[3] = {0, 0, 0}, textures = 0;
    for (guint p = 0; p < model->part_count; p++) {
        const Zen3dPart *part = &model->parts[p];
        g_assert_cmpuint(part->primitive, <=, ZEN3D_POINTS);
        g_assert_cmpuint(part->material, <, model->material_count);
        g_assert_cmpuint(part->first_index + part->index_count, <=, model->index_count);
        primitives[part->primitive]++;
    }
    for (guint v = 0; v < model->vertex_count; v++) {
        const Zen3dVertex *vertex = &model->vertices[v];
        double radius = 0, normal = 0;
        for (int a = 0; a < 3; a++) {
            g_assert_true(isfinite(vertex->position[a]) && isfinite(vertex->normal[a]));
            radius += vertex->position[a] * vertex->position[a];
            normal += vertex->normal[a] * vertex->normal[a];
        }
        g_assert_cmpfloat(radius, <=, 1.00001);
        g_assert_cmpfloat(fabs(normal - 1), <, .00001);
    }
    for (guint i = 0; i < model->index_count; i++) g_assert_cmpuint(model->indices[i], <, model->vertex_count);
    for (guint m = 0; m < model->material_count; m++) if (model->materials[m].texture) {
        g_assert_true(gdk_pixbuf_get_has_alpha(model->materials[m].texture)); textures++;
    }
    g_print("{\"vertices\":%u,\"indices\":%u,\"parts\":[%u,%u,%u],\"textures\":%u,\"dimensions\":[%.9g,%.9g,%.9g],\"data\":[",
        model->vertex_count, model->index_count, primitives[0], primitives[1], primitives[2], textures,
        model->dimensions[0], model->dimensions[1], model->dimensions[2]);
    for (guint v = 0; v < MIN(32, model->vertex_count); v++) {
        const Zen3dVertex *p = &model->vertices[v];
        g_print("%s[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u]", v ? "," : "",
            p->position[0], p->position[1], p->position[2], p->normal[0], p->normal[1], p->normal[2],
            p->uv[0], p->uv[1], p->color[0], p->color[1], p->color[2], p->color[3]);
    }
    g_print("],\"materials\":[");
    for (guint m = 0; m < model->material_count; m++) {
        Zen3dMaterial *material = &model->materials[m];
        g_print("%s[%.9g,%.9g,%.9g,%.9g]", m ? "," : "", material->color[0], material->color[1], material->color[2], material->color[3]);
    }
    g_print("],\"index_data\":[");
    for (guint i = 0; i < MIN(32, model->index_count); i++) g_print("%s%u", i ? "," : "", model->indices[i]);
    g_print("]}\n");
    zen3d_model_free(model);
    return 0;
}
