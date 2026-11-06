/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "book.h"
#include <archive.h>
#include <archive_entry.h>
#include <libxml/parser.h>
#include <libxml/HTMLparser.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARCHIVE_LIMIT ((guint64)1024 * 1024 * 1024)
#define RESOURCE_LIMIT (64u * 1024 * 1024)
#define XML_LIMIT (8u * 1024 * 1024)
#define TEXT_LIMIT (2u * 1024 * 1024)
#define ENTRY_LIMIT 10000u
#define PATH_LIMIT 4096u
#define ITEM_LIMIT 65536u
#define NODE_LIMIT 200000u

typedef struct { gint64 size; } Entry;
typedef struct { char *path; GHashTable *entries, *media_types, *chapter_index; } BookPrivate;
typedef struct { struct archive *archive; int fd; } Zip;
typedef struct { char *path, *media, *properties; } ManifestItem;

static gboolean stopped(GCancellable *cancel, GError **error) {
    return cancel && g_cancellable_set_error_if_cancelled(cancel, error);
}
static void book_error(GError **error, GIOErrorEnum code, const char *text) {
    g_set_error_literal(error, G_IO_ERROR, code, text);
}
static void archive_error(Zip *zip, GError **error) {
    const char *reason = archive_error_string(zip->archive);
    char *valid = g_utf8_make_valid(reason ? reason : "The ZIP archive could not be read.", -1);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s", valid); g_free(valid);
}
static void zip_close(Zip *zip) {
    if (zip->archive) archive_read_free(zip->archive);
    if (zip->fd >= 0) close(zip->fd);
    zip->archive = NULL; zip->fd = -1;
}
static gboolean zip_open(const char *path, Zip *zip, GError **error) {
    zip->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (zip->fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot open book: %s", g_strerror(errno)); return FALSE;
    }
    struct stat status;
    if (fstat(zip->fd, &status) || !S_ISREG(status.st_mode)) {
        book_error(error, G_IO_ERROR_NOT_REGULAR_FILE, "Books must be regular local files."); zip_close(zip); return FALSE;
    }
    if (status.st_size < 0 || (guint64)status.st_size > ARCHIVE_LIMIT) {
        book_error(error, G_IO_ERROR_NO_SPACE, "This book exceeds the 1 GiB archive limit."); zip_close(zip); return FALSE;
    }
    zip->archive = archive_read_new();
    archive_read_support_filter_none(zip->archive);
    archive_read_support_format_zip(zip->archive);
    if (archive_read_open_fd(zip->archive, zip->fd, 65536) != ARCHIVE_OK) {
        archive_error(zip, error); zip_close(zip); return FALSE;
    }
    return TRUE;
}

/* Archive names never escape their virtual root; nothing is extracted to disk. */
static char *archive_path(const char *name) {
    if (!name || !*name || strlen(name) > PATH_LIMIT || !g_utf8_validate(name, -1, NULL) ||
        name[0] == '/' || name[0] == '\\' || strchr(name, ':')) return NULL;
    char *copy = g_strdup(name);
    for (char *p = copy; *p; p++) if (*p == '\\') *p = '/';
    char **parts = g_strsplit(copy, "/", -1);
    GString *out = g_string_new(NULL);
    gboolean valid = TRUE;
    for (guint i = 0; parts[i]; i++) {
        if (!strcmp(parts[i], "..")) { valid = FALSE; break; }
        if (!*parts[i] || !strcmp(parts[i], ".")) continue;
        if (out->len) g_string_append_c(out, '/');
        g_string_append(out, parts[i]);
    }
    g_strfreev(parts); g_free(copy);
    if (!valid || !out->len) { g_string_free(out, TRUE); return NULL; }
    return g_string_free(out, FALSE);
}

/* href paths may walk to an ancestor inside the ZIP. Returned resource paths
 * are decoded; link targets are escaped again so a filename '#' stays distinct
 * from a fragment delimiter. Remote resources are never read by this module. */
