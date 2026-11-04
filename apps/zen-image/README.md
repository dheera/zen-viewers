# zen-image

Open a photograph or illustration in a borderless window. The image fits the window, preserving its aspect ratio. Move the pointer to reveal its original dimensions and view controls.

| Action | Control |
| --- | --- |
| Fit the whole image | **P** or **0** |
| Fit the width | **W** |
| Show one source pixel per physical display pixel | **1** |
| Zoom | **+** / **−**, or **Ctrl+wheel** around the pointer |
| Pan a magnified image | Drag or scroll. **Shift+wheel** pans sideways. |
| Rotate the view clockwise | **R** |
| Open another image | **Ctrl+O**, or drop the image into the window |

The app reads the system’s GdkPixbuf formats, including PNG and JPEG. WebP requires a system WebP loader; on Ubuntu the package is `webp-pixbuf-loader`. The viewer displays a still frame and honors a JPEG’s EXIF orientation. Rotation changes the view only.

An image opens on a worker thread. The GTK widget keeps the decoded image in a single texture as you pan, zoom and rotate. For large source images, the loader is asked for a preview no larger than 32 megapixels or 16,384 pixels on either axis. The dimension label and actual-size mode use the source dimensions. Compressed input is limited to 128 MiB; the requested output dimensions are not a hard bound on every loader’s internal memory use.

To run the optional native-window checks from the repository root, build the suite, install `xvfb xauth xdotool xclip dbus-daemon python3-pil webp-pixbuf-loader`, then run:

```sh
python3 apps/zen-image/tests/ui-smoke.py build/zen-image
```
