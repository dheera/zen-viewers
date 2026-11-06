# zen-image

Open a photograph or illustration in a borderless window. The image fits the window, preserving its aspect ratio. Move the pointer near the top or bottom edge to reveal the corresponding controls. Image details remain hidden until requested.

| Action | Control |
| --- | --- |
| Previous / next image in the same folder | **Left** / **Right**, **<** / **>**, or arrow buttons |
| Show / hide image details | **I**, or the information button |
| Fit the whole image | **P** or **0** |
| Fit the width | **W** |
| Show one source pixel per physical display pixel | **1** |
| Zoom | **+** / **−**, or **Ctrl+wheel** around the pointer |
| Pan a magnified image | Drag or scroll. **Shift+wheel** pans sideways. |
| Rotate the view clockwise | **R** |
| Open another image | **Ctrl+O**, or drop the image into the window |

Folder navigation uses natural filename order, placing `image2` before `image10`. It includes recognized image files in the current folder, skips files that fail to decode, and stops at either end. Reopen an image to pick up changes to the folder. The listing and image decoding run on worker threads, and newer navigation requests replace obsolete loads.

The app uses the system’s GdkPixbuf loaders. Common formats include PNG, JPEG, GIF, BMP, TIFF and ICO. Run `zen-image --formats` to see the actual formats available on your machine. These Ubuntu packages add more image formats.

```sh
sudo apt install webp-pixbuf-loader heif-gdk-pixbuf librsvg2-common
```

These provide WebP, HEIF/HEIC and AVIF, and SVG respectively. On Ubuntu 24.04 and newer, JPEG XL is available separately with `sudo apt install libjxl-gdk-pixbuf`. Codec availability depends on the Ubuntu release. Other installed GdkPixbuf loaders are picked up automatically. Animated and multi-page images show a still frame. Zen honors embedded orientation when the loader supplies it. Rotation applies to the view only.

Image details include the original dimensions, format, file size, modification time, transparency and location. The panel’s text can be selected and copied. If built with `libexif-dev`, it also shows available JPEG EXIF camera, capture date, exposure, aperture, ISO, focal length and credit fields.

The GTK widget keeps the decoded image in a single texture as you pan, zoom and rotate. For large source images, the loader is asked for a preview no larger than 32 megapixels or 16,384 pixels on either axis. Actual-size mode uses the source dimensions. The details panel reports both source and reduced preview dimensions. Compressed input is limited to 128 MiB. A loader may use additional memory to decode an image.

To run the optional native-window checks from the repository root, build the suite, install `xvfb xauth xdotool xclip dbus-daemon python3-pil webp-pixbuf-loader`, then run the test.

```sh
python3 apps/zen-image/tests/ui-smoke.py build/zen-image
```

Set `EXPECT_EXIF=1` when testing a build with libexif to verify the camera fields as well.
With FFmpeg and the corresponding loaders installed, `ZEN_TEST_EXTRA_FORMATS=1` also generates and opens AVIF and JPEG XL fixtures.
