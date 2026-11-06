/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include "ply.h"
#include <assimp/cimport.h>
#include <assimp/cfileio.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIB ((guint64)1024 * 1024)
#define FILE_LIMIT (256 * MIB)
#define INPUT_LIMIT (512 * MIB)
#define GEOMETRY_LIMIT (256 * MIB)
#define TEXTURE_PIXELS (32 * 1024 * 1024u)
#define ONE_TEXTURE_PIXELS (4 * 1024 * 1024u)
#define MAX_MATERIALS 4096u
#define MAX_NODES 100000u
#define MAX_PARTS 100000u
#define MAX_DEPTH 128u

typedef struct {
    GCancellable *cancel;
    char *directory;
    GHashTable *seen;
    GPtrArray *search_dirs;
    guint64 input_bytes, read_bytes;
    GError *failure;
} ImportIO;

typedef struct {
    FILE *stream;
    size_t length;
    ImportIO *owner;
} InputFile;

typedef struct {
    const struct aiScene *scene;
    ImportIO *io;
    Zen3dModel *model;
    guint *uv_channels;
    GHashTable *textures;
    guint64 vertex_count, index_count, part_count;
    guint nodes, mesh_instances, texture_pixels;
    double minimum[3], maximum[3], center[3], radius;
    GError **error;
} Conversion;

static gboolean cancelled(GCancellable *cancel, GError **error) {
    return cancel && g_cancellable_set_error_if_cancelled(cancel, error);
}

static void io_limit(ImportIO *io, const char *message) {
    if (!io->failure) g_set_error_literal(&io->failure, G_IO_ERROR, G_IO_ERROR_NO_SPACE, message);
}

static char *local_path(const char *name, const char *directory) {
    if (!name || !*name) return NULL;
    char *path;
    if (g_str_has_prefix(name, "file:")) path = g_filename_from_uri(name, NULL, NULL);
    else {
        char *scheme = g_uri_parse_scheme(name);
        if (scheme) { g_free(scheme); return NULL; }
        path = g_strdup(name);
        for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
    }
    if (!path) return NULL;
    char *absolute = g_canonicalize_filename(path, directory);
    g_free(path); return absolute;
}

static void remember_directory(ImportIO *io, const char *path) {
    if (io->search_dirs->len >= 256) return;
    char *directory = g_path_get_dirname(path);
    for (guint i = 0; i < io->search_dirs->len; i++)
        if (!strcmp(directory, g_ptr_array_index(io->search_dirs, i))) { g_free(directory); return; }
    g_ptr_array_add(io->search_dirs, directory);
}

/* This also applies the source budget to external buffers and textures. It
 * does not constrain allocations made internally by Assimp or image codecs. */
static InputFile *input_open(ImportIO *io, const char *path) {
    if (io->failure || (io->cancel && g_cancellable_is_cancelled(io->cancel))) return NULL;
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) return NULL;
    FILE *stream = fdopen(descriptor, "rb");
    if (!stream) { close(descriptor); return NULL; }
    struct stat status;
    if (fstat(fileno(stream), &status) || !S_ISREG(status.st_mode) || status.st_size < 0) {
        fclose(stream); return NULL;
    }
    guint64 size = (guint64)status.st_size;
    if (size > FILE_LIMIT) {
        io_limit(io, "A model or its sidecar exceeds the 256 MiB file limit.");
        fclose(stream); return NULL;
    }
    if (!g_hash_table_contains(io->seen, path)) {
        if (io->input_bytes + size > INPUT_LIMIT) {
            io_limit(io, "The model and its sidecars exceed the 512 MiB input limit.");
            fclose(stream); return NULL;
        }
        io->input_bytes += size;
        g_hash_table_add(io->seen, g_strdup(path));
    }
    remember_directory(io, path);
    InputFile *file = g_new0(InputFile, 1);
    file->stream = stream; file->length = (size_t)size; file->owner = io;
    return file;
}

static void input_close(InputFile *file) { fclose(file->stream); g_free(file); }

static size_t input_read(InputFile *file, void *data, size_t size, size_t count) {
    ImportIO *io = file->owner;
    if (!size || !count || count > SIZE_MAX / size || io->failure ||
        (io->cancel && g_cancellable_is_cancelled(io->cancel))) return 0;
    if (io->read_bytes > INPUT_LIMIT * 8) {
        io_limit(io, "The importer exceeded its file-read budget."); return 0;
    }
    size_t result = fread(data, size, count, file->stream);
    io->read_bytes += (guint64)result * size;
    return result;
}

