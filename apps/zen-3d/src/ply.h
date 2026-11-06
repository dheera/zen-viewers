/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "model.h"

/* Unsupported layouts return NULL with handled=FALSE and no error so the
 * general importer can handle them. A recognized but invalid file sets error. */
Zen3dModel *zen3d_ply_load(GFile *file, GCancellable *cancel, gboolean *handled,
                         const Zen3dLoadFeedback *feedback, GError **error);
