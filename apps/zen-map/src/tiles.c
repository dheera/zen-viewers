/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tiles.h"
#include <curl/curl.h>
#include <glib/gstdio.h>
#include <string.h>

#ifndef ZEN_VERSION
#define ZEN_VERSION "0.6.0"
#endif
#define TILE_LIMIT (2 * 1024 * 1024)
#define CACHE_FALLBACK (7 * 24 * 60 * 60)
#define MEMORY_TILES 160
#define MAX_REQUESTS 4
#define LOOKAHEAD_TILES 4

typedef struct {
    char *key;
    int z, x, y;
    GdkTexture *texture;
    GCancellable *cancel;
    guint wanted, used;
    double priority;
    gboolean lookahead;
    gint64 accessed, retry, expires;
} Tile;
struct ZenTiles {
    gint refs;
    GHashTable *items;
    guint frame, active, idle_timer, lookahead_count;
    gint64 settled_since;
    gboolean closed;
    char *url, *cache;
    ZenTilesChanged changed;
    gpointer data;
};
typedef struct { ZenTiles *store; Tile *tile; } Job;
typedef struct { GdkPixbuf *pixbuf; char *warning; gint64 expires; } Result;
typedef struct {
    GByteArray *bytes;
    GCancellable *cancel;
    char *etag, *modified;
    gint64 max_age, expires, date, age;
    gboolean no_store, no_cache, cache_control;
} Transfer;

static void tile_free(gpointer data) {
    Tile *t = data; g_free(t->key); g_clear_object(&t->texture); g_clear_object(&t->cancel); g_free(t);
}
static void store_unref(ZenTiles *s) {
    if (!g_atomic_int_dec_and_test(&s->refs)) return;
    g_hash_table_unref(s->items); g_free(s->url); g_free(s->cache); g_free(s);
}
static void job_free(gpointer data) { Job *j = data; store_unref(j->store); g_free(j); }
static void result_free(gpointer data) { Result *r = data; g_clear_object(&r->pixbuf); g_free(r->warning); g_free(r); }

