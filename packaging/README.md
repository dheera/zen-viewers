# Distribution packages

Each viewer has its own executable, desktop entry, icon and runtime dependency list. The **zen-viewers** metapackage installs all four apps at the same release version. The shared window code is linked into each program. Build packages on the target Linux distribution and CPU architecture.

## Ubuntu and Debian

Run these commands from the repository root to build and install all five packages.

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libpoppler-glib-dev libcmark-gfm-dev libcmark-gfm-extensions-dev libexif-dev libepoxy-dev libassimp-dev dpkg-dev
./scripts/build-deb.sh
sudo apt install ./dist/*_"$(cat VERSION)"-1_*.deb
```

To build just one app, use `./scripts/build-deb.sh --app zen-pdf`, `--app zen-image`, `--app zen-markdown`, or `--app zen-3d`. Install the corresponding local file, for example `sudo apt install ./dist/zen-3d_"$(cat VERSION)"-1_amd64.deb`. GTK 4 development headers are needed to compile all apps. PDF uses Poppler. Markdown uses cmark-gfm and its table extension. The optional libexif headers add JPEG EXIF metadata to the image viewer. Zen 3D uses libepoxy for OpenGL entry points and Assimp for model import. The builder extracts runtime dependencies from each executable with `dpkg-shlibdeps`; Assimp is a dependency of `zen-3d` only.

For a standalone Zen 3D build, the development dependencies are `build-essential pkg-config libgtk-4-dev libepoxy-dev libassimp-dev`; add `dpkg-dev` to build its Debian package. `make APPS=zen-3d` builds only that executable. The existing PDF, image and Markdown packages do not gain the 3D import libraries. Installing the suite metapackage also installs Zen 3D.

Assimp's shared-library ABI can differ between distribution releases. Build the Zen 3D package on the release where it will be installed: a package linked to `libassimp5` needs that library, even if the target system has `libassimp6`. The source supports Ubuntu 22.04's GTK 4.6 and Assimp 5.2; newer distributions can build against their own development packages.

## Fedora and other RPM systems

Install the build tools on Fedora and run the RPM builder.

```sh
sudo dnf install gcc gcc-c++ make cmake curl pkgconf-pkg-config gtk4-devel poppler-glib-devel libexif-devel libepoxy-devel assimp-devel rpm-build
./scripts/build-rpm.sh
```

The output includes binary packages for all four apps, a metapackage and the source RPM. Install one app by its generated filename, for example `sudo dnf install ./dist/zen-3d-"$(cat VERSION)"-1*.x86_64.rpm`. Each binary package has its own automatic shared-library dependencies. To install the metapackage from local files, supply the matching zen-viewers, zen-pdf, zen-image, zen-markdown and zen-3d binary RPMs together.

The RPM builder downloads upstream cmark-gfm **0.29.0.gfm.13**, verifies SHA-256 `5abc61798ebd9de5660bc076443c07abad2b8d15dbc11094a3a79644b8ad243a`, and places that source archive in the source RPM. The spec repeats the checksum check, builds private static libraries, and links them into `zen-markdown`. The upstream license accompanies the binary, and the RPM declares `bundled(cmark-gfm)` so the pinned dependency is visible. Updating the bundled parser requires a rebuilt Markdown RPM. Ubuntu and Debian builds use the distribution's shared libraries.

For an offline build, set `CMARK_GFM_SOURCE=/path/to/cmark-gfm-0.29.0.gfm.13.tar.gz` before running the script. It checks the same digest. Set `JOBS` to change the default of two parallel build jobs. The source comes from [GitHub's cmark-gfm release](https://github.com/github/cmark-gfm/tree/0.29.0.gfm.13).