static size_t assimp_read(struct aiFile *file, char *data, size_t size, size_t count) {
    return input_read((InputFile *)file->UserData, data, size, count);
}
static size_t assimp_write(struct aiFile *file, const char *data, size_t size, size_t count) {
    (void)file; (void)data; (void)size; (void)count; return 0;
}
static size_t assimp_tell(struct aiFile *file) {
    off_t position = ftello(((InputFile *)file->UserData)->stream);
    return position < 0 ? 0 : (size_t)position;
}
static size_t assimp_size(struct aiFile *file) { return ((InputFile *)file->UserData)->length; }
static enum aiReturn assimp_seek(struct aiFile *file, size_t offset, enum aiOrigin origin) {
    InputFile *input = (InputFile *)file->UserData;
    if (input->owner->cancel && g_cancellable_is_cancelled(input->owner->cancel)) return aiReturn_FAILURE;
    int whence = origin == aiOrigin_SET ? SEEK_SET : origin == aiOrigin_CUR ? SEEK_CUR : SEEK_END;
    return fseeko(input->stream, (off_t)offset, whence) ? aiReturn_FAILURE : aiReturn_SUCCESS;
}
static void assimp_flush(struct aiFile *file) { (void)file; }

static struct aiFile *assimp_open(struct aiFileIO *system, const char *name, const char *mode) {
    ImportIO *io = (ImportIO *)system->UserData;
    if (!mode || mode[0] != 'r' || strchr(mode, '+')) return NULL;
    char *path = local_path(name, io->directory);
    if (!path) return NULL;
    InputFile *input = input_open(io, path);
    g_free(path);
    if (!input) return NULL;
    struct aiFile *file = g_new0(struct aiFile, 1);
    file->ReadProc = assimp_read; file->WriteProc = assimp_write;
    file->TellProc = assimp_tell; file->FileSizeProc = assimp_size;
    file->SeekProc = assimp_seek; file->FlushProc = assimp_flush;
    file->UserData = (aiUserData)input;
    return file;
}
static void assimp_close(struct aiFileIO *system, struct aiFile *file) {
    (void)system; input_close((InputFile *)file->UserData); g_free(file);
}

static gboolean invalid(Conversion *work, const char *message) {
    g_set_error_literal(work->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE;
}
static gboolean over_budget(Conversion *work) {
    if (work->vertex_count > UINT32_MAX || work->index_count > UINT32_MAX ||
        work->part_count > MAX_PARTS || work->vertex_count * sizeof(Zen3dVertex) +
        work->index_count * sizeof(uint32_t) + work->part_count * sizeof(Zen3dPart) > GEOMETRY_LIMIT) {
        g_set_error_literal(work->error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                            "This scene exceeds the 256 MiB geometry limit.");
        return TRUE;
    }
    return FALSE;
}

static void matrix_multiply(double out[16], const double parent[16], const struct aiMatrix4x4 *local) {
    const double b[16] = {local->a1, local->a2, local->a3, local->a4,
        local->b1, local->b2, local->b3, local->b4, local->c1, local->c2, local->c3, local->c4,
        local->d1, local->d2, local->d3, local->d4};
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        out[r * 4 + c] = 0;
        for (int k = 0; k < 4; k++) out[r * 4 + c] += parent[r * 4 + k] * b[k * 4 + c];
    }
}
static gboolean transform_position(const double matrix[16], const struct aiVector3D *in, double out[3]) {
    for (int r = 0; r < 3; r++) {
        out[r] = matrix[r * 4] * in->x + matrix[r * 4 + 1] * in->y + matrix[r * 4 + 2] * in->z + matrix[r * 4 + 3];
        if (!isfinite(out[r])) return FALSE;
    }
    return TRUE;
}

/* The cofactor matrix divided by det is the inverse transpose. */
static double normal_matrix(const double m[16], double normal[9]) {
    normal[0] = m[5]*m[10] - m[6]*m[9]; normal[1] = m[6]*m[8] - m[4]*m[10];
    normal[2] = m[4]*m[9] - m[5]*m[8]; normal[3] = m[2]*m[9] - m[1]*m[10];
    normal[4] = m[0]*m[10] - m[2]*m[8]; normal[5] = m[1]*m[8] - m[0]*m[9];
    normal[6] = m[1]*m[6] - m[2]*m[5]; normal[7] = m[2]*m[4] - m[0]*m[6];
    normal[8] = m[0]*m[5] - m[1]*m[4];
    double det = m[0]*normal[0] + m[1]*normal[1] + m[2]*normal[2];
    if (det != 0 && isfinite(det)) for (int i = 0; i < 9; i++) normal[i] /= det;
    else for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) normal[r*3+c] = m[r*4+c];
    return det;
}