static char *resolve_path(const char *base, const char *href, char **fragment) {
    if (fragment) *fragment = NULL;
    if (!href || strlen(href) > PATH_LIMIT || href[0] == '/' || href[0] == '\\') return NULL;
    char *scheme = g_uri_parse_scheme(href);
    if (scheme) { g_free(scheme); return NULL; }
    const char *hash = strchr(href, '#');
    char *encoded = hash ? g_strndup(href, hash-href) : g_strdup(href);
    char *decoded = g_uri_unescape_string(encoded, NULL); g_free(encoded);
    if (!decoded || !g_utf8_validate(decoded, -1, NULL) || strchr(decoded, ':') ||
        decoded[0] == '/' || decoded[0] == '\\') { g_free(decoded); return NULL; }
    if (hash && fragment) {
        *fragment = g_uri_unescape_string(hash+1, NULL);
        if (*fragment && !g_utf8_validate(*fragment, -1, NULL)) g_clear_pointer(fragment, g_free);
    }
    if (!*decoded) { g_free(decoded); return g_strdup(base); }
    char *directory = g_path_get_dirname(base && *base ? base : ".");
    char *joined = !strcmp(directory, ".") ? g_strdup(decoded) : g_strconcat(directory, "/", decoded, NULL);
    g_free(directory); g_free(decoded);
    for (char *p = joined; *p; p++) if (*p == '\\') *p = '/';
    char **parts = g_strsplit(joined, "/", -1);
    GPtrArray *stack = g_ptr_array_new();
    gboolean valid = TRUE;
    for (guint i = 0; parts[i]; i++) {
        if (!*parts[i] || !strcmp(parts[i], ".")) continue;
        if (!strcmp(parts[i], "..")) {
            if (!stack->len) { valid = FALSE; break; }
            g_ptr_array_set_size(stack, stack->len-1);
        } else g_ptr_array_add(stack, parts[i]);
    }
    GString *result = g_string_new(NULL);
    if (valid) for (guint i = 0; i < stack->len; i++) {
        if (i) g_string_append_c(result, '/');
        g_string_append(result, g_ptr_array_index(stack, i));
    }
    g_ptr_array_unref(stack); g_strfreev(parts); g_free(joined);
    if (!valid || !result->len || result->len > PATH_LIMIT) {
        g_string_free(result, TRUE); if (fragment) g_clear_pointer(fragment, g_free); return NULL;
    }
    return g_string_free(result, FALSE);
}
static char *resolve_target(const char *base, const char *href, gboolean *external) {
    if (external) *external = FALSE;
    if (!href || strlen(href) > PATH_LIMIT) return NULL;
    char *scheme = g_uri_parse_scheme(href);
    if (scheme) {
        gboolean allowed = !g_ascii_strcasecmp(scheme, "https") || !g_ascii_strcasecmp(scheme, "http") || !g_ascii_strcasecmp(scheme, "mailto");
        g_free(scheme);
        if (allowed && external) { *external = TRUE; return g_strdup(href); }
        return NULL;
    }
    char *fragment = NULL, *path = resolve_path(base, href, &fragment);
    if (!path) { g_free(fragment); return NULL; }
    char *escaped = g_uri_escape_string(path, "/", TRUE);
    char *escaped_fragment = fragment ? g_uri_escape_string(fragment, NULL, TRUE) : NULL;
    char *target = escaped_fragment && *escaped_fragment ? g_strconcat(escaped, "#", escaped_fragment, NULL) : g_strdup(escaped);
    g_free(escaped); g_free(escaped_fragment); g_free(path); g_free(fragment); return target;
}

static void chapter_free(gpointer data) { ZenBookChapter *c = data; g_free(c->title); g_free(c->path); g_free(c); }
static void toc_free(gpointer data) { ZenBookTocEntry *t = data; g_free(t->title); g_free(t->target); g_free(t); }
static void manifest_free(gpointer data) {
    ManifestItem *m = data; g_free(m->path); g_free(m->media); g_free(m->properties); g_free(m);
}
ZenBook *zen_book_ref(ZenBook *book) { g_atomic_int_inc(&book->references); return book; }
void zen_book_unref(ZenBook *book) {
    if (!book || !g_atomic_int_dec_and_test(&book->references)) return;
    BookPrivate *data = book->private_data;
    if (data) {
        g_free(data->path); g_hash_table_unref(data->entries); g_hash_table_unref(data->media_types);
        g_hash_table_unref(data->chapter_index); g_free(data);
    }
    g_ptr_array_unref(book->chapters); g_ptr_array_unref(book->toc);
    g_free(book->title); g_free(book->author); g_free(book);
}

static GBytes *read_resource(ZenBook *book, const char *name, gsize limit, GCancellable *cancel, GError **error) {
    if (stopped(cancel, error)) return NULL;
    BookPrivate *private = book->private_data;
    Entry *entry = g_hash_table_lookup(private->entries, name);
    if (!entry) { book_error(error, G_IO_ERROR_NOT_FOUND, "This resource is not present in the book."); return NULL; }
    if (entry->size > (gint64)limit) { book_error(error, G_IO_ERROR_NO_SPACE, "This book resource exceeds its size limit."); return NULL; }
    Zip zip = {.fd = -1};
    if (!zip_open(private->path, &zip, error)) return NULL;
    GBytes *result = NULL;
    struct archive_entry *item;
    for (;;) {
        if (stopped(cancel, error)) break;
        int status = archive_read_next_header(zip.archive, &item);
        if (status == ARCHIVE_EOF) break;
        if (status != ARCHIVE_OK && status != ARCHIVE_WARN) { archive_error(&zip, error); break; }
        const char *raw = archive_entry_pathname_utf8(item);
        if (!raw) raw = archive_entry_pathname(item);
        char *path = archive_path(raw);
        gboolean match = path && !strcmp(path, name); g_free(path);
        if (!match) { archive_read_data_skip(zip.archive); continue; }
        if (archive_entry_is_encrypted(item)) { book_error(error, G_IO_ERROR_NOT_SUPPORTED, "Encrypted book resources are not supported."); break; }
        GByteArray *bytes = g_byte_array_sized_new(entry->size > 0 ? (guint)entry->size : 4096);
        guchar buffer[65536];
        gboolean okay = TRUE;
        for (;;) {
            if (stopped(cancel, error)) { okay = FALSE; break; }
            la_ssize_t count = archive_read_data(zip.archive, buffer, sizeof(buffer));
            if (count < 0) { archive_error(&zip, error); okay = FALSE; break; }
            if (!count) break;
            if ((gsize)count > limit - bytes->len) {
                book_error(error, G_IO_ERROR_NO_SPACE, "This book resource exceeds its size limit."); okay = FALSE; break;
            }
            g_byte_array_append(bytes, buffer, (guint)count);
        }
        if (okay) result = g_byte_array_free_to_bytes(bytes); else g_byte_array_unref(bytes);
        break;
    }
    if (!result && (!error || !*error)) book_error(error, G_IO_ERROR_NOT_FOUND, "This resource is no longer present in the book.");
    zip_close(&zip); return result;
}
GBytes *zen_book_read_resource(ZenBook *book, const char *name, GCancellable *cancel, GError **error) {
    char *safe = archive_path(name);
    if (!safe) { book_error(error, G_IO_ERROR_INVALID_ARGUMENT, "The archive resource path is invalid."); return NULL; }
    GBytes *bytes = read_resource(book, safe, RESOURCE_LIMIT, cancel, error); g_free(safe); return bytes;
}

