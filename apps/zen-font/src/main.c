/* SPDX-License-Identifier: GPL-3.0-or-later */
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_6
#include "zen-ui.h"
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <fontconfig/fcfreetype.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SFNT_NAMES_H
#include FT_FONT_FORMATS_H
#include FT_TRUETYPE_TABLES_H
#include <glib/gstdio.h>
#include <unistd.h>
#include <locale.h>
#include <string.h>
#include <math.h>
#ifndef ZEN_VERSION
#define ZEN_VERSION "0.5.0"
#endif

typedef struct { char tag[5]; char *name; double min, max, def; } Axis;
typedef struct {
    char *path, *temporary, *family, *style, *format, *designer, *version, *copyright;
    GPtrArray *faces;
    GArray *chars, *axes;
    guint face, render_face;
    long glyphs;
    FcConfig *config;
    FcPattern *pattern;
} Font;
typedef struct {
    ZenWindow *shell;
    GtkWidget *stack, *heading, *subtitle, *alphabet, *body, *size_label;
    GtkWidget *glyphs, *glyph_status, *glyph_toggle, *face_dropdown, *axis_box, *info;
    GtkWidget *settings, *search;
    GtkTextView *sample;
    GtkTextTag *sample_tag;
    GtkStringList *char_model;
    GtkSingleSelection *selection;
    GPtrArray *axis_scales;
    PangoFontMap *map;
    PangoFontDescription *description;
    Font *font;
    GCancellable *cancel;
    guint generation;
    double size;
    gboolean updating;
} Viewer;
typedef struct { char *path; guint face, generation; } Request;

static const char *default_sample = "The quiet art\nof everyday things.";
static void apply_type(Viewer *v);
static void load_path(Viewer *v, const char *path, guint face);