static gboolean scan_node(Conversion *work, const struct aiNode *node, const double parent[16], guint depth) {
    if (cancelled(work->io->cancel, work->error)) return FALSE;
    if (!node || depth > MAX_DEPTH || ++work->nodes > MAX_NODES)
        return invalid(work, "The scene hierarchy is invalid or too complex.");
    double world[16]; matrix_multiply(world, parent, &node->mTransformation);
    if (node->mNumMeshes && !node->mMeshes) return invalid(work, "The scene has an invalid mesh reference.");
    for (guint i = 0; i < node->mNumMeshes; i++) {
        if (cancelled(work->io->cancel, work->error)) return FALSE;
        if (++work->mesh_instances > MAX_PARTS) return invalid(work, "The scene contains too many mesh instances.");
        if (node->mMeshes[i] >= work->scene->mNumMeshes) return invalid(work, "The scene has an invalid mesh reference.");
        const struct aiMesh *mesh = work->scene->mMeshes[node->mMeshes[i]];
        if (!mesh || (mesh->mNumVertices && !mesh->mVertices) || (mesh->mNumFaces && !mesh->mFaces))
            return invalid(work, "The scene contains an invalid mesh.");
        work->vertex_count += mesh->mNumVertices;
        gboolean primitives[3] = {FALSE, FALSE, FALSE};
        if (!mesh->mNumFaces && mesh->mNumVertices) {
            work->index_count += mesh->mNumVertices; primitives[ZEN3D_POINTS] = TRUE;
        }
        for (guint f = 0; f < mesh->mNumFaces; f++) {
            if (!(f % 4096) && (cancelled(work->io->cancel, work->error) || over_budget(work))) return FALSE;
            const struct aiFace *face = &mesh->mFaces[f];
            if (!face->mNumIndices) continue;
            if (face->mNumIndices > 3 || !face->mIndices) return invalid(work, "A mesh could not be triangulated.");
            primitives[face->mNumIndices == 3 ? ZEN3D_TRIANGLES : face->mNumIndices == 2 ? ZEN3D_LINES : ZEN3D_POINTS] = TRUE;
            work->index_count += face->mNumIndices;
            for (guint j = 0; j < face->mNumIndices; j++)
                if (face->mIndices[j] >= mesh->mNumVertices) return invalid(work, "A mesh contains an invalid vertex index.");
        }
        for (int p = 0; p < 3; p++) if (primitives[p]) work->part_count++;
        if (over_budget(work)) return FALSE;
        for (guint v = 0; v < mesh->mNumVertices; v++) {
            if (!(v % 4096) && cancelled(work->io->cancel, work->error)) return FALSE;
            double point[3];
            if (!transform_position(world, &mesh->mVertices[v], point)) return invalid(work, "A model position is not finite.");
            for (int axis = 0; axis < 3; axis++) {
                work->minimum[axis] = MIN(work->minimum[axis], point[axis]);
                work->maximum[axis] = MAX(work->maximum[axis], point[axis]);
            }
        }
    }
    if (node->mNumChildren && !node->mChildren) return invalid(work, "The scene has an invalid hierarchy.");
    for (guint i = 0; i < node->mNumChildren; i++)
        if (!scan_node(work, node->mChildren[i], world, depth + 1)) return FALSE;
    return TRUE;
}

static float color_component(double value, float fallback) {
    return isfinite(value) ? (float)CLAMP(value, 0., 1.) : fallback;
}

