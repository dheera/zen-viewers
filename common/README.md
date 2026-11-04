# Shared viewer shell

`zen-ui.h` is the boundary between the small GTK window shell and an individual viewer. It accepts an ordinary `GtkWidget`; it does not prescribe a page model, renderer, or document format.

The shell owns rounded borderless windows, edge resize and top-area move gestures, hover overlays, the open/fullscreen/close controls, a bottom controls slot, native file choosing, file drops, global shortcuts, a start screen and transient notices. Filename labels are converted to display-safe UTF-8. Callbacks keep file handling and content gestures in the app.

The app owns parsing/decoding, worker cancellation, document/cache state, content rendering, selection, content-specific controls and keyboard commands. Selection and editing behavior belongs to the content widget. A PDF page number is not a shared concept that video or 3D viewers must implement.

A viewer starts by creating its shell, installing its content widget, adding controls and registering the file-open callback:

```c
ZenWindow *shell = zen_window_new(application, "Zen Example", 800, 700);
zen_window_set_child(shell, content_widget);
zen_window_set_open_handler(shell, open_file, app_state);
gtk_box_append(GTK_BOX(zen_window_get_controls(shell)), app_control);
zen_window_present(shell);
```

After loading, call `zen_window_set_title` and `zen_window_set_ready`. The supplied examples keep a reference to the native window during asynchronous work, reject stale results by generation, and cancel work on close. GTK widgets are only accessed on the main thread. The shell's closed state lets completion callbacks discard results without touching destroyed widgets.

Use `zen_window_set_pointer_claim_func` when content needs a primary-button press in the top area, such as selecting a PDF glyph or Markdown text. Window edges remain available for resizing. Ctrl+primary presses are left to the content for forced panning.

Future `zen-3d` can provide a `GtkGLArea` with its own model loader/camera controls; `zen-video` can provide a media-framework paintable and timeline. Neither should add its renderer or codec dependencies to this library. Keep the app packages independently installable.

The repository root builds `build/libzen-ui.a` once and links it into all selected apps. This avoids introducing a separately versioned runtime UI library while the internal API is evolving. It also keeps the existing lightweight dependency model: GTK plus each app's own content library.