ZenTiles *zen_tiles_new(ZenTilesChanged changed, gpointer data) {
    ZenTiles *s = g_new0(ZenTiles, 1); s->refs = 1;
    s->items = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, tile_free); s->changed = changed; s->data = data;
    const char *url = g_getenv("ZEN_MAP_TILE_URL");
    if (!url || !strstr(url, "{z}") || !strstr(url, "{x}") || !strstr(url, "{y}")) url = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
    s->url = g_strdup(url);
    char *id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, url, -1);
    s->cache = g_build_filename(g_get_user_cache_dir(), "zen-viewers", "maps", id, NULL); g_free(id);
    return s;
}
void zen_tiles_free(ZenTiles *s) {
    if (!s) return;
    s->closed = TRUE; s->changed = NULL; s->data = NULL;
    if (s->idle_timer) { g_source_remove(s->idle_timer); s->idle_timer = 0; }
    GHashTableIter i; gpointer value; g_hash_table_iter_init(&i, s->items);
    while (g_hash_table_iter_next(&i, NULL, &value)) { Tile *t = value; if (t->cancel) g_cancellable_cancel(t->cancel); }
    store_unref(s);
}
void zen_tiles_begin(ZenTiles *s) { s->frame++; s->lookahead_count = 0; }
static Tile *lookup(ZenTiles *s, int z, int x, int y, gboolean create) {
    if (z < 0 || z > 19 || y < 0 || y >= (1 << z)) return NULL;
    int count = 1 << z; x = ((x % count) + count) % count;
    char key[64]; g_snprintf(key, sizeof key, "%d-%d-%d", z, x, y);
    Tile *t = g_hash_table_lookup(s->items, key);
    if (!t && create) { t = g_new0(Tile, 1); t->key = g_strdup(key); t->z = z; t->x = x; t->y = y; g_hash_table_insert(s->items, t->key, t); }
    return t;
}
static void want(ZenTiles *s, Tile *t, double priority, gboolean lookahead) {
    if (t->wanted != s->frame) { t->wanted = s->frame; t->priority = priority; t->lookahead = lookahead; }
    else if (!lookahead || t->lookahead) { t->priority = MIN(t->priority, priority); t->lookahead = lookahead; }
}
static void retain(ZenTiles *s, Tile *t) { t->used = s->frame; t->accessed = g_get_monotonic_time(); }
void zen_tiles_lookahead(ZenTiles *s, int z, int x, int y, double priority) {
    if (s->lookahead_count >= LOOKAHEAD_TILES) return;
    Tile *t = lookup(s, z, x, y, TRUE); if (!t || t->wanted == s->frame) return;
    s->lookahead_count++; want(s, t, priority, TRUE);
}
void zen_tiles_snapshot(ZenTiles *s, GtkSnapshot *snapshot, int z, int x, int y,
                        const graphene_rect_t *r, double priority) {
    Tile *exact = lookup(s, z, x, y, TRUE); if (!exact) return;
    want(s, exact, priority, FALSE); retain(s, exact);
    if (exact->texture) { gtk_snapshot_append_texture(snapshot, exact->texture, r); return; }
    /* Clip a scaled ancestor to this tile's exact geographical extent. */
    gtk_snapshot_push_clip(snapshot, r);
    for (int parent_z = z - 1; parent_z >= 0; parent_z--) {
        int factor = 1 << (z - parent_z);
        Tile *parent = lookup(s, parent_z, exact->x / factor, y / factor, FALSE);
        if (!parent) continue;
        if (parent->texture || parent->cancel) retain(s, parent);
        if (!parent->texture) continue;
        graphene_rect_t parent_rect = GRAPHENE_RECT_INIT(r->origin.x - (exact->x % factor) * r->size.width,
            r->origin.y - (y % factor) * r->size.height, r->size.width * factor, r->size.height * factor);
        gtk_snapshot_append_texture(snapshot, parent->texture, &parent_rect); break;
    }
    /* On zoom-out, smaller already decoded tiles form a correctly positioned
     * mosaic. Coarser cached levels go first, then finer ones refine them. */
    for (int child_z = z + 1; child_z <= 19; child_z++) {
        int factor = 1 << (child_z - z); GHashTableIter it; gpointer value;
        g_hash_table_iter_init(&it, s->items);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            Tile *child = value;
            if (child->z != child_z || child->x / factor != exact->x || child->y / factor != y) continue;
            if (child->texture || child->cancel) retain(s, child);
            if (!child->texture) continue;
            graphene_rect_t child_rect = GRAPHENE_RECT_INIT(r->origin.x + (child->x % factor) * r->size.width / factor,
                r->origin.y + (child->y % factor) * r->size.height / factor, r->size.width / factor, r->size.height / factor);
            gtk_snapshot_append_texture(snapshot, child->texture, &child_rect);
        }
    }
    gtk_snapshot_pop(snapshot);
}
gboolean zen_tiles_busy(ZenTiles *s) {
    GHashTableIter it; gpointer value; g_hash_table_iter_init(&it, s->items);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        Tile *t = value;
        if (t->cancel && !t->lookahead && (t->wanted == s->frame || t->used == s->frame)) return TRUE;
    }
    return FALSE;
}