static gboolean has_entity_declarations(xmlDoc *document) {
    xmlDtd *dtd = document->intSubset;
    if (!dtd) return FALSE;
    for (xmlNode *node = dtd->children; node; node = node->next)
        if (node->type == XML_ENTITY_DECL) return TRUE;
    return dtd->entities || dtd->pentities;
}
static xmlDoc *read_xml(ZenBook *book, const char *path, gboolean html, GCancellable *cancel, GError **error) {
    GBytes *bytes = read_resource(book, path, XML_LIMIT, cancel, error);
    if (!bytes) return NULL;
    gsize size; const char *data = g_bytes_get_data(bytes, &size);
    int flags = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_COMPACT;
    xmlDoc *document = xmlReadMemory(data, (int)size, NULL, NULL, flags);
    if (document && has_entity_declarations(document)) {
        xmlFreeDoc(document); document = NULL;
        book_error(error, G_IO_ERROR_NOT_SUPPORTED, "XML entity declarations are not supported in books.");
    }
    if (!document && html && (!error || !*error))
        document = htmlReadMemory(data, (int)size, NULL, NULL, HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT);
    g_bytes_unref(bytes);
    if (!document && (!error || !*error)) book_error(error, G_IO_ERROR_INVALID_DATA, "The book contains unreadable XML or HTML.");
    return document;
}
static gboolean named(const xmlNode *node, const char *name) {
    return node && node->type == XML_ELEMENT_NODE && !xmlStrcasecmp(node->name, BAD_CAST name);
}
static xmlNode *child(xmlNode *parent, const char *name) {
    for (xmlNode *node = parent ? parent->children : NULL; node; node = node->next) if (named(node, name)) return node;
    return NULL;
}
static xmlNode *descendant(xmlNode *parent, const char *name, guint depth) {
    if (!parent || depth > 64) return NULL;
    if (named(parent, name)) return parent;
    for (xmlNode *node = parent->children; node; node = node->next) {
        xmlNode *result = descendant(node, name, depth+1); if (result) return result;
    }
    return NULL;
}
static char *property(xmlNode *node, const char *name) {
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    char *result = value ? g_strdup((const char *)value) : NULL; xmlFree(value); return result;
}
static char *display_text(const char *text) {
    if (!text) return g_strdup("");
    GString *result = g_string_new(NULL);
    gboolean space = FALSE;
    for (const char *p = text; *p && result->len < 4096; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char_validated(p, -1);
        if (c == (gunichar)-1 || c == (gunichar)-2) break;
        if (g_unichar_isspace(c)) { space = result->len != 0; continue; }
        if (space) g_string_append_c(result, ' ');
        space = FALSE; g_string_append_unichar(result, c);
    }
    return g_string_free(result, FALSE);
}
static char *node_text(xmlNode *node) {
    xmlChar *content = node ? xmlNodeGetContent(node) : NULL;
    char *text = display_text((const char *)content); xmlFree(content); return text;
}
static gboolean token_present(const char *list, const char *token) {
    if (!list) return FALSE;
    char **parts = g_strsplit_set(list, " \t\r\n", -1); gboolean found = FALSE;
    for (guint i = 0; parts[i]; i++) if (!strcmp(parts[i], token)) found = TRUE;
    g_strfreev(parts); return found;
}