static gboolean fill_node(Conversion *work, const struct aiNode *node, const double parent[16]) {
    if (cancelled(work->io->cancel, work->error)) return FALSE;
    Zen3dModel *model = work->model;
    double world[16], normal[9]; matrix_multiply(world, parent, &node->mTransformation);
    double determinant = normal_matrix(world, normal);
    for (guint m = 0; m < node->mNumMeshes; m++) {
        const struct aiMesh *mesh = work->scene->mMeshes[node->mMeshes[m]];
        uint32_t first_vertex = model->vertex_count;
        guint material = mesh->mMaterialIndex < model->material_count ? mesh->mMaterialIndex : 0;
        guint channel = work->uv_channels[material];
        const struct aiVector3D *uvs = channel < AI_MAX_NUMBER_OF_TEXTURECOORDS ? mesh->mTextureCoords[channel] : NULL;
        gboolean repair_normals = FALSE;
        for (guint v = 0; v < mesh->mNumVertices; v++) {
            if (!(v % 4096) && cancelled(work->io->cancel, work->error)) return FALSE;
            Zen3dVertex *vertex = &model->vertices[model->vertex_count++];
            double position[3]; transform_position(world, &mesh->mVertices[v], position);
            for (int axis = 0; axis < 3; axis++) vertex->position[axis] = (float)((position[axis] - work->center[axis]) / work->radius);
            double n[3] = {0, 0, 0};
            if (mesh->mNormals) for (int r = 0; r < 3; r++) {
                const struct aiVector3D *source = &mesh->mNormals[v];
                n[r] = normal[r*3] * source->x + normal[r*3+1] * source->y + normal[r*3+2] * source->z;
            }
            double length = hypot(hypot(n[0], n[1]), n[2]);
            if (!(length > DBL_MIN) || !isfinite(length)) {
                n[0] = n[1] = n[2] = 0; length = 1; repair_normals = TRUE;
            }
            for (int axis = 0; axis < 3; axis++) vertex->normal[axis] = (float)(n[axis] / length);
            if (uvs) {
                vertex->uv[0] = isfinite(uvs[v].x) ? uvs[v].x : 0;
                vertex->uv[1] = isfinite(uvs[v].y) ? uvs[v].y : 0;
            }
            if (mesh->mColors[0]) {
                const struct aiColor4D *color = &mesh->mColors[0][v];
                vertex->color[0] = (uint8_t)lroundf(color_component(color->r, 1) * 255);
                vertex->color[1] = (uint8_t)lroundf(color_component(color->g, 1) * 255);
                vertex->color[2] = (uint8_t)lroundf(color_component(color->b, 1) * 255);
                vertex->color[3] = (uint8_t)lroundf(color_component(color->a, 1) * 255);
            } else memset(vertex->color, 255, 4);
        }
        /* STL files often store zero normals. Assimp preserves those values,
         * so recover a geometric normal without replacing valid artist normals. */
        if (repair_normals) {
            for (guint f = 0; f < mesh->mNumFaces; f++) {
                if (!(f % 4096) && cancelled(work->io->cancel, work->error)) return FALSE;
                const struct aiFace *face = &mesh->mFaces[f];
                if (face->mNumIndices != 3) continue;
                Zen3dVertex *v[3];
                for (int j = 0; j < 3; j++) v[j] = &model->vertices[first_vertex + face->mIndices[j]];
                double a[3], b[3], n[3];
                for (int j = 0; j < 3; j++) {
                    a[j] = (double)v[1]->position[j] - v[0]->position[j];
                    b[j] = (double)v[2]->position[j] - v[0]->position[j];
                }
                n[0] = a[1]*b[2] - a[2]*b[1]; n[1] = a[2]*b[0] - a[0]*b[2]; n[2] = a[0]*b[1] - a[1]*b[0];
                double length = hypot(hypot(n[0], n[1]), n[2]);
                if (!(length > DBL_MIN)) continue;
                double scale = (determinant < 0 ? -1. : 1.) / length;
                for (int j = 0; j < 3; j++) if (v[j]->normal[0] == 0 && v[j]->normal[1] == 0 && v[j]->normal[2] == 0)
                    for (int axis = 0; axis < 3; axis++) v[j]->normal[axis] = (float)(n[axis] * scale);
            }
            for (guint v = first_vertex; v < model->vertex_count; v++) {
                float *n = model->vertices[v].normal;
                if (n[0] == 0 && n[1] == 0 && n[2] == 0) n[2] = 1;
            }
        }
        for (int primitive = 0; primitive < 3; primitive++) {
            guint face_size = primitive == ZEN3D_TRIANGLES ? 3 : primitive == ZEN3D_LINES ? 2 : 1;
            uint32_t first = model->index_count;
            if (!mesh->mNumFaces && primitive == ZEN3D_POINTS) {
                for (guint v = 0; v < mesh->mNumVertices; v++) model->indices[model->index_count++] = first_vertex + v;
            }
            for (guint f = 0; f < mesh->mNumFaces; f++) {
                if (!(f % 4096) && cancelled(work->io->cancel, work->error)) return FALSE;
                const struct aiFace *face = &mesh->mFaces[f];
                if (face->mNumIndices != face_size) continue;
                for (guint j = 0; j < face_size; j++) {
                    guint at = face_size == 3 && determinant < 0 && j ? 3 - j : j;
                    model->indices[model->index_count++] = first_vertex + face->mIndices[at];
                }
            }
            if (model->index_count != first) {
                Zen3dPart *part = &model->parts[model->part_count++];
                part->first_index = first; part->index_count = model->index_count - first;
                part->material = material; part->primitive = (Zen3dPrimitive)primitive;
            }
        }
    }
    for (guint i = 0; i < node->mNumChildren; i++)
        if (!fill_node(work, node->mChildren[i], world)) return FALSE;
    return TRUE;
}

