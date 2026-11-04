/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZEN_UI_H
#define ZEN_UI_H
#include <gtk/gtk.h>

typedef struct ZenWindow ZenWindow;
typedef void (*ZenOpenFunc)(ZenWindow *window, GFile *file, gpointer user_data);
/* TRUE when content owns a press at window-local x/y (e.g. a text glyph).
 * This prevents the shared top-area move gesture from stealing that press. */
typedef gboolean (*ZenPointerClaimFunc)(double x, double y, gpointer user_data);

void zen_ui_init(void);
ZenWindow *zen_window_new(GtkApplication *application, const char *app_name, int width, int height);
GtkWindow *zen_window_get_window(ZenWindow *window);
GtkWidget *zen_window_get_root(ZenWindow *window);
GtkWidget *zen_window_get_controls(ZenWindow *window); /* bottom GtkBox */
void zen_window_set_child(ZenWindow *window, GtkWidget *child);
void zen_window_set_open_handler(ZenWindow *window, ZenOpenFunc callback, gpointer user_data);
void zen_window_set_pointer_claim_func(ZenWindow *window, ZenPointerClaimFunc callback, gpointer user_data);
void zen_window_add_file_filter(ZenWindow *window, const char *name,
                               const char *const *mime_types, const char *const *patterns);
void zen_window_set_empty_text(ZenWindow *window, const char *title, const char *subtitle);
void zen_window_set_title(ZenWindow *window, const char *filename); /* NULL resets app title */
void zen_window_set_ready(ZenWindow *window, gboolean ready);
void zen_window_present(ZenWindow *window);
void zen_window_choose_file(ZenWindow *window);
void zen_window_notice(ZenWindow *window, const char *message);
void zen_window_clear_notice(ZenWindow *window);
void zen_window_reveal_controls(ZenWindow *window);
void zen_window_toggle_fullscreen(ZenWindow *window);
gboolean zen_window_is_closed(ZenWindow *window);
int zen_window_edge_at(ZenWindow *window, double x, double y); /* GdkSurfaceEdge, -1 inside */
GtkWidget *zen_button_new(const char *icon_name, const char *tooltip,
                          GCallback clicked, gpointer user_data);
#endif