gint zen_book_find_chapter(const ZenBook *book, const char *target, char **fragment) {
    if (fragment) *fragment = NULL;
    if (!target) return -1;
    char *path = resolve_path("", target, fragment);
    if (!path) return -1;
    BookPrivate *private = book->private_data;
    gpointer found = g_hash_table_lookup(private->chapter_index, path);
    gint index = found ? (gint)GPOINTER_TO_UINT(found) - 1 : -1;
    g_free(path);
    if (index < 0 && fragment) g_clear_pointer(fragment, g_free);
    return index;
}
static void toc_add(ZenBook *book, char *title, char *target, guint depth) {
    if (!target || !*title || book->toc->len >= ENTRY_LIMIT || zen_book_find_chapter(book, target, NULL) < 0) {
        g_free(title); g_free(target); return;
    }
    ZenBookTocEntry *entry = g_new0(ZenBookTocEntry, 1);
    entry->title = title; entry->target = target; entry->depth = MIN(depth, 15);
    g_ptr_array_add(book->toc, entry);
}
static void nav_walk(ZenBook *book, xmlNode *node, const char *base, guint depth, guint *visited) {
    if (!node || depth > 32 || ++*visited > NODE_LIMIT) return;
    if (named(node, "a")) {
        char *href = property(node, "href");
        toc_add(book, node_text(node), resolve_target(base, href, NULL), depth ? depth-1 : 0); g_free(href);
        return;
    }
    for (xmlNode *part = node->children; part; part = part->next)
        nav_walk(book, part, base, depth + (named(part, "ol") || named(part, "ul")), visited);
}
static xmlNode *find_nav(xmlNode *node, guint depth, xmlNode **first) {
    if (!node || depth > 64) return NULL;
    if (named(node, "nav")) {
        if (!*first) *first = node;
        xmlChar *kind = xmlGetNsProp(node, BAD_CAST "type", BAD_CAST "http://www.idpf.org/2007/ops");
        gboolean toc = token_present((const char *)kind, "toc"); xmlFree(kind);
        if (toc) return node;
    }
    for (xmlNode *part = node->children; part; part = part->next) {
        xmlNode *result = find_nav(part, depth+1, first); if (result) return result;
    }
    return NULL;
}
static void ncx_walk(ZenBook *book, xmlNode *node, const char *base, guint depth, guint *visited) {
    if (!node || depth > 32 || ++*visited > NODE_LIMIT) return;
    for (xmlNode *part = node->children; part; part = part->next) if (named(part, "navPoint")) {
        xmlNode *content = child(part, "content"), *label = child(part, "navLabel");
        char *src = content ? property(content, "src") : NULL;
        toc_add(book, node_text(label), resolve_target(base, src, NULL), depth); g_free(src);
        ncx_walk(book, part, base, depth+1, visited);
    }
}
static void read_toc(ZenBook *book, const char *path, gboolean ncx, GCancellable *cancel) {
    GError *error = NULL;
    xmlDoc *document = read_xml(book, path, FALSE, cancel, &error);
    if (!document) { g_clear_error(&error); return; }
    guint visited = 0;
    if (ncx) ncx_walk(book, descendant(xmlDocGetRootElement(document), "navMap", 0), path, 0, &visited);
    else {
        xmlNode *first = NULL, *nav = find_nav(xmlDocGetRootElement(document), 0, &first);
        nav_walk(book, nav ? nav : first, path, 0, &visited);
    }
    xmlFreeDoc(document);
}