typedef struct { guint pixels; } TextureSize;
static void texture_dimensions(int width, int height, guint pixels, int *w, int *h) {
    double scale = MIN(1., MIN(4096. / MAX(width, height), sqrt((double)pixels / ((double)width * height))));
    *w = MAX(1, (int)(width * scale)); *h = MAX(1, (int)(height * scale));
    if ((guint64)*w * *h > pixels) {
        if (*w >= *h) *w = MAX(1, pixels / *h);
        else *h = MAX(1, pixels / *w);
    }
}
static void texture_size(GdkPixbufLoader *loader, int width, int height, gpointer data) {
    TextureSize *size = data;
    if (width < 1 || height < 1) return;
    int w, h; texture_dimensions(width, height, size->pixels, &w, &h);
    if (w != width || h != height) gdk_pixbuf_loader_set_size(loader, w, h);
}
static GdkPixbuf *finish_texture(GdkPixbufLoader *loader, gboolean okay, guint pixels) {
    GError *error = NULL;
    if (!gdk_pixbuf_loader_close(loader, &error)) okay = FALSE;
    g_clear_error(&error);
    GdkPixbuf *source = okay ? gdk_pixbuf_loader_get_pixbuf(loader) : NULL;
    if (!source) return NULL;
    int w, h; texture_dimensions(gdk_pixbuf_get_width(source), gdk_pixbuf_get_height(source), pixels, &w, &h);
    GdkPixbuf *scaled = w != gdk_pixbuf_get_width(source) || h != gdk_pixbuf_get_height(source) ?
        gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_BILINEAR) : g_object_ref(source);
    if (!scaled) return NULL;
    if (gdk_pixbuf_get_has_alpha(scaled)) return scaled;
    GdkPixbuf *rgba = gdk_pixbuf_add_alpha(scaled, FALSE, 0, 0, 0); g_object_unref(scaled);
    return rgba;
}
static GdkPixbuf *encoded_texture(Conversion *work, InputFile *input, const guchar *memory, gsize length, guint pixels) {
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    TextureSize size = {pixels};
    g_signal_connect(loader, "size-prepared", G_CALLBACK(texture_size), &size);
    gboolean okay = TRUE;
    guchar buffer[65536];
    for (gsize offset = 0;;) {
        if (work->io->cancel && g_cancellable_is_cancelled(work->io->cancel)) { okay = FALSE; break; }
        gsize count = input ? input_read(input, buffer, 1, sizeof(buffer)) : MIN(length - offset, sizeof(buffer));
        if (!count) { if (input && ferror(input->stream)) okay = FALSE; break; }
        GError *error = NULL;
        if (!gdk_pixbuf_loader_write(loader, input ? buffer : memory + offset, count, &error)) okay = FALSE;
        g_clear_error(&error);
        if (!okay) break;
        offset += count;
    }
    GdkPixbuf *result = finish_texture(loader, okay, pixels); g_object_unref(loader);
    return result;
}
static GdkPixbuf *raw_texture(Conversion *work, const struct aiTexture *texture, guint pixels) {
    if (!texture->mWidth || !texture->mHeight || texture->mWidth > INT_MAX || texture->mHeight > INT_MAX || !texture->pcData ||
        (guint64)texture->mWidth * texture->mHeight > FILE_LIMIT / sizeof(struct aiTexel))
        return NULL;
    int w, h; texture_dimensions((int)texture->mWidth, (int)texture->mHeight, pixels, &w, &h);
    GdkPixbuf *result = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    if (!result) return NULL;
    guchar *data = gdk_pixbuf_get_pixels(result); int stride = gdk_pixbuf_get_rowstride(result);
    for (int y = 0; y < h; y++) {
        if (work->io->cancel && g_cancellable_is_cancelled(work->io->cancel)) { g_object_unref(result); return NULL; }
        guint sy = (guint)((guint64)y * texture->mHeight / h);
        for (int x = 0; x < w; x++) {
            guint sx = (guint)((guint64)x * texture->mWidth / w);
            const struct aiTexel *texel = &texture->pcData[(gsize)sy * texture->mWidth + sx];
            guchar *out = data + (gsize)y * stride + x * 4;
            out[0] = texel->r; out[1] = texel->g; out[2] = texel->b; out[3] = texel->a;
        }
    }
    return result;
}
static void optional_unref(gpointer object) { if (object) g_object_unref(object); }