static size_t receive(char *data, size_t size, size_t count, void *user) {
    Transfer *t = user; gsize n = size * count;
    if (g_cancellable_is_cancelled(t->cancel) || n > TILE_LIMIT || t->bytes->len > TILE_LIMIT - n) return 0;
    g_byte_array_append(t->bytes, (guint8 *)data, (guint)n); return n;
}
static size_t headers(char *data, size_t size, size_t count, void *user) {
    Transfer *t = user; gsize n = size * count;
    if (n > 8192) return n;
    char *line = g_strndup(data, n), *colon = strchr(line, ':');
    if (!colon) { g_free(line); return n; }
    *colon++ = 0; char *value = g_strstrip(colon);
    if (!g_ascii_strcasecmp(line, "ETag") && strlen(value) < 512) { g_free(t->etag); t->etag = g_strdup(value); }
    else if (!g_ascii_strcasecmp(line, "Last-Modified") && strlen(value) < 128) { g_free(t->modified); t->modified = g_strdup(value); }
    else if (!g_ascii_strcasecmp(line, "Expires")) t->expires = curl_getdate(value, NULL);
    else if (!g_ascii_strcasecmp(line, "Date")) t->date = curl_getdate(value, NULL);
    else if (!g_ascii_strcasecmp(line, "Age")) t->age = MAX(0, g_ascii_strtoll(value, NULL, 10));
    else if (!g_ascii_strcasecmp(line, "Cache-Control")) {
        t->cache_control = TRUE;
        char **parts = g_strsplit(value, ",", -1);
        for (guint i = 0; parts[i]; i++) {
            char *p = g_strstrip(parts[i]);
            if (!g_ascii_strncasecmp(p, "max-age=", 8)) t->max_age = MAX(0, g_ascii_strtoll(p + 8 + (p[8] == '"'), NULL, 10));
            if (!g_ascii_strcasecmp(p, "no-store")) t->no_store = TRUE;
            if (!g_ascii_strcasecmp(p, "no-cache")) t->no_cache = TRUE;
        }
        g_strfreev(parts);
    }
    g_free(line); return n;
}
static int progress(void *user, curl_off_t total, curl_off_t done, curl_off_t upload, curl_off_t uploaded) {
    (void)total; (void)done; (void)upload; (void)uploaded;
    return g_cancellable_is_cancelled(user) ? 1 : 0;
}
static void png_size(GdkPixbufLoader *loader, int w, int h, gpointer user) {
    gboolean *valid = user; *valid = w == 256 && h == 256;
    if (!*valid) gdk_pixbuf_loader_set_size(loader, 1, 1);
}
static GdkPixbuf *decode(const guint8 *data, gsize length) {
    /* Bound decompression before invoking a loader, including sandboxed loaders
     * whose size-prepared notification may follow their initial allocation. */
    static const guint8 png_signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (length < 33 || memcmp(data, png_signature, 8) || memcmp(data + 12, "IHDR", 4) ||
        data[8] || data[9] || data[10] || data[11] != 13 ||
        data[16] || data[17] || data[18] != 1 || data[19] ||
        data[20] || data[21] || data[22] != 1 || data[23]) return NULL;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new_with_type("png", NULL);
    if (!loader) return NULL;
    gboolean valid = FALSE; g_signal_connect(loader, "size-prepared", G_CALLBACK(png_size), &valid);
    gboolean written = gdk_pixbuf_loader_write(loader, data, length, NULL);
    gboolean closed = gdk_pixbuf_loader_close(loader, NULL);
    GdkPixbuf *p = written && closed && valid ? gdk_pixbuf_loader_get_pixbuf(loader) : NULL;
    if (p) g_object_ref(p);
    g_object_unref(loader); return p;
}
static char *replace(const char *s, const char *key, int value) {
    char number[32]; g_snprintf(number, sizeof number, "%d", value);
    char **parts = g_strsplit(s, key, -1); char *result = g_strjoinv(number, parts); g_strfreev(parts); return result;
}
static gboolean safe_url(const char *url) {
    if (g_str_has_prefix(url, "https://")) return TRUE;
    GUri *u = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    const char *host = u ? g_uri_get_host(u) : NULL;
    gboolean local = u && !g_strcmp0(g_uri_get_scheme(u), "http") && host &&
        (!strcmp(host, "localhost") || !strcmp(host, "127.0.0.1") || !strcmp(host, "::1"));
    if (u) g_uri_unref(u);
    return local;
}
static char *limited_cached(const char *path, gsize *size) {
    GStatBuf st; char *bytes = NULL;
    if (!g_stat(path, &st) && st.st_size > 0 && st.st_size <= TILE_LIMIT) g_file_get_contents(path, &bytes, size, NULL);
    return bytes;
}
static void fetch(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; Job *j = data; ZenTiles *s = j->store; Tile *tile = j->tile;
    char *base = g_build_filename(s->cache, tile->key, NULL);
    char *png = g_strconcat(base, ".png", NULL), *ini = g_strconcat(base, ".ini", NULL);
    GKeyFile *meta = g_key_file_new(); g_key_file_load_from_file(meta, ini, G_KEY_FILE_NONE, NULL);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 expiry = g_key_file_get_int64(meta, "tile", "expires", NULL);
    gsize cached_size = 0; char *cached = limited_cached(png, &cached_size);
    GdkPixbuf *image = cached ? decode((guint8 *)cached, cached_size) : NULL;
    Result *result = g_new0(Result, 1);
    if (image && expiry > now) { result->pixbuf = image; image = NULL; result->expires = expiry; goto done; }
    if (g_cancellable_is_cancelled(cancel)) goto done;
    Transfer t = {.bytes = g_byte_array_new(), .cancel = cancel, .max_age = -1, .expires = -1, .date = -1};
    char *a = replace(s->url, "{z}", tile->z), *b = replace(a, "{x}", tile->x), *url = replace(b, "{y}", tile->y);
    g_free(a); g_free(b);
    CURL *curl = safe_url(url) ? curl_easy_init() : NULL;
    struct curl_slist *request_headers = NULL;
    char *etag = g_key_file_get_string(meta, "tile", "etag", NULL), *modified = g_key_file_get_string(meta, "tile", "modified", NULL);
    if (image && etag && !strpbrk(etag, "\r\n")) { char *h = g_strconcat("If-None-Match: ", etag, NULL); request_headers = curl_slist_append(request_headers, h); g_free(h); }
    if (image && modified && !strpbrk(modified, "\r\n")) { char *h = g_strconcat("If-Modified-Since: ", modified, NULL); request_headers = curl_slist_append(request_headers, h); g_free(h); }
    CURLcode code = CURLE_URL_MALFORMAT; long status = 0;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "zen-map/" ZEN_VERSION " (+https://github.com/dheera/zen-viewers)");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L); curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headers); curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
        if (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP2) curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        code = curl_easy_perform(curl); curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status); curl_easy_cleanup(curl);
    }
    if (!g_cancellable_is_cancelled(cancel) && code == CURLE_OK && (status == 200 || (status == 304 && image))) {
        if (status == 200) result->pixbuf = decode(t.bytes->data, t.bytes->len);
        else result->pixbuf = g_object_ref(image);
        if (result->pixbuf) {
            gint64 fallback = status == 304 && g_key_file_has_key(meta, "tile", "lifetime", NULL) ? g_key_file_get_int64(meta, "tile", "lifetime", NULL) : CACHE_FALLBACK;
            gint64 lifetime = t.max_age >= 0 ? t.max_age : t.expires >= 0 ? MAX(0, t.expires - (t.date > 0 ? t.date : now)) : fallback;
            if (status == 304 && !t.cache_control) t.no_cache = g_key_file_get_boolean(meta, "tile", "revalidate", NULL);
            gint64 age = MAX(t.age, t.date > 0 ? MAX(0, now - t.date) : 0);
            expiry = t.no_cache ? now : now + MAX(0, MIN(lifetime, (gint64)315360000) - age);
            result->expires = expiry;
            g_key_file_set_int64(meta, "tile", "expires", expiry);
            g_key_file_set_int64(meta, "tile", "lifetime", lifetime);
            g_key_file_set_boolean(meta, "tile", "revalidate", t.no_cache);
            if (t.etag) g_key_file_set_string(meta, "tile", "etag", t.etag);
            else if (status == 200) g_key_file_remove_key(meta, "tile", "etag", NULL);
            if (t.modified) g_key_file_set_string(meta, "tile", "modified", t.modified);
            else if (status == 200) g_key_file_remove_key(meta, "tile", "modified", NULL);
            if (!t.no_store && !g_mkdir_with_parents(s->cache, 0700)) {
                gboolean saved = status == 304 || g_file_set_contents(png, (char *)t.bytes->data, t.bytes->len, NULL);
                if (saved) g_key_file_save_to_file(meta, ini, NULL);
            }
        }
        if (t.no_store) { g_unlink(png); g_unlink(ini); }
    }
    if (!result->pixbuf && !g_cancellable_is_cancelled(cancel)) {
        result->pixbuf = image ? g_object_ref(image) : NULL;
        result->warning = g_strdup("Map tiles are unavailable. Your local map is still usable.");
    }
    g_free(etag); g_free(modified); g_free(url); curl_slist_free_all(request_headers);
    g_byte_array_unref(t.bytes); g_free(t.etag); g_free(t.modified);