static gboolean parse_epub(ZenBook *book, GCancellable *cancel, GError **error) {
    BookPrivate *private = book->private_data;
    xmlDoc *container = read_xml(book, "META-INF/container.xml", FALSE, cancel, error);
    if (!container) return FALSE;
    xmlNode *rootfile = descendant(xmlDocGetRootElement(container), "rootfile", 0);
    char *full_path = rootfile ? property(rootfile, "full-path") : NULL;
    char *package_path = resolve_path("", full_path, NULL);
    g_free(full_path); xmlFreeDoc(container);
    if (!package_path) { book_error(error, G_IO_ERROR_INVALID_DATA, "The EPUB does not identify a valid package file."); return FALSE; }
    xmlDoc *package = read_xml(book, package_path, FALSE, cancel, error);
    if (!package) { g_free(package_path); return FALSE; }
    xmlNode *root = xmlDocGetRootElement(package), *metadata = child(root, "metadata"), *manifest = child(root, "manifest"), *spine = child(root, "spine");
    gboolean okay = FALSE;
    GHashTable *items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, manifest_free);
    if (!named(root, "package") || !manifest || !spine) {
        book_error(error, G_IO_ERROR_INVALID_DATA, "The EPUB package has no valid manifest and spine."); goto done;
    }
    char *title = node_text(child(metadata, "title"));
    if (*title) { g_free(book->title); book->title = title; } else g_free(title);
    GString *authors = g_string_new(NULL);
    for (xmlNode *node = metadata ? metadata->children : NULL; node; node = node->next) if (named(node, "creator")) {
        char *author = node_text(node);
        if (*author && authors->len < 8192) { if (authors->len) g_string_append(authors, ", "); g_string_append(authors, author); }
        g_free(author);
    }
    g_free(book->author); book->author = g_string_free(authors, FALSE);
    char *nav_path = NULL, *ncx_path = NULL, *ncx_id = property(spine, "toc");
    for (xmlNode *node = manifest->children; node; node = node->next) if (named(node, "item")) {
        if (stopped(cancel, error)) goto manifest_done;
        if (g_hash_table_size(items) >= ENTRY_LIMIT) { book_error(error, G_IO_ERROR_NO_SPACE, "The EPUB manifest is too large."); goto manifest_done; }
        char *id = property(node, "id"), *href = property(node, "href");
        ManifestItem *item = g_new0(ManifestItem, 1);
        item->path = resolve_path(package_path, href, NULL); item->media = property(node, "media-type"); item->properties = property(node, "properties");
        g_free(href);
        if (!id || !*id || !item->path || !g_hash_table_contains(private->entries, item->path)) { g_free(id); manifest_free(item); continue; }
        if (!nav_path && token_present(item->properties, "nav")) nav_path = g_strdup(item->path);
        if (!ncx_path && ((ncx_id && !strcmp(ncx_id, id)) || !g_strcmp0(item->media, "application/x-dtbncx+xml"))) ncx_path = g_strdup(item->path);
        g_hash_table_replace(private->media_types, g_strdup(item->path), g_strdup(item->media ? item->media : ""));
        g_hash_table_replace(items, id, item);
    }
    for (xmlNode *node = spine->children; node; node = node->next) if (named(node, "itemref")) {
        if (stopped(cancel, error)) goto manifest_done;
        char *idref = property(node, "idref");
        ManifestItem *item = idref ? g_hash_table_lookup(items, idref) : NULL; g_free(idref);
        if (!item || (g_strcmp0(item->media, "application/xhtml+xml") && g_strcmp0(item->media, "text/html") && g_strcmp0(item->media, "image/svg+xml"))) continue;
        if (book->chapters->len >= ENTRY_LIMIT) { book_error(error, G_IO_ERROR_NO_SPACE, "The EPUB spine is too large."); goto manifest_done; }
        ZenBookChapter *chapter = g_new0(ZenBookChapter, 1);
        chapter->path = g_strdup(item->path); chapter->title = g_path_get_basename(item->path);
        g_ptr_array_add(book->chapters, chapter);
        if (!g_hash_table_contains(private->chapter_index, chapter->path))
            g_hash_table_insert(private->chapter_index, g_strdup(chapter->path), GUINT_TO_POINTER(book->chapters->len));
    }
    if (!book->chapters->len) { book_error(error, G_IO_ERROR_INVALID_DATA, "The EPUB contains no readable chapters."); goto manifest_done; }
    if (nav_path) read_toc(book, nav_path, FALSE, cancel);
    if (!book->toc->len && ncx_path) read_toc(book, ncx_path, TRUE, cancel);
    GHashTable *titled = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint t = 0; t < book->toc->len; t++) {
        ZenBookTocEntry *entry = g_ptr_array_index(book->toc, t);
        gint index = zen_book_find_chapter(book, entry->target, NULL);
        gpointer key = GUINT_TO_POINTER(index+1);
        if (index >= 0 && !g_hash_table_contains(titled, key)) {
            ZenBookChapter *chapter = g_ptr_array_index(book->chapters, index);
            g_free(chapter->title); chapter->title = g_strdup(entry->title); g_hash_table_add(titled, key);
        }
    }
    g_hash_table_unref(titled);
    okay = !stopped(cancel, error);
manifest_done:
    g_free(nav_path); g_free(ncx_path); g_free(ncx_id);
done:
    g_hash_table_unref(items); xmlFreeDoc(package); g_free(package_path); return okay;
}