static char *safe_text(const char *text) { return g_utf8_make_valid(text ? text : "", -1); }
static void temporary_free(gpointer data) {
    char *path = data;
    if (path) { g_unlink(path); g_free(path); }
}
static gboolean is_webfont(const char *path) {
    FILE *file = g_fopen(path, "rb");
    if (!file) return FALSE;
    char magic[4]; size_t count = fread(magic, 1, sizeof(magic), file); fclose(file);
    return count == 4 && (!memcmp(magic, "wOFF", 4) || !memcmp(magic, "wOF2", 4));
}
static char *unpack_webfont(FT_Face face, guint *render_face, GError **error) {
    /* FreeType exposes WOFF/WOFF2's decompressed SFNT stream with table tag zero.
     * Pango's HarfBuzz file reader needs that stream, rather than the web wrapper. */
    FT_ULong length = 0;
    if (FT_Load_Sfnt_Table(face, 0, 0, NULL, &length) || length < 12 || length > 256u * 1024 * 1024) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The webfont could not be unpacked.");
        return NULL;
    }
    g_autofree guchar *bytes = g_malloc(length);
    if (FT_Load_Sfnt_Table(face, 0, 0, bytes, &length) ||
        (memcmp(bytes, "\0\1\0\0", 4) && memcmp(bytes, "OTTO", 4) && memcmp(bytes, "ttcf", 4))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The webfont has an unsupported font stream.");
        return NULL;
    }
    if (memcmp(bytes, "ttcf", 4)) *render_face = 0;
    char *path = NULL;
    int fd = g_file_open_tmp("zen-font-XXXXXX", &path, error);
    if (fd < 0) return NULL;
    close(fd);
    if (!g_file_set_contents(path, (const char *)bytes, length, error)) { temporary_free(path); return NULL; }
    return path;
}
static char *sfnt_name(FT_Face face, FT_UShort id) {
    char *best = NULL;
    int score = -1;
    for (FT_UInt i = 0; i < FT_Get_Sfnt_Name_Count(face); i++) {
        FT_SfntName name;
        if (FT_Get_Sfnt_Name(face, i, &name) || name.name_id != id || name.string_len > 65536) continue;
        int priority = name.platform_id == 3 ? (name.language_id == 0x409 ? 3 : 1) :
                       name.platform_id == 0 ? 2 : 0;
        if (priority <= score) continue;
        char *value = NULL;
        if (name.platform_id == 3 || name.platform_id == 0)
            value = g_convert((char *)name.string, name.string_len, "UTF-8", "UTF-16BE", NULL, NULL, NULL);
        else if (name.platform_id == 1)
            value = g_convert((char *)name.string, name.string_len, "UTF-8", "MACINTOSH", NULL, NULL, NULL);
        if (value) { g_free(best); best = value; score = priority; }
    }
    return best ? best : g_strdup("");
}
static void font_free(gpointer data) {
    Font *font = data;
    if (!font) return;
    g_free(font->path); temporary_free(font->temporary);
    g_free(font->family); g_free(font->style); g_free(font->format);
    g_free(font->designer); g_free(font->version); g_free(font->copyright);
    if (font->faces) g_ptr_array_unref(font->faces);
    if (font->chars) g_array_unref(font->chars);
    if (font->axes) {
        for (guint i = 0; i < font->axes->len; i++) g_free(g_array_index(font->axes, Axis, i).name);
        g_array_unref(font->axes);
    }
    if (font->config) FcConfigDestroy(font->config);
    if (font->pattern) FcPatternDestroy(font->pattern);
    g_free(font);
}
static Font *font_load(const char *path, guint index, GCancellable *cancel, GError **error) {
    FT_Library library = NULL;
    FT_Face face = NULL;
    Font *font = NULL;
    if (g_cancellable_set_error_if_cancelled(cancel, error)) return NULL;
    if (FT_Init_FreeType(&library) || FT_New_Face(library, path, index, &face)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "This file could not be read as a font.");
        goto done;
    }
    if (!FT_IS_SFNT(face)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "Open a TrueType, OpenType or webfont. Legacy Type 1 and bitmap files are not supported.");
        goto done;
    }
    if (face->num_faces > 1024 || face->num_glyphs > 2000000) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "This font exceeds the viewer's limits.");
        goto done;
    }
    font = g_new0(Font, 1);
    font->path = g_strdup(path); font->face = index; font->render_face = index; font->glyphs = face->num_glyphs;
    font->family = safe_text(face->family_name); font->style = safe_text(face->style_name);
    font->format = safe_text(FT_Get_Font_Format(face));
    font->designer = sfnt_name(face, 9); font->version = sfnt_name(face, 5); font->copyright = sfnt_name(face, 0);
    font->chars = g_array_new(FALSE, FALSE, sizeof(gunichar));
    font->axes = g_array_new(FALSE, FALSE, sizeof(Axis));
    font->faces = g_ptr_array_new_with_free_func(g_free);
    for (long i = 0; i < face->num_faces; i++) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) goto failed;
        FT_Face item = NULL;
        if (!FT_New_Face(library, path, i, &item)) {
            g_autofree char *family = safe_text(item->family_name), *style = safe_text(item->style_name);
            g_ptr_array_add(font->faces, g_strdup_printf("%s · %s", family, style));
            FT_Done_Face(item);
        } else g_ptr_array_add(font->faces, g_strdup_printf("Face %ld", i + 1));
    }
    if (!FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
        FT_UInt glyph;
        FT_ULong code = FT_Get_First_Char(face, &glyph);
        while (glyph) {
            if (code > 0 && code <= 0x10ffff && g_unichar_validate(code) && !g_unichar_iscntrl(code)) {
                gunichar character = code;
                g_array_append_val(font->chars, character);
            }
            code = FT_Get_Next_Char(face, code, &glyph);
            if (!(font->chars->len % 1024) && g_cancellable_set_error_if_cancelled(cancel, error)) goto failed;
        }
    }
    FT_MM_Var *mm = NULL;
    if (!FT_Get_MM_Var(face, &mm)) {
        for (FT_UInt i = 0; i < MIN(mm->num_axis, 64u); i++) {
            FT_Var_Axis *item = &mm->axis[i];
            Axis axis = { .min = item->minimum / 65536., .max = item->maximum / 65536., .def = item->def / 65536. };
            for (int j = 0; j < 4; j++) axis.tag[j] = (item->tag >> (24 - j * 8)) & 255;
            axis.name = sfnt_name(face, item->strid);
            if (!*axis.name) { g_free(axis.name); axis.name = safe_text(item->name); }
            g_array_append_val(font->axes, axis);
        }
        FT_Done_MM_Var(library, mm);
    }
    if (is_webfont(path)) {
        font->temporary = unpack_webfont(face, &font->render_face, error);
        if (!font->temporary) goto failed;
    }
    const char *render_path = font->temporary ? font->temporary : path;
    int count = 0;
    font->pattern = FcFreeTypeQuery((const FcChar8 *)render_path, font->render_face, NULL, &count);
    font->config = FcConfigCreate();
    if (!font->pattern || !font->config || !FcConfigAppFontAddFile(font->config, (const FcChar8 *)render_path)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The font renderer could not open this file.");
        goto failed;
    }
    /* Private aliases keep collection faces and installed names from colliding. */
    FcFontSet *set = FcConfigGetFonts(font->config, FcSetApplication);
    for (int i = 0; set && i < set->nfont; i++) {
        int face_index = 0;
        FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &face_index);
        g_autofree char *alias = g_strdup_printf("Zen Specimen %u", (guint)face_index & 0xffff);
        FcPatternDel(set->fonts[i], FC_FAMILY);
        FcPatternAddString(set->fonts[i], FC_FAMILY, (const FcChar8 *)alias);
    }
    if (g_cancellable_set_error_if_cancelled(cancel, error)) goto failed;
    goto done;
