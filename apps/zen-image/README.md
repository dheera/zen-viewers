# zen-image

Open a photograph or illustration in a borderless window. The image fits the window, preserving its aspect ratio. Move the pointer near the top or bottom edge to reveal the corresponding controls. Image details remain hidden until requested.

| Action | Control |
| --- | --- |
| Previous / next image in the same folder | **Left** / **Right**, **<** / **>**, or arrow buttons |
| Show / hide image details | **I**, or the information button |
| Fit the whole image | **P** or **0** |
| Fit the width | **W** |
| Show the image at actual size | **1** |
| Zoom | **+** / **−**, or **Ctrl+wheel** around the pointer |
| Pan a magnified image | Drag or scroll. **Shift+wheel** pans sideways. |
| Rotate the view clockwise | **R** |
| Open another image | **Ctrl+O**, or drop the image into the window |

Folder navigation uses natural filename order, placing `image2` before `image10`. It includes recognized image files in the current folder, skips files that fail to decode, and stops at either end. Zen preloads the next and previous images on worker threads and reuses their textures when you press an arrow. An unusually large neighbor keeps a smaller preview within the cache budget. When the filesystem reports an edit, Zen invalidates the affected cached image. Reopen an image to refresh the folder listing and cache.

On a cache miss, Zen shows a valid desktop thumbnail while decoding if one is available. Otherwise, it holds the current image until the next one is ready. A small loading indicator appears if the switch takes more than a moment. Rapid navigation gives priority to the latest selection.

The app uses the system’s GdkPixbuf loaders. Common formats include PNG, JPEG, GIF, BMP, TIFF and ICO. Run `zen-image --formats` to see the actual formats available on your machine. These Ubuntu packages add more image formats.

```sh
sudo apt install webp-pixbuf-loader heif-gdk-pixbuf librsvg2-common
```

These provide WebP, HEIF/HEIC and AVIF, and SVG respectively. On Ubuntu 24.04 and newer, JPEG XL is available separately with `sudo apt install libjxl-gdk-pixbuf`. Codec availability depends on the Ubuntu release. Other installed GdkPixbuf loaders are picked up automatically. Animated and multi-page images show a still frame. Zen honors embedded orientation when the loader supplies it. Rotation applies to the view only.

With LibRaw enabled, Zen opens Canon CR2/CR3, Sony ARW, DNG, Nikon NEF, Fujifilm RAF, and other camera RAW files. It displays the camera's embedded preview when available. When the preview is missing or very small, Zen tries a quick reduced-resolution render with camera white balance. The details panel identifies which preview is shown and lists its dimensions separately from the RAW image dimensions. Camera and compression support depend on the installed LibRaw version. The RAW input limit is 256 MiB.

To build with RAW support, install `libraw-dev` on Ubuntu or `LibRaw-devel` on Fedora before building. `zen-image --formats` lists camera RAW extensions when support is enabled. For a build without LibRaw, pass `IMAGE_RAW=no` to `make`.

Image details include the original dimensions, format, file size, modification time, transparency and location. The panel’s text can be selected and copied. If built with `libexif-dev`, it also shows available JPEG EXIF camera, capture date, exposure, aperture, ISO, focal length and credit fields.

The GTK widget uses a smaller texture to display large images at fit-to-window scale and switches to the full decoded texture when more detail is needed. The two neighbors retain up to 80 MiB of pixel data each. For large images loaded by GdkPixbuf, the loader is asked for a preview no larger than 32 megapixels or 16,384 pixels on either axis. Actual-size mode uses source dimensions for those formats and preview dimensions for camera RAW. The details panel reports both source and reduced preview dimensions. Compressed input for GdkPixbuf is limited to 128 MiB. A loader may use additional memory to decode an image.

To run the optional native-window checks from the repository root, build the suite, install `xvfb xauth xdotool xclip dbus-daemon python3-pil webp-pixbuf-loader`, then run the test. The thumbnail ordering check also uses the build toolchain to pause a source read during the test.

```sh
python3 apps/zen-image/tests/ui-smoke.py build/zen-image
```

Set `EXPECT_EXIF=1` when testing a build with libexif to verify the camera fields as well.
With FFmpeg and the corresponding loaders installed, `ZEN_TEST_EXTRA_FORMATS=1` also generates and opens AVIF and JPEG XL fixtures.