static gboolean image_name(const char *path) {
    if (g_str_has_prefix(path, "__MACOSX/")) return FALSE;
    const char *base = strrchr(path, '/'); base = base ? base+1 : path;
    if (g_str_has_prefix(base, "._")) return FALSE;
    const char *dot = strrchr(base, '.');
    if (!dot) return FALSE;
    const char *formats[] = {"jpg", "jpeg", "png", "webp", "gif", "bmp", "tif", "tiff", "avif", "heic", "heif", "jxl", "svg", "jp2", NULL};
    for (guint i = 0; formats[i]; i++) if (!g_ascii_strcasecmp(dot+1, formats[i])) return TRUE;
    return FALSE;
}
static gint chapter_compare(gconstpointer left, gconstpointer right, gpointer keys) {
    const ZenBookChapter *a = *(ZenBookChapter * const *)left, *b = *(ZenBookChapter * const *)right;
    const char *ka = g_hash_table_lookup(keys, a->path), *kb = g_hash_table_lookup(keys, b->path);
    gint result = strcmp(ka, kb);
    return result ? result : strcmp(a->path, b->path);
}
ZenBook *zen_book_open(GFile *file, GCancellable *cancel, GError **error) {
    if (stopped(cancel, error)) return NULL;
    char *path = g_file_get_path(file);
    if (!path) { book_error(error, G_IO_ERROR_NOT_SUPPORTED, "Only local EPUB and CBZ files are supported."); return NULL; }
    static gsize xml_initialized;
    if (g_once_init_enter(&xml_initialized)) { xmlInitParser(); g_once_init_leave(&xml_initialized, 1); }
    Zip zip = {.fd = -1};
    if (!zip_open(path, &zip, error)) { g_free(path); return NULL; }
    ZenBook *book = g_new0(ZenBook, 1); book->references = 1;
    book->chapters = g_ptr_array_new_with_free_func(chapter_free); book->toc = g_ptr_array_new_with_free_func(toc_free);
    char *basename = g_file_get_basename(file);
    book->title = g_filename_display_name(basename); g_free(basename); book->author = g_strdup("");
    BookPrivate *private = g_new0(BookPrivate, 1); book->private_data = private; private->path = path;
    private->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    private->media_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    private->chapter_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    struct archive_entry *entry;
    guint entries = 0, names_size = 0;
    gboolean okay = FALSE;
    for (;;) {
        if (stopped(cancel, error)) goto done;
        int status = archive_read_next_header(zip.archive, &entry);
        if (status == ARCHIVE_EOF) break;
        if (status != ARCHIVE_OK && status != ARCHIVE_WARN) { archive_error(&zip, error); goto done; }
        if (++entries > ENTRY_LIMIT) { book_error(error, G_IO_ERROR_NO_SPACE, "The book archive contains too many entries."); goto done; }
        const char *raw = archive_entry_pathname_utf8(entry);
        if (!raw) raw = archive_entry_pathname(entry);
        char *safe = archive_path(raw);
        if (!safe) { book_error(error, G_IO_ERROR_INVALID_DATA, "The book archive contains an unsafe entry path."); goto done; }
        names_size += (guint)strlen(safe);
        if (names_size > 4*1024*1024) { g_free(safe); book_error(error, G_IO_ERROR_NO_SPACE, "The book archive index is too large."); goto done; }
        if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry)) { g_free(safe); archive_read_data_skip(zip.archive); continue; }
        if (g_hash_table_contains(private->entries, safe)) { g_free(safe); book_error(error, G_IO_ERROR_INVALID_DATA, "The book archive contains duplicate entry paths."); goto done; }
        Entry *item = g_new0(Entry, 1); item->size = archive_entry_size_is_set(entry) ? archive_entry_size(entry) : -1;
        g_hash_table_insert(private->entries, safe, item);
        archive_read_data_skip(zip.archive);
    }
    zip_close(&zip);
    if (g_hash_table_contains(private->entries, "META-INF/container.xml")) {
        book->kind = ZEN_BOOK_EPUB;
        okay = parse_epub(book, cancel, error);
    } else {
        const char *extension = strrchr(path, '.');
        if (extension && !g_ascii_strcasecmp(extension, ".epub")) { book_error(error, G_IO_ERROR_INVALID_DATA, "The EPUB has no container metadata."); goto done; }
        book->kind = ZEN_BOOK_CBZ;
        GHashTable *sort_keys = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
        GHashTableIter iter; gpointer key;
        g_hash_table_iter_init(&iter, private->entries);
        while (g_hash_table_iter_next(&iter, &key, NULL)) if (image_name(key)) {
            ZenBookChapter *chapter = g_new0(ZenBookChapter, 1);
            chapter->path = g_strdup(key); chapter->title = g_path_get_basename(key); g_ptr_array_add(book->chapters, chapter);
            g_hash_table_insert(sort_keys, chapter->path, g_utf8_collate_key_for_filename(chapter->path, -1));
        }
        g_ptr_array_sort_with_data(book->chapters, chapter_compare, sort_keys); g_hash_table_unref(sort_keys);
        for (guint c = 0; c < book->chapters->len; c++) {
            ZenBookChapter *chapter = g_ptr_array_index(book->chapters, c);
            g_hash_table_insert(private->chapter_index, g_strdup(chapter->path), GUINT_TO_POINTER(c+1));
        }
        if (!book->chapters->len) book_error(error, G_IO_ERROR_INVALID_DATA, "The CBZ archive contains no image pages.");
        else okay = !stopped(cancel, error);
    }
done:
    zip_close(&zip);
    if (!okay) { zen_book_unref(book); return NULL; }
    return book;
}

