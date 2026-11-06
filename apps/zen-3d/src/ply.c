/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "ply.h"
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GEOMETRY_LIMIT ((guint64)512 * 1024 * 1024)
#define FILE_LIMIT ((guint64)256 * 1024 * 1024)
#define BLOCK_RECORDS 16384u
#define HEADER_LIMIT 65536u
#define PREVIEW_POINTS 65536u
#define PREVIEW_TRIANGLES 1500000u

G_STATIC_ASSERT(sizeof(float) == 4);

static const Zen3dMaterial scan_material = {.color = {.63f, .72f, .75f, 1.f}};

typedef struct {
    guint64 vertices, faces;
    off_t data_offset;
} PlyHeader;

typedef enum { HEADER_OTHER, HEADER_OK, HEADER_INVALID } HeaderResult;

static gboolean count_value(const char *text, guint64 *out) {
    if (!text || !*text || *text == '-') return FALSE;
    char *end;
    errno = 0; *out = g_ascii_strtoull(text, &end, 10);
    return !errno && !*end;
}

static HeaderResult read_header(FILE *stream, PlyHeader *header) {
    char line[1024];
    if (!fgets(line, sizeof(line), stream) || strcmp(g_strchomp(line), "ply")) return HEADER_OTHER;
    gboolean format = FALSE, vertices = FALSE, faces = FALSE;
    guint vertex_properties = 0, face_properties = 0;
    for (guint bytes = 4; bytes < HEADER_LIMIT;) {
        if (!fgets(line, sizeof(line), stream)) return format ? HEADER_INVALID : HEADER_OTHER;
        bytes += (guint)strlen(line);
        if (!strchr(line, '\n')) return HEADER_OTHER;
        char *tokens[8], *state = NULL;
        guint count = 0;
        for (char *token = strtok_r(line, " \t\r\n", &state); token; token = strtok_r(NULL, " \t\r\n", &state)) {
            if (count == G_N_ELEMENTS(tokens)) break;
            tokens[count++] = token;
        }
        if (!count) continue;
        if (!strcmp(tokens[0], "comment") || !strcmp(tokens[0], "obj_info")) {
            /* A texture declaration has semantics that the general importer
             * must preserve, even when no UV properties were declared. */
            if (count > 1 && !strcmp(tokens[1], "TextureFile")) return HEADER_OTHER;
            continue;
        }
        if (!strcmp(tokens[0], "format")) {
            if (format || vertices || count != 3 || strcmp(tokens[1], "binary_little_endian") || strcmp(tokens[2], "1.0"))
                return HEADER_OTHER;
            format = TRUE;
        } else if (!strcmp(tokens[0], "element")) {
            if (!format || count != 3) return HEADER_OTHER;
            if (!strcmp(tokens[1], "vertex") && !vertices && !faces) {
                if (!count_value(tokens[2], &header->vertices)) return HEADER_INVALID;
                vertices = TRUE;
            } else if (!strcmp(tokens[1], "face") && vertices && !faces && vertex_properties == 3) {
                if (!count_value(tokens[2], &header->faces)) return HEADER_INVALID;
                faces = TRUE;
            } else return HEADER_OTHER;
        } else if (!strcmp(tokens[0], "property")) {
            if (!vertices) return HEADER_OTHER;
            if (!faces) {
                const char *names[] = {"x", "y", "z"};
                if (vertex_properties >= 3 || count != 3 || (strcmp(tokens[1], "float") && strcmp(tokens[1], "float32")) ||
                    strcmp(tokens[2], names[vertex_properties])) return HEADER_OTHER;
                vertex_properties++;
            } else {
                if (face_properties || count != 5 || strcmp(tokens[1], "list") ||
                    (strcmp(tokens[2], "uchar") && strcmp(tokens[2], "uint8")) ||
                    (strcmp(tokens[3], "int") && strcmp(tokens[3], "int32") && strcmp(tokens[3], "uint") && strcmp(tokens[3], "uint32")) ||
                    (strcmp(tokens[4], "vertex_indices") && strcmp(tokens[4], "vertex_index"))) return HEADER_OTHER;
                face_properties++;
            }
        } else if (!strcmp(tokens[0], "end_header")) {
            if (count != 1 || !format || !vertices || vertex_properties != 3 || (faces && face_properties != 1)) return HEADER_OTHER;
            header->data_offset = ftello(stream);
            return header->data_offset >= 0 ? HEADER_OK : HEADER_INVALID;
        } else return HEADER_OTHER;
    }
    return HEADER_OTHER;
}