static GdkPixbuf *load_texture(Conversion *work, const char *name) {
    gpointer cached;
    if (g_hash_table_lookup_extended(work->textures, name, NULL, &cached)) return cached ? g_object_ref(cached) : NULL;
    GdkPixbuf *result = NULL;
    guint pixels = MIN(ONE_TEXTURE_PIXELS, TEXTURE_PIXELS - work->texture_pixels);
    if (!pixels) return NULL;
    const struct aiTexture *embedded = NULL;
    if (name[0] == '*') {
        char *end; guint64 index = g_ascii_strtoull(name + 1, &end, 10);
        if (end != name + 1 && !*end && index < work->scene->mNumTextures) embedded = work->scene->mTextures[index];
    }
    for (guint i = 0; !embedded && i < work->scene->mNumTextures; i++)
        if (work->scene->mTextures[i] && !strcmp(work->scene->mTextures[i]->mFilename.data, name)) embedded = work->scene->mTextures[i];
    if (embedded && embedded->pcData) {
        if (embedded->mHeight) result = raw_texture(work, embedded, pixels);
        else if (embedded->mWidth <= FILE_LIMIT)
            result = encoded_texture(work, NULL, (const guchar *)embedded->pcData, embedded->mWidth, pixels);
    } else if (name[0] != '*') {
        char *unescaped = g_uri_unescape_string(name, NULL);
        for (guint d = 0; !result && d < work->io->search_dirs->len; d++) {
            const char *directory = g_ptr_array_index(work->io->search_dirs, d);
            char *path = local_path(unescaped ? unescaped : name, directory);
            if (!path) break;
            InputFile *input = input_open(work->io, path);
            g_free(path);
            if (input) { result = encoded_texture(work, input, NULL, 0, pixels); input_close(input); }
            if (g_path_is_absolute(name) || work->io->failure) break;
        }
        g_free(unescaped);
    }
    if (result) work->texture_pixels += gdk_pixbuf_get_width(result) * gdk_pixbuf_get_height(result);
    g_hash_table_insert(work->textures, g_strdup(name), result ? g_object_ref(result) : NULL);
    return result;
}

static gboolean load_materials(Conversion *work) {
    Zen3dModel *model = work->model;
    for (guint i = 0; i < model->material_count; i++) {
        if (cancelled(work->io->cancel, work->error)) return FALSE;
        Zen3dMaterial *out = &model->materials[i];
        for (int c = 0; c < 4; c++) out->color[c] = 1;
        if (i >= work->scene->mNumMaterials || !work->scene->mMaterials[i]) continue;
        const struct aiMaterial *material = work->scene->mMaterials[i];
        struct aiColor4D color = {1, 1, 1, 1};
        gboolean base_color = aiGetMaterialColor(material, AI_MATKEY_BASE_COLOR, &color) == aiReturn_SUCCESS;
        if (!base_color) aiGetMaterialColor(material, AI_MATKEY_COLOR_DIFFUSE, &color);
        out->color[0] = color_component(color.r, 1); out->color[1] = color_component(color.g, 1);
        out->color[2] = color_component(color.b, 1); out->color[3] = color_component(color.a, 1);
        ai_real opacity = 1; unsigned int count = 1;
        if (!base_color && aiGetMaterialFloatArray(material, AI_MATKEY_OPACITY, &opacity, &count) == aiReturn_SUCCESS)
            out->color[3] *= color_component(opacity, 1);
        struct aiString path = {0};
        enum aiTextureType type = aiGetMaterialTextureCount(material, aiTextureType_BASE_COLOR) ? aiTextureType_BASE_COLOR : aiTextureType_DIFFUSE;
        enum aiTextureMapping mapping = aiTextureMapping_UV;
        if (aiGetMaterialTexture(material, type, 0, &path, &mapping, &work->uv_channels[i], NULL, NULL, NULL, NULL) == aiReturn_SUCCESS &&
            mapping == aiTextureMapping_UV && path.length && path.length < sizeof(path.data)) {
            char *name = g_strndup(path.data, path.length);
            out->texture = load_texture(work, name); g_free(name);
        }
        if (work->io->failure) { g_propagate_error(work->error, g_steal_pointer(&work->io->failure)); return FALSE; }
    }
    return TRUE;
}

void zen3d_model_free(Zen3dModel *model) {
    if (!model) return;
    if (model->materials) for (guint i = 0; i < model->material_count; i++) g_clear_object(&model->materials[i].texture);
    g_free(model->vertices); g_free(model->indices); g_free(model->preview_indices);
    g_free(model->parts); g_free(model->materials); g_free(model);
}

/* Dense scanned meshes are too costly to draw at full detail on every camera
 * frame. Weld nearby vertices for a motion-only index buffer. Both views use
 * the original vertex buffer, and the full mesh stays intact. */
enum { PREVIEW_GRID = 128, PREVIEW_MAX_TRIANGLES = 500000, PREVIEW_TABLE_SIZE = 1 << 20 };

static uint64_t preview_face_key(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t low = MIN(a, MIN(b, c)), high = MAX(a, MAX(b, c));
    uint32_t middle = a + b + c - low - high;
    /* Each cell address fits in 21 bits. Reserve key zero for an empty slot. */
    return (((uint64_t)low << 42) | ((uint64_t)middle << 21) | high) + 1;
}