failed:
    font_free(font); font = NULL;
done:
    if (face) FT_Done_Face(face);
    if (library) FT_Done_FreeType(library);
    return font;
}

static void request_free(gpointer data) { Request *r = data; g_free(r->path); g_free(r); }
static void load_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    (void)object; Request *r = data; GError *error = NULL;
    Font *font = font_load(r->path, r->face, cancel, &error);
    if (font) g_task_return_pointer(task, font, font_free);
    else g_task_return_error(task, error);
}
static PangoAttrList *font_attrs(Viewer *v, double size) {
    PangoAttrList *attrs = pango_attr_list_new();
    if (v->description) {
        PangoFontDescription *desc = pango_font_description_copy(v->description);
        pango_font_description_set_size(desc, size * PANGO_SCALE);
        pango_attr_list_insert(attrs, pango_attr_font_desc_new(desc));
        pango_attr_list_insert(attrs, pango_attr_fallback_new(FALSE));
        pango_font_description_free(desc);
    }
    return attrs;
}
static void style_label(Viewer *v, GtkWidget *label, double size) {
    gtk_widget_set_font_map(label, v->map);
    PangoAttrList *attrs = font_attrs(v, size);
    gtk_label_set_attributes(GTK_LABEL(label), attrs);
    pango_attr_list_unref(attrs);
}
static void tag_sample(Viewer *v) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(v->sample);
    GtkTextIter begin, end;
    gtk_text_buffer_get_bounds(buffer, &begin, &end);
    gtk_text_buffer_apply_tag(buffer, v->sample_tag, &begin, &end);
}
static void text_changed(GtkTextBuffer *buffer, gpointer data) { (void)buffer; tag_sample(data); }
static void glyph_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory; (void)data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(box, "font-glyph");
    GtkWidget *glyph = gtk_label_new(NULL), *code = gtk_label_new(NULL);
    gtk_widget_add_css_class(glyph, "font-character");
    gtk_widget_set_size_request(glyph, 74, 64);
    gtk_widget_add_css_class(code, "font-code");
    gtk_box_append(GTK_BOX(box), glyph); gtk_box_append(GTK_BOX(box), code);
    g_object_set_data(G_OBJECT(box), "glyph", glyph); g_object_set_data(G_OBJECT(box), "code", code);
    gtk_list_item_set_child(item, box);
}
static void glyph_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory; Viewer *v = data;
    GtkWidget *box = gtk_list_item_get_child(item);
    GtkWidget *glyph = g_object_get_data(G_OBJECT(box), "glyph"), *code = g_object_get_data(G_OBJECT(box), "code");
    const char *text = gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item)));
    gunichar ch = g_utf8_get_char(text);
    g_autofree char *caption = g_strdup_printf("U+%04X", ch);
    gtk_label_set_text(GTK_LABEL(glyph), text); gtk_label_set_text(GTK_LABEL(code), caption);
    style_label(v, glyph, 32);
    gtk_widget_set_tooltip_text(box, "Click to copy this character");
}
static void glyph_activated(GtkGridView *grid, guint position, gpointer data) {
    (void)grid; Viewer *v = data;
    const char *text = gtk_string_list_get_string(v->char_model, position);
    if (!text) return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(v->glyphs), text);
    g_autofree char *message = g_strdup_printf("Copied U+%04X", g_utf8_get_char(text));
    gtk_label_set_text(GTK_LABEL(v->glyph_status), message);
}
static void filter_glyphs(Viewer *v) {
    if (!v->font) return;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(v->search));
    gunichar wanted = 0;
    if (g_str_has_prefix(query, "U+") || g_str_has_prefix(query, "u+")) {
        char *end; guint64 code = g_ascii_strtoull(query + 2, &end, 16);
        if (*query && !*end && code <= 0x10ffff) wanted = code;
    } else if (*query) wanted = g_utf8_get_char(query);
    GPtrArray *strings = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < v->font->chars->len; i++) {
        gunichar ch = g_array_index(v->font->chars, gunichar, i);
        if (*query && wanted != ch) continue;
        char utf8[7] = {0}; g_unichar_to_utf8(ch, utf8);
        g_ptr_array_add(strings, g_strdup(utf8));
    }
    guint count = strings->len;
    g_ptr_array_add(strings, NULL);
    gtk_string_list_splice(v->char_model, 0, g_list_model_get_n_items(G_LIST_MODEL(v->char_model)),
                           (const char *const *)strings->pdata);
    g_ptr_array_unref(strings);
    g_autofree char *caption = g_strdup_printf("%u Unicode characters  ·  Click a glyph to copy", count);
    gtk_label_set_text(GTK_LABEL(v->glyph_status), caption);
}
static void search_changed(GtkSearchEntry *entry, gpointer data) { (void)entry; filter_glyphs(data); }
static void restyle_glyphs(Viewer *v, GtkWidget *widget) {
    if (gtk_widget_has_css_class(widget, "font-character")) style_label(v, widget, 32);
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        restyle_glyphs(v, child);
}
static void apply_type(Viewer *v) {
    if (!v->font || !v->description) return;
    GString *variations = g_string_new(NULL);
    for (guint i = 0; i < v->axis_scales->len; i++) {
        Axis *axis = &g_array_index(v->font->axes, Axis, i);
        char value[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(value, sizeof(value), gtk_range_get_value(g_ptr_array_index(v->axis_scales, i)));
        g_string_append_printf(variations, "%s%s=%s", i ? "," : "", axis->tag, value);
    }
    pango_font_description_set_variations(v->description, variations->str);
    g_string_free(variations, TRUE);
    pango_font_description_set_size(v->description, v->size * PANGO_SCALE);
    gtk_widget_set_font_map(GTK_WIDGET(v->sample), v->map);
    /* TextView keeps separate bidirectional contexts, created before async loading. */
    pango_context_set_font_map(gtk_text_view_get_ltr_context(v->sample), v->map);
    pango_context_set_font_map(gtk_text_view_get_rtl_context(v->sample), v->map);
    g_object_set(v->sample_tag, "font-desc", v->description, "fallback", FALSE, NULL);
    tag_sample(v);
    style_label(v, v->alphabet, 22); style_label(v, v->body, 14);
    g_autofree char *size = g_strdup_printf("%.0f pt", v->size);
    gtk_label_set_text(GTK_LABEL(v->size_label), size);
    /* Only allocated cells need restyling, even in fonts with tens of thousands of glyphs. */
    restyle_glyphs(v, v->glyphs);
}
static void axis_changed(GtkRange *range, gpointer data) { (void)range; Viewer *v = data; if (!v->updating) apply_type(v); }
static void size_change(Viewer *v, double delta) { v->size = CLAMP(v->size + delta, 12., 160.); apply_type(v); }
static void smaller(GtkButton *button, gpointer data) { (void)button; size_change(data, -4); }
static void larger(GtkButton *button, gpointer data) { (void)button; size_change(data, 4); }
static void reset_sample(GtkButton *button, gpointer data) {
    (void)button; Viewer *v = data;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(v->sample), default_sample, -1);
    v->size = 52; v->updating = TRUE;
    for (guint i = 0; v->font && i < v->axis_scales->len; i++)
        gtk_range_set_value(g_ptr_array_index(v->axis_scales, i), g_array_index(v->font->axes, Axis, i).def);
    v->updating = FALSE; apply_type(v);
}
static void toggle_glyphs(GtkToggleButton *button, gpointer data) {
    Viewer *v = data;
    gboolean show = gtk_toggle_button_get_active(button);
    gtk_stack_set_visible_child_name(GTK_STACK(v->stack), show ? "glyphs" : "specimen");
    if (show) filter_glyphs(v);
}
static void face_changed(GObject *dropdown, GParamSpec *param, gpointer data) {
    (void)param; Viewer *v = data;
    if (v->updating || !v->font) return;
    guint face = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (face != GTK_INVALID_LIST_POSITION && face != v->font->face) load_path(v, v->font->path, face);
}
static void populate_settings(Viewer *v) {
    v->updating = TRUE;
    GtkStringList *faces = gtk_string_list_new(NULL);
    for (guint i = 0; i < v->font->faces->len; i++) gtk_string_list_append(faces, g_ptr_array_index(v->font->faces, i));
    gtk_drop_down_set_model(GTK_DROP_DOWN(v->face_dropdown), G_LIST_MODEL(faces));
    g_object_unref(faces);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(v->face_dropdown), v->font->face);
    gtk_widget_set_visible(v->face_dropdown, v->font->faces->len > 1);
    g_ptr_array_set_size(v->axis_scales, 0);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(v->axis_box))) gtk_box_remove(GTK_BOX(v->axis_box), child);
    for (guint i = 0; i < v->font->axes->len; i++) {
        Axis *axis = &g_array_index(v->font->axes, Axis, i);
        GtkWidget *label = gtk_label_new(axis->name);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_add_css_class(label, "font-axis-name");
        gtk_box_append(GTK_BOX(v->axis_box), label);
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, axis->min, MAX(axis->max, axis->min + .01), MAX((axis->max-axis->min)/200, .01));
        gtk_scale_set_digits(GTK_SCALE(scale), axis->max - axis->min <= 10 ? 2 : 0);
        gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
        gtk_range_set_value(GTK_RANGE(scale), axis->def);
        gtk_widget_set_sensitive(scale, axis->max > axis->min);
        gtk_accessible_update_property(GTK_ACCESSIBLE(scale), GTK_ACCESSIBLE_PROPERTY_LABEL, axis->name, -1);
        gtk_box_append(GTK_BOX(v->axis_box), scale);
        g_ptr_array_add(v->axis_scales, scale);
        g_signal_connect(scale, "value-changed", G_CALLBACK(axis_changed), v);
    }
    g_autofree char *info = g_strdup_printf("%s\n%ld glyphs · %u Unicode characters%s%s%s%s\n\n%s",
        v->font->format, v->font->glyphs, v->font->chars->len,
        *v->font->designer ? "\n" : "", v->font->designer,
        *v->font->version ? "\n" : "", v->font->version, v->font->copyright);
    gtk_label_set_text(GTK_LABEL(v->info), info);
    v->updating = FALSE;
}
static void loaded(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data; Viewer *v = g_object_get_data(source, "font-viewer");
    GTask *task = G_TASK(result); Request *request = g_task_get_task_data(task);
    GError *error = NULL; Font *font = g_task_propagate_pointer(task, &error);
    if (!v || zen_window_is_closed(v->shell) || request->generation != v->generation) {
        font_free(font); g_clear_error(&error); return;
    }
    zen_window_hide_progress(v->shell);
    if (!font) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) zen_window_notice(v->shell, error->message);
        g_clear_error(&error); return;
    }
    font_free(v->font); v->font = font;
    g_clear_object(&v->map);
    g_clear_pointer(&v->description, pango_font_description_free);
    v->map = pango_cairo_font_map_new();
    pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(v->map), font->config);
    if (font->temporary)
        g_object_set_data_full(G_OBJECT(v->map), "font-file", g_steal_pointer(&font->temporary), temporary_free);
    v->description = pango_fc_font_description_from_pattern(font->pattern, FALSE);
    g_autofree char *alias = g_strdup_printf("Zen Specimen %u", font->render_face);
    pango_font_description_set_family(v->description, alias);
    gtk_label_set_text(GTK_LABEL(v->heading), font->family);
    g_autofree char *sub = g_strdup_printf("%s  /  %s%s", font->style, font->format, font->axes->len ? "  /  Variable" : "");
    gtk_label_set_text(GTK_LABEL(v->subtitle), sub);
    populate_settings(v); apply_type(v); filter_glyphs(v);
    g_autofree char *basename = g_path_get_basename(font->path);
    zen_window_set_title(v->shell, basename);
    gtk_widget_set_visible(v->stack, TRUE);
    zen_window_clear_notice(v->shell); zen_window_set_ready(v->shell, TRUE);
    gtk_window_set_focus(zen_window_get_window(v->shell), NULL);
}
static void load_path(Viewer *v, const char *path, guint face) {
    if (v->cancel) g_cancellable_cancel(v->cancel);
    g_clear_object(&v->cancel); v->cancel = g_cancellable_new(); v->generation++;
    Request *request = g_new0(Request, 1);
    request->path = g_strdup(path); request->face = face; request->generation = v->generation;
    zen_window_show_progress(v->shell, -1);
    GTask *task = g_task_new(zen_window_get_window(v->shell), v->cancel, loaded, NULL);
    g_task_set_task_data(task, request, request_free); g_task_run_in_thread(task, load_worker); g_object_unref(task);
}
static void open_file(ZenWindow *shell, GFile *file, gpointer data) {
    Viewer *v = data; g_autofree char *path = g_file_get_path(file);
    if (!path) { zen_window_notice(shell, "Open a local font file."); return; }
    load_path(v, path, 0);
}
static gboolean closing(GtkWindow *window, gpointer data) {
    (void)window; Viewer *v = data; v->generation++;
    if (v->cancel) g_cancellable_cancel(v->cancel);
    return FALSE;
}
static void viewer_free(gpointer data) {
    Viewer *v = data;
    if (v->cancel) g_cancellable_cancel(v->cancel);
    g_clear_object(&v->cancel); g_clear_object(&v->map);
    g_clear_object(&v->char_model); g_clear_pointer(&v->description, pango_font_description_free);
    g_ptr_array_unref(v->axis_scales); font_free(v->font); g_free(v);
}
static gboolean claim_content(double x, double y, gpointer data) {
    Viewer *v = data;
    GtkWidget *widget = gtk_widget_pick(zen_window_get_root(v->shell), x, y, GTK_PICK_DEFAULT);
    return widget && ((GTK_IS_LABEL(widget) && gtk_label_get_selectable(GTK_LABEL(widget))) ||
           widget == GTK_WIDGET(v->sample) || gtk_widget_is_ancestor(widget, GTK_WIDGET(v->sample)) ||
           widget == v->glyphs || gtk_widget_is_ancestor(widget, v->glyphs));
}
static gboolean key_pressed(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer data) {
    (void)controller; (void)code; Viewer *v = data;
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    if (key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_KP_Add) { size_change(v, 4); return TRUE; }
    if (key == GDK_KEY_minus || key == GDK_KEY_KP_Subtract) { size_change(v, -4); return TRUE; }
    if (key == GDK_KEY_g) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->glyph_toggle), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->glyph_toggle)));
        return TRUE;
    }
    if (key == GDK_KEY_r) { reset_sample(NULL, v); return TRUE; }
    if (key == GDK_KEY_i) { gtk_menu_button_popup(GTK_MENU_BUTTON(v->settings)); return TRUE; }
    return FALSE;
}
static GtkWidget *caption(const char *text, const char *class_name) {
    GtkWidget *label = gtk_label_new(text); gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (class_name) gtk_widget_add_css_class(label, class_name);
    return label;
}
static Viewer *new_viewer(GtkApplication *app) {
    Viewer *v = g_new0(Viewer, 1); v->size = 52; v->axis_scales = g_ptr_array_new();
    v->shell = zen_window_new(app, "Zen Font", 1000, 820);
    GtkWindow *window = zen_window_get_window(v->shell);
    g_object_set_data_full(G_OBJECT(window), "font-viewer", v, viewer_free);
    g_signal_connect(window, "close-request", G_CALLBACK(closing), v);
    zen_window_set_open_handler(v->shell, open_file, v); zen_window_set_pointer_claim_func(v->shell, claim_content, v);
    zen_window_set_empty_text(v->shell, "Zen Font", "A little room for type. Open a font or drop one here.");
    const char *mimes[] = {"font/ttf", "font/otf", "font/collection", "font/woff", "font/woff2", "application/x-font-ttf", "application/x-font-otf", NULL};
    const char *patterns[] = {"*.ttf", "*.otf", "*.ttc", "*.otc", "*.woff", "*.woff2", "*.TTF", "*.OTF", "*.TTC", "*.OTC", "*.WOFF", "*.WOFF2", NULL};
    zen_window_add_file_filter(v->shell, "Font files", mimes, patterns);
    v->stack = gtk_stack_new(); gtk_widget_add_css_class(v->stack, "font-paper");
    gtk_stack_set_transition_type(GTK_STACK(v->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(v->stack), 140);
    zen_window_set_child(v->shell, v->stack);
    GtkWidget *scroll = gtk_scrolled_window_new(), *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(page, "font-specimen");
    gtk_box_append(GTK_BOX(page), caption("TYPE SPECIMEN", "font-eyebrow"));
    v->heading = caption("", "font-heading"); gtk_label_set_wrap(GTK_LABEL(v->heading), TRUE);
    gtk_box_append(GTK_BOX(page), v->heading);
    v->subtitle = caption("", "font-subtitle"); gtk_box_append(GTK_BOX(page), v->subtitle);
    v->sample = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_widget_add_css_class(GTK_WIDGET(v->sample), "font-sample");
    gtk_text_view_set_wrap_mode(v->sample, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_pixels_above_lines(v->sample, 2); gtk_text_view_set_pixels_below_lines(v->sample, 7);
    gtk_text_view_set_top_margin(v->sample, 8); gtk_text_view_set_bottom_margin(v->sample, 10);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(v->sample);
    v->sample_tag = gtk_text_buffer_create_tag(buffer, "specimen", "size-points", v->size, NULL);
    gtk_text_buffer_set_text(buffer, default_sample, -1);
    g_signal_connect(buffer, "changed", G_CALLBACK(text_changed), v);
    gtk_box_append(GTK_BOX(page), GTK_WIDGET(v->sample));
    gtk_box_append(GTK_BOX(page), caption("CLICK THE SPECIMEN TO MAKE IT YOUR OWN", "font-hint"));
    GtkWidget *rule = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL); gtk_widget_add_css_class(rule, "font-rule");
    gtk_box_append(GTK_BOX(page), rule);
    v->alphabet = caption("ABCDEFGHIJKLMNOPQRSTUVWXYZ\nabcdefghijklmnopqrstuvwxyz\n0123456789  & @ # % ? !", "font-alphabet");
    gtk_label_set_wrap(GTK_LABEL(v->alphabet), TRUE); gtk_box_append(GTK_BOX(page), v->alphabet);
    v->body = caption("Good typography gives words room to breathe.\nThe quick brown fox jumps over the lazy dog.", "font-body");
    gtk_label_set_wrap(GTK_LABEL(v->body), TRUE); gtk_label_set_selectable(GTK_LABEL(v->body), TRUE);
    gtk_box_append(GTK_BOX(page), v->body);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page);
    gtk_stack_add_named(GTK_STACK(v->stack), scroll, "specimen");

    GtkWidget *glyph_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(glyph_page, "font-glyph-page");
    GtkWidget *glyph_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *glyph_title = caption("Characters", "font-heading"); gtk_widget_set_hexpand(glyph_title, TRUE);
    gtk_box_append(GTK_BOX(glyph_top), glyph_title);
    v->search = gtk_search_entry_new();
    g_object_set(v->search, "placeholder-text", "Character or U+0041", NULL);
    gtk_widget_set_size_request(v->search, 200, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(v->search), GTK_ACCESSIBLE_PROPERTY_LABEL, "Find a Unicode character", -1);
    g_signal_connect(v->search, "search-changed", G_CALLBACK(search_changed), v);
    gtk_box_append(GTK_BOX(glyph_top), v->search); gtk_box_append(GTK_BOX(glyph_page), glyph_top);
    v->glyph_status = caption("", "font-subtitle"); gtk_box_append(GTK_BOX(glyph_page), v->glyph_status);
    v->char_model = gtk_string_list_new(NULL);
    v->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(v->char_model)));
    gtk_single_selection_set_autoselect(v->selection, FALSE);
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(glyph_setup), v); g_signal_connect(factory, "bind", G_CALLBACK(glyph_bind), v);
    v->glyphs = gtk_grid_view_new(GTK_SELECTION_MODEL(v->selection), factory);
    gtk_grid_view_set_min_columns(GTK_GRID_VIEW(v->glyphs), 2); gtk_grid_view_set_max_columns(GTK_GRID_VIEW(v->glyphs), 10);
    gtk_grid_view_set_single_click_activate(GTK_GRID_VIEW(v->glyphs), TRUE);
    g_signal_connect(v->glyphs, "activate", G_CALLBACK(glyph_activated), v);
    GtkWidget *glyph_scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(glyph_scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(glyph_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(glyph_scroll), v->glyphs);
    gtk_box_append(GTK_BOX(glyph_page), glyph_scroll); gtk_stack_add_named(GTK_STACK(v->stack), glyph_page, "glyphs");

    GtkWidget *controls = zen_window_get_controls(v->shell);
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-out-symbolic", "Smaller type (Ctrl+−)", G_CALLBACK(smaller), v));
    v->size_label = gtk_label_new("52 pt"); gtk_box_append(GTK_BOX(controls), v->size_label);
    gtk_box_append(GTK_BOX(controls), zen_button_new("zoom-in-symbolic", "Larger type (Ctrl++)", G_CALLBACK(larger), v));
    v->glyph_toggle = gtk_toggle_button_new_with_label("Glyphs");
    gtk_widget_set_tooltip_text(v->glyph_toggle, "Browse and copy characters (Ctrl+G)");
    g_signal_connect(v->glyph_toggle, "toggled", G_CALLBACK(toggle_glyphs), v);
    gtk_box_append(GTK_BOX(controls), v->glyph_toggle);
    gtk_box_append(GTK_BOX(controls), zen_button_new("edit-undo-symbolic", "Reset specimen and variations (Ctrl+R)", G_CALLBACK(reset_sample), v));
    v->settings = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(v->settings), "document-properties-symbolic");
    gtk_widget_set_tooltip_text(v->settings, "Font details and variations (Ctrl+I)");
    GtkWidget *popover = gtk_popover_new(); gtk_widget_add_css_class(popover, "font-details");
    GtkWidget *settings_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(settings_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(settings_scroll), 460);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(settings_scroll), TRUE);
    GtkWidget *settings_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_size_request(settings_box, 290, -1);
    gtk_widget_set_margin_start(settings_box, 16); gtk_widget_set_margin_end(settings_box, 16);
    gtk_widget_set_margin_top(settings_box, 16); gtk_widget_set_margin_bottom(settings_box, 16);
    gtk_box_append(GTK_BOX(settings_box), caption("Font details", "font-panel-title"));
    v->face_dropdown = gtk_drop_down_new(NULL, NULL);
    g_signal_connect(v->face_dropdown, "notify::selected", G_CALLBACK(face_changed), v);
    gtk_box_append(GTK_BOX(settings_box), v->face_dropdown);
    v->axis_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_box_append(GTK_BOX(settings_box), v->axis_box);
    v->info = caption("", "font-info"); gtk_label_set_wrap(GTK_LABEL(v->info), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v->info), 42); gtk_label_set_selectable(GTK_LABEL(v->info), TRUE);
    gtk_box_append(GTK_BOX(settings_box), v->info);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(settings_scroll), settings_box);
    gtk_popover_set_child(GTK_POPOVER(popover), settings_scroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(v->settings), popover); gtk_box_append(GTK_BOX(controls), v->settings);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), v); gtk_widget_add_controller(GTK_WIDGET(window), keys);
    gtk_widget_set_visible(v->stack, FALSE);
    zen_window_set_ready(v->shell, FALSE); zen_window_present(v->shell);
    return v;
}
static void startup(GApplication *application, gpointer data) {
    (void)application; (void)data; zen_ui_init();
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".font-paper {background:#f6f3eb;color:#303a35;}"
        ".font-specimen {padding:82px 72px 104px;}"
        ".font-eyebrow {font:10px sans-serif;letter-spacing:3px;color:#7b8378;margin-bottom:18px;}"
        ".font-heading {font:400 27px sans-serif;letter-spacing:-0.5px;color:#303a35;}"
        ".font-subtitle {font:12px sans-serif;color:#808579;margin-top:8px;}"
        "textview.font-sample, textview.font-sample text {background:transparent;color:#303a35;}"
        "textview.font-sample {margin-top:48px;}"
        "textview.font-sample text selection {background:#c8d8c5;color:#26352a;}"
        ".font-hint {font:9px sans-serif;letter-spacing:1.5px;color:#919789;margin-top:15px;}"
        ".font-rule {background:#dcded2;min-height:1px;margin:34px 0 28px;}"
        ".font-alphabet {line-height:1.6;}"
        ".font-body {margin-top:32px;line-height:1.65;color:#697367;}"
        ".font-glyph-page {padding:70px 48px 84px;}"
        ".font-glyph-page gridview {background:transparent;color:#303a35;}"
        ".font-glyph-page gridview child {padding:0;border-radius:8px;}"
        ".font-glyph-page gridview child:hover {background:#e9ebdf;}"
        ".font-glyph-page gridview child:selected {background:#dbe3d3;color:#303a35;}"
        ".font-glyph {padding:16px 6px;border-bottom:1px solid #e3e3d9;}"
        ".font-code {font:9px monospace;color:#8a9184;letter-spacing:0.5px;}"
        ".font-details contents {background:#f6f3eb;color:#303a35;border:1px solid #d9ddcf;border-radius:18px;}"
        ".font-details .font-panel-title {font-size:18px;font-weight:600;}"
        ".font-details .font-axis-name {font-size:11px;color:#6c7769;}"
        ".font-details .font-info {font-size:12px;color:#687263;}"
        ".font-details scale value {color:#53634e;font-size:11px;}"
        ".font-details scale highlight {background:#819779;}"
        ".font-details button {color:#303a35;background:#e6e9dd;border-radius:8px;}"
        ".font-details label {text-shadow:none;}", -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}
