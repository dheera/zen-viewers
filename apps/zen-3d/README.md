# Zen 3D

Zen 3D is a borderless desktop viewer for inspecting a model. It opens OBJ, GLB, glTF, STL, PLY and other formats, including Collada, FBX, 3MF and OFF. Most formats use Assimp. Common binary PLY files have a dedicated streaming loader. Run `zen-3d --formats` to see the extensions supported by your installed importer.

Open a model from the app menu, drag it onto the window, or run `zen-3d /path/to/model.glb`. Keep related texture and material files beside the model in their original folder structure. Hover near the top to open a file, enter fullscreen or close the window. Hover near the bottom for the view controls.

| Action | Control |
| --- | --- |
| Orbit | Primary drag or **arrow keys** |
| Pan | **Shift+drag**, middle or right drag, or **Shift+arrow keys** |
| Zoom | Wheel, touchpad pinch, or **+ / −** |
| Center and fit the model | **F** or **Home** |
| Reset the camera | **R** |
| Front, right and top views | **1**, **2**, **3**, or the **View** menu |
| Switch perspective and orthographic views | **O** |
| Show or hide wireframe overlay | **W** |
| Show or hide ground grid | **G** |
| Show or hide model details | **I**. **Esc** dismisses the panel. |
| Open, fullscreen, close | **Ctrl+O**, **F11**, **Ctrl+W** |

The viewer previews static geometry, base colors, base-color textures, vertex colors, lines and point clouds. The details panel reports geometry counts and bounding-box dimensions in source units. Models are centered and scaled for navigation while their original axes are preserved.

File import runs on a worker. The OpenGL renderer uploads indexed geometry in chunks, retains it on the GPU, and redraws on demand. Once uploaded, it releases the CPU vertex, index and decoded texture buffers. Zen 3D requires desktop OpenGL 3.3. Mesa's software OpenGL can run it when no hardware driver is available.

For a binary little-endian PLY containing float XYZ vertices and indexed triangles, the loader retains shared vertices and reads the file in blocks. It computes smooth normals without expanding every triangle into separate vertices. PLY files with colors, supplied normals, other vertex properties, polygons or another byte order use Assimp so those features are preserved.

Large supported PLY files show a progress bar. When opening into an empty window, an initial point preview appears after the vertices are read, while triangle processing continues. A coarser mesh can appear during upload and camera movement. The full model returns when movement settles and upload is complete. The details panel reports the full geometry counts after loading. Preview availability depends on the mesh and available memory.

Each input file is limited to 256 MiB. Decoded geometry is limited to 512 MiB for the dedicated PLY loader and 256 MiB for the general Assimp path. Texture previews are bounded to 4 megapixels per image and 32 megapixels in total. Decoders, previews and graphics drivers can use additional memory. Missing textures fall back to the material color.

## Build

From the repository root on Ubuntu, install the development libraries and build Zen 3D.

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libepoxy-dev libassimp-dev
make zen-3d
build/zen-3d examples/trefoil.glb
```

Read model statistics without opening a window.

```sh
build/zen-3d --inspect examples/trefoil.glb
```

The standalone Debian package builder is `./scripts/build-deb.sh --app zen-3d`. Build on the Ubuntu release where you intend to install the package. [Packaging notes](../../packaging/README.md)

## Verification

```sh
python3 apps/zen-3d/tests/import-smoke.py
python3 apps/zen-3d/tests/ply-smoke.py
python3 apps/zen-3d/tests/ui-smoke.py build/zen-3d
```

The import checks generate models and textures locally. The PLY checks include a temporary 20 MiB fixture for early preview ownership, progress and cancellation, plus malformed input, shared indexing, normals, source/geometry limits and fallback coverage. The window checks use Xvfb with OpenGL, xdotool, xclip, dbus-daemon and Python Pillow.

Run the external-model check against the installed executable.

```sh
python3 apps/zen-3d/tests/large-model-smoke.py /path/to/model.ply --binary /usr/bin/zen-3d
```

Use `--expect-bytes`, `--expect-vertices`, `--expect-triangles`, `--expect-sha256` or `--expect-version` to check a known fixture. Use `--deb /path/to/zen-3d.deb` to extract and inspect a package in a temporary directory on a machine with the matching runtime libraries. The JSON report measures headless import and records file and executable hashes, geometry counts, elapsed time, CPU time and peak resident memory. Hashing the file before import warms the OS cache.