static void prepare_motion_preview(Zen3dModel *model, GCancellable *cancel) {
    if (model->index_count < 4500000) return;
    g_autofree uint32_t *cell_for_vertex = g_try_new(uint32_t, model->vertex_count);
    g_autofree uint32_t *representative = g_try_new0(uint32_t, PREVIEW_GRID * PREVIEW_GRID * PREVIEW_GRID);
    g_autofree uint64_t *faces = g_try_new0(uint64_t, PREVIEW_TABLE_SIZE);
    g_autofree uint32_t *indices = g_try_new(uint32_t, PREVIEW_MAX_TRIANGLES * 3);
    if (!cell_for_vertex || !representative || !faces || !indices) return;

    float extent = 0;
    for (int axis = 0; axis < 3; axis++) extent = MAX(extent, model->bounds_max[axis] - model->bounds_min[axis]);
    if (extent <= 0) return;
    float scale = PREVIEW_GRID / extent;
    for (uint32_t i = 0; i < model->vertex_count; i++) {
        if (!(i & 4095) && cancelled(cancel, NULL)) return;
        uint32_t cell = 0;
        for (int axis = 0; axis < 3; axis++) {
            int coordinate = (int)((model->vertices[i].position[axis] - model->bounds_min[axis]) * scale);
            cell = cell * PREVIEW_GRID + (uint32_t)CLAMP(coordinate, 0, PREVIEW_GRID - 1);
        }
        cell_for_vertex[i] = cell;
        if (!representative[cell]) representative[cell] = i + 1;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < model->index_count; i += 3) {
        if (!(i & 4095) && cancelled(cancel, NULL)) return;
        uint32_t a = cell_for_vertex[model->indices[i]];
        uint32_t b = cell_for_vertex[model->indices[i + 1]];
        uint32_t c = cell_for_vertex[model->indices[i + 2]];
        if (a == b || b == c || a == c) continue;
        uint64_t key = preview_face_key(a, b, c);
        uint32_t slot = (uint32_t)(((key ^ (key >> 23)) * UINT64_C(0x9e3779b97f4a7c15)) >> 44);
        while (faces[slot] && faces[slot] != key) slot = (slot + 1) & (PREVIEW_TABLE_SIZE - 1);
        if (faces[slot]) continue;
        if (count == PREVIEW_MAX_TRIANGLES * 3) return;
        faces[slot] = key;
        indices[count++] = representative[a] - 1;
        indices[count++] = representative[b] - 1;
        indices[count++] = representative[c] - 1;
    }
    if (count) {
        uint32_t *compact = g_try_renew(uint32_t, indices, count);
        if (compact) indices = compact;
        model->preview_indices = g_steal_pointer(&indices);
        model->preview_index_count = count;
    }
}