static void activate(GtkApplication *app, gpointer data) { (void)data; new_viewer(app); }
static void opened(GtkApplication *app, GFile **files, int n, const char *hint, gpointer data) {
    (void)hint; (void)data;
    for (int i = 0; i < n; i++) { Viewer *v = new_viewer(app); open_file(v->shell, files[i], v); }
}
static int inspect_font(const char *path) {
    GError *error = NULL; Font *font = font_load(path, 0, NULL, &error);
    if (!font) { g_printerr("%s\n", error->message); g_error_free(error); return 1; }
    g_print("%s · %s\n%s: %u faces, %ld glyphs, %u Unicode characters, %u variation axes\n",
        font->family, font->style, font->format, font->faces->len, font->glyphs, font->chars->len, font->axes->len);
    font_free(font); return 0;
}
int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    if (argc == 2 && !strcmp(argv[1], "--version")) { g_print("zen-font " ZEN_VERSION "\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        g_print("Usage: zen-font [FONT …]\nOpen TTF, OTF, TTC/OTC or WOFF/WOFF2 fonts without installing them.\n"
            "Ctrl+O open · Ctrl++/− type size · Ctrl+G glyphs · Ctrl+I details · Ctrl+R reset\n"
            "Edit the specimen directly; click any glyph to copy.\n  --inspect FONT  Print font details without opening a window.\n"); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--inspect")) return inspect_font(argv[2]);
    g_set_prgname("zen-font"); g_set_application_name("Zen Font");
    GtkApplication *app = gtk_application_new("io.github.zenviewers.Font", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(startup), NULL); g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(opened), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv); g_object_unref(app); return status;
}
