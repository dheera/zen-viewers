/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "model.h"

typedef struct Zen3dRenderer Zen3dRenderer;

typedef struct {
    float eye[3], center[3], up[3];
    gboolean orthographic;
    float ortho_height;       /* Full vertical span, in normalized model units. */
    float vertical_fov;       /* Degrees; zero selects 38 degrees. */
    gboolean wireframe;
    gboolean grid;
    gboolean interacting;    /* Use optional motion preview; idle keeps full detail. */
} Zen3dView;

/* CPU-only constructor. The renderer is main-thread only. */
Zen3dRenderer *zen3d_renderer_new(void);
/* Current desktop OpenGL 3.3 context required; safe to call again after release. */
gboolean zen3d_renderer_initialize(Zen3dRenderer *renderer, GError **error);
/* Takes ownership; no GL calls. NULL empties the scene. Upload happens in render.
 * An already-uploaded scene with at most 32 MiB of upload data remains visible until a replacement
 * can draw its coarse or full mesh; cleanup is deferred to the current context. */
void zen3d_renderer_set_model(Zen3dRenderer *renderer, Zen3dModel *model);
const Zen3dModel *zen3d_renderer_get_model(const Zen3dRenderer *renderer);
/* Draw into the caller's current framebuffer. Dimensions are physical pixels.
 * Keeps upload work bounded per call; request another frame while pending().
 * Call only with the GLArea context current and its framebuffer attached. */
gboolean zen3d_renderer_render(Zen3dRenderer *renderer, const Zen3dView *view,
                               int width, int height, GError **error);
gboolean zen3d_renderer_pending(const Zen3dRenderer *renderer);
double zen3d_renderer_progress(const Zen3dRenderer *renderer); /* 0..1 */
/* Uploaded full/preview index and vertex arrays and decoded pixbufs are released. Metadata stays
 * available through get_model(). After a later context loss, reload the source
 * file if needs_reload() is TRUE. Pending uploads retain their CPU data. */
gboolean zen3d_renderer_needs_reload(const Zen3dRenderer *renderer);
/* Current context required. Deletes all GL resources. */
void zen3d_renderer_release_gl(Zen3dRenderer *renderer);
/* CPU-only destructor. Call release_gl first if initialization succeeded. */
void zen3d_renderer_free(Zen3dRenderer *renderer);