static Zen3dModel *load_model_serialized(GFile *file, GCancellable *cancel,
                                        const Zen3dLoadFeedback *feedback, GError **error) {
    if (cancelled(cancel, error)) return NULL;
    gboolean handled;
    Zen3dModel *packed = zen3d_ply_load(file, cancel, &handled, feedback, error);
    if (handled) {
        if (packed && feedback && feedback->progress) feedback->progress(94, feedback->data);
        if (packed) prepare_motion_preview(packed, cancel);
        if (packed && cancelled(cancel, error)) {
            zen3d_model_free(packed);
            return NULL;
        }
        if (packed && feedback && feedback->progress) feedback->progress(100, feedback->data);
        return packed;
    }
    char *path = g_file_get_path(file);
    if (!path) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Only local model files are supported."); return NULL; }
    ImportIO io = {.cancel = cancel, .directory = g_path_get_dirname(path),
        .seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), .search_dirs = g_ptr_array_new_with_free_func(g_free)};
    g_ptr_array_add(io.search_dirs, g_strdup(io.directory));
    struct aiFileIO system = {.OpenProc = assimp_open, .CloseProc = assimp_close, .UserData = (aiUserData)&io};
    struct aiPropertyStore *properties = aiCreatePropertyStore();
    aiSetImportPropertyInteger(properties, AI_CONFIG_IMPORT_NO_SKELETON_MESHES, TRUE);
    /* Assimp's generic validation rejects valid face-free PLY point clouds.
     * scan_node instead checks the hierarchy, indices, positions and budgets. */
    unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_TransformUVCoords | aiProcess_FlipUVs;
    const struct aiScene *scene = aiImportFileExWithProperties(path, flags, &system, properties);
    aiReleasePropertyStore(properties);
    Zen3dModel *result = NULL;
    Conversion work = {.scene = scene, .io = &io, .error = error,
        .textures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, optional_unref)};
    if (cancelled(cancel, error)) goto done;
    if (io.failure) { g_propagate_error(error, g_steal_pointer(&io.failure)); goto done; }
    if (!scene) {
        const char *message = aiGetErrorString();
        char *valid = g_utf8_make_valid(message && *message ? message : "The model could not be imported.", -1);
        char *shortened = g_utf8_substring(valid, 0, MIN((glong)240, g_utf8_strlen(valid, -1)));
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s", shortened);
        g_free(shortened); g_free(valid); goto done;
    }
    if (!scene->mRootNode || !scene->mNumMeshes || !scene->mMeshes) { invalid(&work, "This file contains no mesh geometry."); goto done; }
    if (scene->mNumMaterials > MAX_MATERIALS || scene->mNumTextures > MAX_MATERIALS ||
        (scene->mNumMaterials && !scene->mMaterials) || (scene->mNumTextures && !scene->mTextures)) {
        invalid(&work, "The model has invalid or excessive material data."); goto done;
    }
    for (int axis = 0; axis < 3; axis++) { work.minimum[axis] = DBL_MAX; work.maximum[axis] = -DBL_MAX; }
    const double identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (!scan_node(&work, scene->mRootNode, identity, 0)) goto done;
    if (!work.vertex_count || !work.index_count) { invalid(&work, "This file contains no drawable geometry."); goto done; }
    result = g_new0(Zen3dModel, 1); work.model = result;
    result->material_count = MAX(scene->mNumMaterials, 1);
    result->vertices = g_try_malloc0_n((gsize)work.vertex_count, sizeof(Zen3dVertex));
    result->indices = g_try_malloc_n((gsize)work.index_count, sizeof(uint32_t));
    result->parts = g_try_malloc0_n((gsize)work.part_count, sizeof(Zen3dPart));
    result->materials = g_try_malloc0_n(result->material_count, sizeof(Zen3dMaterial));
    work.uv_channels = g_try_malloc0_n(result->material_count, sizeof(guint));
    if (!result->vertices || !result->indices || !result->parts || !result->materials || !work.uv_channels) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "There is not enough memory for this model."); goto failed;
    }
    for (int axis = 0; axis < 3; axis++) {
        result->dimensions[axis] = work.maximum[axis] - work.minimum[axis];
        work.center[axis] = work.minimum[axis] + result->dimensions[axis] / 2;
        if (!isfinite(result->dimensions[axis]) || !isfinite(work.center[axis])) { invalid(&work, "The model dimensions are not finite."); goto failed; }
    }
    work.radius = hypot(hypot(result->dimensions[0], result->dimensions[1]), result->dimensions[2]) / 2;
    if (!(work.radius > DBL_MIN)) work.radius = 1;
    for (int axis = 0; axis < 3; axis++) {
        result->bounds_min[axis] = (float)((work.minimum[axis] - work.center[axis]) / work.radius);
        result->bounds_max[axis] = (float)((work.maximum[axis] - work.center[axis]) / work.radius);
    }
    if (!load_materials(&work) || !fill_node(&work, scene->mRootNode, identity)) goto failed;
    goto done;
failed:
    zen3d_model_free(result); result = NULL;
done:
    if (scene) aiReleaseImport(scene);
    g_free(work.uv_channels); g_hash_table_unref(work.textures);
    g_clear_error(&io.failure); g_hash_table_unref(io.seen); g_ptr_array_unref(io.search_dirs);
    g_free(io.directory); g_free(path);
    return result;
}

Zen3dModel *zen3d_model_load_with_feedback(GFile *file, GCancellable *cancel,
                                          const Zen3dLoadFeedback *feedback, GError **error) {
    /* The Assimp C API in supported releases has a process-global last-error
     * string. Keep it stable, and avoid parallel large scene allocations.
     * Only worker code waits here; format discovery never takes this lock. */
    static GMutex importer_mutex;
    if (cancelled(cancel, error)) return NULL;
    g_mutex_lock(&importer_mutex);
    /* load_model_serialized checks cancellation again immediately. */
    Zen3dModel *result = load_model_serialized(file, cancel, feedback, error);
    g_mutex_unlock(&importer_mutex);
    return result;
}

Zen3dModel *zen3d_model_load(GFile *file, GCancellable *cancel, GError **error) {
    return zen3d_model_load_with_feedback(file, cancel, NULL, error);
}

char *zen3d_model_formats(void) {
    struct aiString extensions = {0}; aiGetExtensionList(&extensions);
    return g_strndup(extensions.data, MIN(extensions.length, sizeof(extensions.data) - 1));
}
