# Distribution packages

Each viewer has its own executable, desktop entry, icon and runtime dependency list. The **zen-viewers** metapackage installs all nine apps at the same release version. The shared window code is linked into each program. Build packages on the target Linux distribution and CPU architecture.

## Ubuntu and Debian

Run these commands from the repository root to build and install the suite.

```sh
sudo apt install build-essential cmake pkg-config libgtk-4-dev libpoppler-glib-dev libcmark-gfm-dev libcmark-gfm-extensions-dev libexif-dev libraw-dev libepoxy-dev libassimp-dev libmpv-dev libarchive-dev libxml2-dev libfontconfig-dev libfreetype-dev libcurl4-gnutls-dev libjson-glib-dev libzstd-dev zlib1g-dev liblz4-dev libbrotli-dev dpkg-dev
./scripts/build-deb.sh
sudo apt install ./dist/*_"$(cat VERSION)"-1_*.deb
```

To build one app, pass its name to `./scripts/build-deb.sh --app zen-map`. Install the corresponding local package with `sudo apt install ./dist/zen-map_"$(cat VERSION)"-1_amd64.deb` on amd64. The builder extracts linked runtime dependencies from each executable with `dpkg-shlibdeps`. See the individual build targets in the [root README](../README.md#build) for development libraries.

For a standalone Zen 3D build, the development dependencies are `build-essential pkg-config libgtk-4-dev libepoxy-dev libassimp-dev`; add `dpkg-dev` to build its Debian package. `make APPS=zen-3d` builds only that executable. The existing PDF, image and Markdown packages do not gain the 3D import libraries. Installing the suite metapackage also installs Zen 3D.

Zen Image enables camera RAW previews when the LibRaw development files are available. `IMAGE_RAW=no ./scripts/build-deb.sh --app zen-image` builds a package with only the system image loaders. RAW-enabled packages pick up the distribution's LibRaw runtime dependency automatically.

Zen Table links the pinned Carquet C library into its executable. The source archive is included and verified by SHA-256 before extraction. System zstd, zlib, LZ4 and Brotli libraries provide compression. The installed package includes the bundled code's license notices.

Shared-library ABIs can differ between distribution releases. Build each package on the release where it will be installed: a package linked to `libassimp5` needs that library, even if the target system has `libassimp6`. Similarly, Ubuntu 22.04 uses libmpv1 while newer releases may use libmpv2. The source supports Ubuntu 22.04's GTK 4.6, Assimp 5.2 and libmpv 0.34; newer distributions can build against their own development packages.

For standalone video or book builds:

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libepoxy-dev libmpv-dev dpkg-dev
./scripts/build-deb.sh --app zen-video
# Or:
sudo apt install build-essential pkg-config libgtk-4-dev libarchive-dev libxml2-dev dpkg-dev
./scripts/build-deb.sh --app zen-books
```

`make APPS=zen-video` and `make APPS=zen-books` also build the individual executables. Video format and codec support comes from the distribution's libmpv/FFmpeg build; it may differ between distributions. The video player links dynamically to the distribution library; it does not bundle codecs.

`VIDEO_CFLAGS` and `VIDEO_LIBS` can be set in the environment or passed to `make` when using an independently staged libmpv SDK. Include both libmpv and libepoxy flags. This only changes the build; `dpkg-shlibdeps` still inspects the resulting executable to determine package runtime requirements.

## Fedora and other RPM systems

Install the build tools on Fedora and run the RPM builder.

```sh
sudo dnf install gcc gcc-c++ make cmake curl pkgconf-pkg-config gtk4-devel poppler-glib-devel libexif-devel LibRaw-devel libepoxy-devel assimp-devel mpv-devel libarchive-devel libxml2-devel fontconfig-devel freetype-devel libcurl-devel json-glib-devel libzstd-devel zlib-devel lz4-devel brotli-devel rpm-build
./scripts/build-rpm.sh
```

The output includes binary packages for all nine apps, a metapackage and the source RPM. Install one app by its generated filename, for example `sudo dnf install ./dist/zen-font-"$(cat VERSION)"-1*.x86_64.rpm`. Each binary package has its own automatic shared-library dependencies. To install the metapackage from local files, supply it and the nine matching viewer RPMs together.

When rebuilding the source RPM directly, `rpmbuild --without raw` disables camera RAW previews in Zen Image.

The RPM builder downloads upstream cmark-gfm **0.29.0.gfm.13**, verifies SHA-256 `5abc61798ebd9de5660bc076443c07abad2b8d15dbc11094a3a79644b8ad243a`, and places that source archive in the source RPM. The spec repeats the checksum check, builds private static libraries, and links them into `zen-markdown`. The upstream license accompanies the binary, and the RPM declares `bundled(cmark-gfm)` so the pinned dependency is visible. Updating the bundled parser requires a rebuilt Markdown RPM. Ubuntu and Debian builds use the distribution's shared libraries.

For an offline build, set `CMARK_GFM_SOURCE=/path/to/cmark-gfm-0.29.0.gfm.13.tar.gz` before running the script. It checks the same digest. Set `JOBS` to change the default of two parallel build jobs. The source comes from [GitHub's cmark-gfm release](https://github.com/github/cmark-gfm/tree/0.29.0.gfm.13).