static gboolean stopped(GCancellable *cancel, GError **error) {
    return cancel && g_cancellable_set_error_if_cancelled(cancel, error);
}
static void invalid(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
}
static uint32_t little_u32(const guchar *bytes) {
    uint32_t value; memcpy(&value, bytes, sizeof(value)); return GUINT32_FROM_LE(value);
}
static float little_float(const guchar *bytes) {
    uint32_t bits = little_u32(bytes);
    float value; memcpy(&value, &bits, sizeof(value)); return value;
}

typedef struct {
    const Zen3dLoadFeedback *feedback;
    unsigned last;
} Progress;
static void progress(Progress *state, unsigned value) {
    if (state->feedback && state->feedback->progress && value != state->last) {
        state->last = value;
        state->feedback->progress(value, state->feedback->data);
    }
}

static gboolean publish_preview(const Zen3dModel *source, GCancellable *cancel,
                                 const Zen3dLoadFeedback *feedback, GError **error) {
    if (!feedback || !feedback->preview) return TRUE;
    Zen3dModel *preview = g_try_new0(Zen3dModel, 1);
    if (!preview) return TRUE; /* The optional preview must not fail the load. */
    preview->vertex_count = preview->index_count = MIN(source->vertex_count, PREVIEW_POINTS);
    preview->part_count = preview->material_count = 1;
    preview->vertices = g_try_malloc_n(preview->vertex_count, sizeof(Zen3dVertex));
    preview->indices = g_try_malloc_n(preview->index_count, sizeof(uint32_t));
    preview->parts = g_try_new0(Zen3dPart, 1); preview->materials = g_try_new0(Zen3dMaterial, 1);
    if (!preview->vertices || !preview->indices || !preview->parts || !preview->materials) {
        zen3d_model_free(preview); return TRUE;
    }
    memcpy(preview->bounds_min, source->bounds_min, sizeof(preview->bounds_min));
    memcpy(preview->bounds_max, source->bounds_max, sizeof(preview->bounds_max));
    memcpy(preview->dimensions, source->dimensions, sizeof(preview->dimensions));
    for (uint32_t i = 0; i < preview->vertex_count; i++) {
        if (!(i % BLOCK_RECORDS) && stopped(cancel, error)) { zen3d_model_free(preview); return FALSE; }
        uint32_t at = preview->vertex_count > 1 ?
            (uint32_t)((guint64)i * (source->vertex_count - 1) / (preview->vertex_count - 1)) : 0;
        preview->vertices[i] = source->vertices[at];
        preview->vertices[i].normal[2] = 1;
        preview->indices[i] = i;
    }
    preview->parts[0].index_count = preview->index_count;
    preview->parts[0].primitive = ZEN3D_POINTS;
    preview->materials[0] = scan_material;
    if (stopped(cancel, error)) { zen3d_model_free(preview); return FALSE; }
    feedback->preview(preview, feedback->data); /* Transfers ownership. */
    return TRUE;
}

