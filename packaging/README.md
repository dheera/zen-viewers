# Distribution packages

Each viewer has its own executable, desktop entry, icon and runtime dependency list. The **zen-viewers** metapackage installs all three apps at the same release version. The shared window code is linked into each program. Build packages on the target Linux distribution and CPU architecture.

## Ubuntu and Debian

Run these commands from the repository root to build and install all four packages.

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libpoppler-glib-dev libcmark-gfm-dev libcmark-gfm-extensions-dev libexif-dev dpkg-dev
./scripts/build-deb.sh
sudo apt install ./dist/*_"$(cat VERSION)"-1_*.deb
```

To build just one app, use `./scripts/build-deb.sh --app zen-pdf`, `--app zen-image`, or `--app zen-markdown`. Install the corresponding local file, for example `sudo apt install ./dist/zen-image_"$(cat VERSION)"-1_amd64.deb`. GTK 4 development headers are needed to compile all apps. PDF uses Poppler. Markdown uses cmark-gfm and its table extension. The optional libexif headers add JPEG EXIF metadata to the image viewer. The builder extracts runtime dependencies from each executable with `dpkg-shlibdeps`.

## Fedora and other RPM systems

Install the build tools on Fedora and run the RPM builder.

```sh
sudo dnf install gcc gcc-c++ make cmake curl pkgconf-pkg-config gtk4-devel poppler-glib-devel libexif-devel rpm-build
./scripts/build-rpm.sh
```

The output includes binary packages for all three apps, a metapackage and the source RPM. Install one app by its generated filename, for example `sudo dnf install ./dist/zen-image-"$(cat VERSION)"-1*.x86_64.rpm`. To install the metapackage from local files, supply the matching zen-viewers, zen-pdf, zen-image and zen-markdown binary RPMs together.

The RPM builder downloads upstream cmark-gfm **0.29.0.gfm.13**, verifies SHA-256 `5abc61798ebd9de5660bc076443c07abad2b8d15dbc11094a3a79644b8ad243a`, and places that source archive in the source RPM. The spec repeats the checksum check, builds private static libraries, and links them into `zen-markdown`. The upstream license accompanies the binary, and the RPM declares `bundled(cmark-gfm)` so the pinned dependency is visible. Updating the bundled parser requires a rebuilt Markdown RPM. Ubuntu and Debian builds use the distribution's shared libraries.

For an offline build, set `CMARK_GFM_SOURCE=/path/to/cmark-gfm-0.29.0.gfm.13.tar.gz` before running the script. It checks the same digest. Set `JOBS` to change the default of two parallel build jobs. The source comes from [GitHub's cmark-gfm release](https://github.com/github/cmark-gfm/tree/0.29.0.gfm.13).