done:
    g_clear_object(&image); g_free(cached); g_key_file_unref(meta); g_free(base); g_free(png); g_free(ini);
    g_task_return_pointer(task, result, result_free);
}
static void fetched(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source; (void)data;
    GTask *task = G_TASK(res); Job *j = g_task_get_task_data(task); ZenTiles *s = j->store; Tile *t = j->tile;
    GError *error = NULL; Result *r = g_task_propagate_pointer(task, &error);
    s->active--; g_clear_object(&t->cancel);
    if (!s->closed && r && r->pixbuf) { g_clear_object(&t->texture); t->texture = gdk_texture_new_for_pixbuf(r->pixbuf); t->expires = r->expires; }
    /* Do not spin on deliberately uncacheable responses; a revisit can revalidate. */
    if (r && r->pixbuf && t->expires <= g_get_real_time() / G_USEC_PER_SEC) t->retry = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
    if (r && r->warning) t->retry = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
    if (!s->closed && s->changed) s->changed(s->data, r && !t->lookahead && t->wanted == s->frame ? r->warning : NULL);
    g_clear_error(&error); if (r) result_free(r);
}
static gboolean idle_ready(gpointer data) {
    ZenTiles *s = data; s->idle_timer = 0;
    if (!s->closed && s->changed) s->changed(s->data, NULL);
    return G_SOURCE_REMOVE;
}
void zen_tiles_end(ZenTiles *s, gboolean enabled, gboolean settled) {
    GHashTableIter i; gpointer value; g_hash_table_iter_init(&i, s->items);
    gint64 now = g_get_monotonic_time();
    gint64 seconds = g_get_real_time() / G_USEC_PER_SEC;
    gboolean visible_missing = FALSE; guint nearby_active = 0;
    if (!settled || !enabled) s->settled_since = 0;
    else if (!s->settled_since) s->settled_since = now;
    while (g_hash_table_iter_next(&i, NULL, &value)) {
        Tile *t = value;
        if ((!enabled || (t->wanted != s->frame && t->used != s->frame)) && t->cancel) g_cancellable_cancel(t->cancel);
        if (t->wanted == s->frame && !t->lookahead && (!t->texture || t->expires <= seconds || t->cancel)) visible_missing = TRUE;
        if (t->cancel && t->lookahead) nearby_active++;
    }
    if (visible_missing) {
        g_hash_table_iter_init(&i, s->items);
        while (g_hash_table_iter_next(&i, NULL, &value)) {
            Tile *t = value;
            if (t->cancel && t->lookahead) g_cancellable_cancel(t->cancel);
        }
    }
    gboolean nearby = enabled && settled && !visible_missing && now - s->settled_since >= 180000;
    if (enabled && settled && !visible_missing && !nearby && !s->idle_timer)
        s->idle_timer = g_timeout_add(190, idle_ready, s);
    /* Exact visible tiles always win, ordered from the center out. A single
     * idle slot is available for at most four same-level adjacent tiles. */
    while (enabled && settled && s->active < MAX_REQUESTS) {
        Tile *best = NULL; g_hash_table_iter_init(&i, s->items);
        while (g_hash_table_iter_next(&i, NULL, &value)) {
            Tile *t = value;
            if (t->wanted != s->frame || t->cancel || t->retry > now || (t->texture && t->expires > seconds)) continue;
            if (t->lookahead && (!nearby || nearby_active)) continue;
            if (!best || (best->lookahead && !t->lookahead) || (best->lookahead == t->lookahead && t->priority < best->priority)) best = t;
        }
        if (!best) break;
        best->cancel = g_cancellable_new(); s->active++; if (best->lookahead) nearby_active++;
        Job *j = g_new0(Job, 1); j->store = s; j->tile = best; g_atomic_int_inc(&s->refs);
        GTask *task = g_task_new(NULL, best->cancel, fetched, NULL);
        g_task_set_task_data(task, j, job_free); g_task_run_in_thread(task, fetch); g_object_unref(task);
    }
    while (g_hash_table_size(s->items) > MEMORY_TILES) {
        Tile *oldest = NULL; g_hash_table_iter_init(&i, s->items);
        while (g_hash_table_iter_next(&i, NULL, &value)) {
            Tile *t = value;
            if (!t->cancel && t->wanted != s->frame && t->used != s->frame && (!oldest || t->accessed < oldest->accessed)) oldest = t;
        }
        if (!oldest) {
            /* A wide zoom-out can make every old tile a fallback. The snapshot
             * already retains its texture nodes; do not let that protection
             * grow the cache indefinitely. Keep active/current requests. */
            g_hash_table_iter_init(&i, s->items);
            while (g_hash_table_iter_next(&i, NULL, &value)) {
                Tile *t = value;
                if (!t->cancel && t->wanted != s->frame && (!oldest || t->accessed < oldest->accessed)) oldest = t;
            }
        }
        if (!oldest) break;
        g_hash_table_remove(s->items, oldest->key);
    }
}