Zen3dModel *zen3d_ply_load(GFile *file, GCancellable *cancel, gboolean *handled,
                         const Zen3dLoadFeedback *feedback, GError **error) {
    *handled = FALSE;
    if (stopped(cancel, error)) { *handled = TRUE; return NULL; }
    char *path = g_file_get_path(file);
    if (!path) return NULL;
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    g_free(path);
    if (descriptor < 0) return NULL;
    struct stat status;
    if (fstat(descriptor, &status) || !S_ISREG(status.st_mode)) { close(descriptor); return NULL; }
    FILE *stream = fdopen(descriptor, "rb");
    if (!stream) { close(descriptor); return NULL; }
    PlyHeader header = {0};
    HeaderResult match = read_header(stream, &header);
    Zen3dModel *model = NULL;
    if (match == HEADER_OTHER) goto done;
    *handled = TRUE;
    if (match == HEADER_INVALID) { invalid(error, "The PLY header is invalid or truncated."); goto done; }
    if ((guint64)status.st_size > FILE_LIMIT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This PLY file exceeds the 256 MiB file limit."); goto done;
    }
    if (!header.vertices) { invalid(error, "This PLY file contains no vertices."); goto done; }
    guint64 indices = header.faces ? header.faces * 3 : header.vertices;
    if (header.vertices > UINT32_MAX || header.faces > UINT32_MAX / 3 ||
        header.vertices * sizeof(Zen3dVertex) + indices * sizeof(uint32_t) + sizeof(Zen3dPart) + sizeof(Zen3dMaterial) > GEOMETRY_LIMIT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This PLY scene exceeds the 512 MiB geometry limit."); goto done;
    }
    if ((guint64)status.st_size < (guint64)header.data_offset + header.vertices * 12) {
        invalid(error, "The PLY vertex data is truncated."); goto done;
    }
    /* Reject an obviously non-triangular mesh before allocating its buffers. */
    if (header.faces) {
        off_t faces_at = header.data_offset + (off_t)(header.vertices * 12);
        if (fseeko(stream, faces_at, SEEK_SET)) { invalid(error, "The PLY face data could not be read."); goto done; }
        int first = fgetc(stream);
        if (first == EOF) { invalid(error, "The PLY face data is truncated."); goto done; }
        if (first != 3) { *handled = FALSE; goto done; }
        if (fseeko(stream, header.data_offset, SEEK_SET)) { invalid(error, "The PLY vertex data could not be read."); goto done; }
    }
    if (stopped(cancel, error)) goto done;
    Progress report = {feedback, G_MAXUINT};
    progress(&report, 0);
    model = g_try_new0(Zen3dModel, 1);
    if (model) {
        model->vertex_count = (uint32_t)header.vertices; model->index_count = (uint32_t)indices;
        model->part_count = model->material_count = 1;
        model->vertices = g_try_malloc0_n(model->vertex_count, sizeof(Zen3dVertex));
        model->indices = g_try_malloc_n(model->index_count, sizeof(uint32_t));
        model->parts = g_try_new0(Zen3dPart, 1); model->materials = g_try_new0(Zen3dMaterial, 1);
    }
    if (!model || !model->vertices || !model->indices || !model->parts || !model->materials) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "There is not enough memory for this PLY model."); goto failed;
    }
    double minimum[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, maximum[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    guchar block[BLOCK_RECORDS * 13];
    for (uint32_t base = 0; base < model->vertex_count;) {
        if (stopped(cancel, error)) goto failed;
        guint count = MIN(BLOCK_RECORDS, model->vertex_count - base);
        if (fread(block, 12, count, stream) != count) { invalid(error, "The PLY vertex data is truncated."); goto failed; }
        for (guint i = 0; i < count; i++) {
            Zen3dVertex *vertex = &model->vertices[base+i];
            for (int axis = 0; axis < 3; axis++) {
                float value = little_float(block + i*12 + axis*4);
                if (!isfinite(value)) { invalid(error, "A PLY vertex position is not finite."); goto failed; }
                vertex->position[axis] = value;
                minimum[axis] = MIN(minimum[axis], value); maximum[axis] = MAX(maximum[axis], value);
            }
            memset(vertex->color, 255, sizeof(vertex->color));
        }
        base += count;
        progress(&report, (unsigned)((guint64)base * 25 / model->vertex_count));
    }
    double center[3];
    for (int axis = 0; axis < 3; axis++) {
        model->dimensions[axis] = maximum[axis] - minimum[axis];
        center[axis] = minimum[axis] + model->dimensions[axis] / 2;
    }
    double radius = hypot(hypot(model->dimensions[0], model->dimensions[1]), model->dimensions[2]) / 2;
    if (!(radius > DBL_MIN)) radius = 1;
    for (int axis = 0; axis < 3; axis++) {
        model->bounds_min[axis] = (float)((minimum[axis] - center[axis]) / radius);
        model->bounds_max[axis] = (float)((maximum[axis] - center[axis]) / radius);
    }
    for (uint32_t i = 0; i < model->vertex_count; i++) {
        if (!(i % BLOCK_RECORDS)) {
            if (stopped(cancel, error)) goto failed;
            progress(&report, 25 + (unsigned)((guint64)i * 10 / model->vertex_count));
        }
        for (int axis = 0; axis < 3; axis++) model->vertices[i].position[axis] = (float)(((double)model->vertices[i].position[axis] - center[axis]) / radius);
    }
    progress(&report, 35);
    gboolean triangle_size = (guint64)status.st_size == (guint64)header.data_offset + header.vertices*12 + header.faces*13;
    if (header.faces >= PREVIEW_TRIANGLES && triangle_size && !publish_preview(model, cancel, feedback, error)) goto failed;
    for (uint32_t base = 0; base < header.faces;) {
        if (stopped(cancel, error)) goto failed;
        guint count = MIN((guint64)BLOCK_RECORDS, header.faces - base);
        size_t bytes = fread(block, 1, count*13, stream);
        for (guint i = 0; i < count; i++) {
            size_t offset = (size_t)i * 13;
            if (offset >= bytes) { invalid(error, "The PLY face data is truncated."); goto failed; }
            if (block[offset] != 3) { *handled = FALSE; goto failed; }
            if (offset + 13 > bytes) { invalid(error, "The PLY face data is truncated."); goto failed; }
            uint32_t *triangle = model->indices + (size_t)(base+i) * 3;
            Zen3dVertex *vertices[3];
            for (int corner = 0; corner < 3; corner++) {
                triangle[corner] = little_u32(block + offset + 1 + corner*4);
                if (triangle[corner] >= model->vertex_count) { invalid(error, "A PLY face references an invalid vertex."); goto failed; }
                vertices[corner] = &model->vertices[triangle[corner]];
            }
            double a[3], b[3], normal[3];
            for (int axis = 0; axis < 3; axis++) {
                a[axis] = (double)vertices[1]->position[axis] - vertices[0]->position[axis];
                b[axis] = (double)vertices[2]->position[axis] - vertices[0]->position[axis];
            }
            normal[0] = a[1]*b[2] - a[2]*b[1]; normal[1] = a[2]*b[0] - a[0]*b[2]; normal[2] = a[0]*b[1] - a[1]*b[0];
            for (int corner = 0; corner < 3; corner++) for (int axis = 0; axis < 3; axis++)
                vertices[corner]->normal[axis] += (float)normal[axis];
        }
        base += count;
        progress(&report, 35 + (unsigned)((guint64)base * 50 / header.faces));
    }
    for (uint32_t i = 0; i < model->vertex_count; i++) {
        if (!(i % BLOCK_RECORDS)) {
            if (stopped(cancel, error)) goto failed;
            progress(&report, 85 + (unsigned)((guint64)i * 5 / model->vertex_count));
        }
        float *normal = model->vertices[i].normal;
        double length = sqrt((double)normal[0]*normal[0] + (double)normal[1]*normal[1] + (double)normal[2]*normal[2]);
        if (length > DBL_MIN) for (int axis = 0; axis < 3; axis++) normal[axis] = (float)(normal[axis] / length);
        else normal[2] = 1;
        if (!header.faces) model->indices[i] = i;
    }
    model->parts[0].index_count = model->index_count;
    model->parts[0].primitive = header.faces ? ZEN3D_TRIANGLES : ZEN3D_POINTS;
    model->materials[0] = scan_material;
    progress(&report, 90);
    goto done;
failed:
    zen3d_model_free(model); model = NULL;
done:
    fclose(stream);
    return model;
}
