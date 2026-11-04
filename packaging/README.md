# Distribution packages

Each viewer has its own executable, desktop entry, icon and runtime dependency list. The **zen-viewers** metapackage installs all three apps at the same release version. The shared window code is linked into each program. Build packages on the target Linux distribution and CPU architecture.

## Ubuntu and Debian

From the repository root, build and install all four packages:

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libpoppler-glib-dev libcmark-dev dpkg-dev
./scripts/build-deb.sh
sudo apt install ./dist/*_0.2.0-1_*.deb
```

To build just one app, use `./scripts/build-deb.sh --app zen-pdf`, `--app zen-image`, or `--app zen-markdown`. Install the corresponding local file, for example `sudo apt install ./dist/zen-image_0.2.0-1_amd64.deb`. GTK 4 development headers are needed to compile all apps. Only PDF needs the Poppler development package, and only Markdown needs the cmark development package. The builder extracts runtime dependencies from each executable with `dpkg-shlibdeps`.

## Fedora and other RPM systems

On Fedora, run:

```sh
sudo dnf install gcc make pkgconf-pkg-config gtk4-devel poppler-glib-devel cmark-devel rpm-build
./scripts/build-rpm.sh
```

The output includes binary packages for all three apps, a metapackage and the source RPM. Install one app by its generated filename, for example `sudo dnf install ./dist/zen-image-0.2.0-1*.x86_64.rpm`. To install the metapackage from local files, supply the matching zen-viewers, zen-pdf, zen-image and zen-markdown binary RPMs together.