static void link_free(gpointer data) { ZenBookLink *p = data; g_free(p->target); g_free(p); }
static void image_free(gpointer data) { ZenBookImage *p = data; g_free(p->path); g_free(p->alt); g_free(p); }
static void anchor_free(gpointer data) { ZenBookAnchor *p = data; g_free(p->id); g_free(p); }
void zen_book_document_free(ZenBookDocument *document) {
    if (!document) return;
    g_free(document->title); g_free(document->path); g_free(document->text);
    g_ptr_array_unref(document->spans); g_ptr_array_unref(document->links);
    g_ptr_array_unref(document->images); g_ptr_array_unref(document->anchors); g_free(document);
}
static ZenBookDocument *document_new(const ZenBookChapter *chapter) {
    ZenBookDocument *doc = g_new0(ZenBookDocument, 1);
    doc->title = g_strdup(chapter->title); doc->path = g_strdup(chapter->path);
    doc->spans = g_ptr_array_new_with_free_func(g_free); doc->links = g_ptr_array_new_with_free_func(link_free);
    doc->images = g_ptr_array_new_with_free_func(image_free); doc->anchors = g_ptr_array_new_with_free_func(anchor_free);
    return doc;
}
typedef struct {
    ZenBook *book; ZenBookDocument *doc; GString *text;
    GCancellable *cancel; GError **error;
    guint visited, chars; gboolean pending_space, failed;
} Reader;
static void append_raw(Reader *r, const char *text, gssize size) {
    if (r->failed) return;
    if (size < 0) size = (gssize)strlen(text);
    if ((gsize)size > TEXT_LIMIT - r->text->len) {
        book_error(r->error, G_IO_ERROR_NO_SPACE, "This chapter exceeds the 2 MiB text limit."); r->failed = TRUE; return;
    }
    g_string_append_len(r->text, text, size); r->chars += (guint)g_utf8_strlen(text, size);
}
static void append_text(Reader *r, const char *text, gboolean pre) {
    if (!text || r->failed) return;
    if (pre) { r->pending_space = FALSE; append_raw(r, text, -1); return; }
    for (const char *p = text; *p && !r->failed;) {
        gunichar c = g_utf8_get_char_validated(p, -1);
        if (c == (gunichar)-1 || c == (gunichar)-2) break;
        const char *next = g_utf8_next_char(p);
        if (g_unichar_isspace(c)) r->pending_space = TRUE;
        else {
            if (r->pending_space && r->text->len && r->text->str[r->text->len-1] != '\n') append_raw(r, " ", 1);
            r->pending_space = FALSE; append_raw(r, p, next-p);
        }
        p = next;
    }
}
static void line_break(Reader *r, guint count) {
    r->pending_space = FALSE;
    if (!r->text->len) return;
    guint present = 0;
    for (gsize i = r->text->len; i && r->text->str[i-1] == '\n'; i--) present++;
    while (present++ < count) append_raw(r, "\n", 1);
}
static gboolean block_node(const xmlNode *node) {
    const char *blocks[] = {"p", "div", "section", "article", "header", "footer", "aside", "address", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "pre", "figure", "figcaption", "dl", "dt", "dd", "table", NULL};
    for (guint i = 0; blocks[i]; i++) if (named(node, blocks[i])) return TRUE;
    return FALSE;
}
static void add_anchor(Reader *r, xmlNode *node) {
    char *id = property(node, "id");
    if (!id && named(node, "a")) id = property(node, "name");
    if (!id) return;
    if (strlen(id) <= PATH_LIMIT && r->doc->anchors->len < ITEM_LIMIT) {
        ZenBookAnchor *anchor = g_new0(ZenBookAnchor, 1); anchor->id = id; anchor->offset = r->chars;
        g_ptr_array_add(r->doc->anchors, anchor);
    } else g_free(id);
}
static void walk(Reader *r, xmlNode *node, guint depth, gboolean pre, guint list_number) {
    if (!node || r->failed) return;
    if (depth > 128 || ++r->visited > NODE_LIMIT) {
        book_error(r->error, G_IO_ERROR_NO_SPACE, "This chapter's document structure is too complex."); r->failed = TRUE; return;
    }
    if (!(r->visited % 256) && stopped(r->cancel, r->error)) { r->failed = TRUE; return; }
    if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) { append_text(r, (const char *)node->content, pre); return; }
    if (node->type != XML_ELEMENT_NODE) return;
    const char *skip[] = {"script", "style", "head", "iframe", "object", "embed", "audio", "video", "canvas", "form", "input", "button", "noscript", NULL};
    for (guint i = 0; skip[i]; i++) if (named(node, skip[i])) return;
    char *hidden = property(node, "hidden"), *aria = property(node, "aria-hidden");
    gboolean invisible = hidden != NULL || !g_strcmp0(aria, "true"); g_free(hidden); g_free(aria);
    if (invisible) return;
    gboolean block = block_node(node);
    if (block) line_break(r, 2);
    else if (named(node, "li") || named(node, "tr") || named(node, "ul") || named(node, "ol")) line_break(r, 1);
    add_anchor(r, node);
    guint start = r->chars, flags = 0, heading = 0;
    if (named(node, "b") || named(node, "strong")) flags |= ZEN_BOOK_BOLD;
    if (named(node, "i") || named(node, "em") || named(node, "cite")) flags |= ZEN_BOOK_ITALIC;
    if (named(node, "code") || named(node, "pre") || named(node, "kbd") || named(node, "samp")) flags |= ZEN_BOOK_MONOSPACE;
    if (named(node, "u")) flags |= ZEN_BOOK_UNDERLINE;
    if (named(node, "s") || named(node, "del") || named(node, "strike")) flags |= ZEN_BOOK_STRIKE;
    if (named(node, "blockquote")) flags |= ZEN_BOOK_QUOTE;
    if (node->name[0] == 'h' && node->name[1] >= '1' && node->name[1] <= '6' && !node->name[2]) {
        heading = node->name[1]-'0'; flags |= ZEN_BOOK_BOLD;
    }
    char *href = named(node, "a") ? property(node, "href") : NULL;
    if (named(node, "br")) { append_raw(r, "\n", 1); r->pending_space = FALSE; }
    else if (named(node, "hr")) { line_break(r, 2); append_raw(r, "—", -1); line_break(r, 2); }
    else if (named(node, "img") || named(node, "image")) {
        char *src = property(node, named(node, "img") ? "src" : "href"), *alt = property(node, "alt");
        if (!src && named(node, "image")) {
            xmlChar *value = xmlGetNsProp(node, BAD_CAST "href", BAD_CAST "http://www.w3.org/1999/xlink");
            src = value ? g_strdup((const char *)value) : NULL; xmlFree(value);
        }
        char *path = resolve_path(r->doc->path, src, NULL); g_free(src);
        BookPrivate *private = r->book->private_data;
        if (path && g_hash_table_contains(private->entries, path) && r->doc->images->len < 512) {
            if (r->pending_space && r->text->len && r->text->str[r->text->len-1] != '\n') append_raw(r, " ", 1);
            r->pending_space = FALSE;
            ZenBookImage *image = g_new0(ZenBookImage, 1); image->offset = r->chars; image->path = path; image->alt = alt ? alt : g_strdup("");
            g_ptr_array_add(r->doc->images, image); append_raw(r, "\357\277\274", 3);
        } else { append_text(r, alt, pre); g_free(path); g_free(alt); }
    } else {
        if (named(node, "li")) {
            char *prefix = list_number ? g_strdup_printf("%u. ", list_number) : g_strdup("• ");
            append_raw(r, prefix, -1); g_free(prefix);
        }
        guint number = 0;
        for (xmlNode *part = node->children; part && !r->failed; part = part->next) {
            guint item_number = named(node, "ol") && named(part, "li") ? ++number : 0;
            walk(r, part, depth+1, pre || named(node, "pre"), item_number);
        }
    }
    guint end = r->chars;
    if ((flags || heading) && end > start && r->doc->spans->len < ITEM_LIMIT) {
        ZenBookSpan *span = g_new0(ZenBookSpan, 1); span->start = start; span->end = end; span->flags = flags; span->heading_level = heading;
        g_ptr_array_add(r->doc->spans, span);
    }
    if (href && end > start && r->doc->links->len < ITEM_LIMIT) {
        gboolean external;
        char *target = resolve_target(r->doc->path, href, &external);
        if (target) {
            ZenBookLink *link = g_new0(ZenBookLink, 1); link->start = start; link->end = end; link->target = target; link->external = external;
            g_ptr_array_add(r->doc->links, link);
        }
    }
    g_free(href);
    if (block) line_break(r, 2);
    else if (named(node, "li") || named(node, "tr") || named(node, "ul") || named(node, "ol")) line_break(r, 1);
    else if (named(node, "td") || named(node, "th")) append_raw(r, "  ", 2);
}

ZenBookDocument *zen_book_load_chapter(ZenBook *book, guint chapter_index, GCancellable *cancel, GError **error) {
    if (stopped(cancel, error)) return NULL;
    if (chapter_index >= book->chapters->len) { book_error(error, G_IO_ERROR_INVALID_ARGUMENT, "The requested chapter does not exist."); return NULL; }
    ZenBookChapter *chapter = g_ptr_array_index(book->chapters, chapter_index);
    ZenBookDocument *doc = document_new(chapter);
    BookPrivate *private = book->private_data;
    const char *media = g_hash_table_lookup(private->media_types, chapter->path);
    if (book->kind == ZEN_BOOK_CBZ || !g_strcmp0(media, "image/svg+xml")) {
        ZenBookImage *image = g_new0(ZenBookImage, 1); image->path = g_strdup(chapter->path); image->alt = g_strdup(chapter->title);
        g_ptr_array_add(doc->images, image); doc->text = g_strdup("\357\277\274"); doc->characters = 1; return doc;
    }
    xmlDoc *xml = read_xml(book, chapter->path, TRUE, cancel, error);
    if (!xml) { zen_book_document_free(doc); return NULL; }
    xmlNode *root = xmlDocGetRootElement(xml), *body = descendant(root, "body", 0);
    xmlNode *title_node = descendant(root, "title", 0);
    char *title = node_text(title_node);
    if (*title) { g_free(doc->title); doc->title = title; } else g_free(title);
    Reader reader = {.book = book, .doc = doc, .text = g_string_new(NULL), .cancel = cancel, .error = error};
    walk(&reader, body ? body : root, 0, FALSE, 0);
    xmlFreeDoc(xml);
    if (reader.failed || stopped(cancel, error)) {
        g_string_free(reader.text, TRUE); zen_book_document_free(doc); return NULL;
    }
    /* Preserve offsets already assigned to spans, links and anchors. */
    doc->text = g_string_free(reader.text, FALSE); doc->characters = reader.chars;
    return doc;
}
