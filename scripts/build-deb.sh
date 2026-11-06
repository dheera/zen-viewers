#!/usr/bin/env bash
# Build native packages against the Debian/Ubuntu release running this script.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
version=$(cat "$root/VERSION")-1
arch=$(dpkg --print-architecture)
apps=(zen-pdf zen-image zen-markdown zen-3d)
meta=true
if (( $# )); then
    if [[ $# != 2 || $1 != --app || ! $2 =~ ^zen-(pdf|image|markdown|3d)$ ]]; then
        echo "Usage: $0 [--app zen-pdf|zen-image|zen-markdown|zen-3d]" >&2; exit 2
    fi
    apps=("$2"); meta=false
fi
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
cd "$tmp"
mkdir source debian packages
# Stale objects in the checkout cannot bypass the distribution's release flags.
cp "$root/Makefile" "$root/VERSION" source/
cp -r "$root/common" source/
mkdir source/apps
for app in "${apps[@]}"; do
    mkdir -p "source/apps/$app"
    cp -r "$root/apps/$app/data" "source/apps/$app/"
    if [[ $app == zen-image ]]; then cp "$root/apps/$app/main.c" "source/apps/$app/"
    else cp -r "$root/apps/$app/src" "source/apps/$app/"; fi
done
cat > debian/changelog <<CHANGELOG
zen-viewers ($version) unstable; urgency=medium

  * Local build from the zen-viewers source distribution.

 -- zen-viewers local builder <builder@localhost>  $(LC_ALL=C date -Ru)
CHANGELOG
cat > debian/control <<'CONTROL'
Source: zen-viewers
Section: graphics
Priority: optional
Maintainer: zen-viewers local builder <builder@localhost>
CONTROL
for app in "${apps[@]}"; do
    printf '\nPackage: %s\nArchitecture: any\nDescription: Native GTK 4 viewer\n' "$app" >> debian/control
done
export DEB_BUILD_MAINT_OPTIONS=hardening=+all
CFLAGS=$(dpkg-buildflags --get CFLAGS)
CPPFLAGS=$(dpkg-buildflags --get CPPFLAGS)
LDFLAGS=$(dpkg-buildflags --get LDFLAGS)
export CFLAGS CPPFLAGS LDFLAGS
make -C source APPS="${apps[*]}" all

finish_package() {
    local app=$1 pkg_arch=$2 depends=$3 description=$4 docs=$5
    local stage="$tmp/debian/$app"
    mkdir -p "$stage/DEBIAN" "$stage/usr/share/doc/$app"
    install -m644 "$root/LICENSE" "$stage/usr/share/doc/$app/copyright"
    ln -s copyright "$stage/usr/share/doc/$app/LICENSE"
    if [[ -e $docs ]]; then install -m644 "$docs" "$stage/usr/share/doc/$app/README.md"; fi
    gzip -n -9 -c debian/changelog > "$stage/usr/share/doc/$app/changelog.Debian.gz"
    cat > "$stage/DEBIAN/control" <<CONTROL
Package: $app
Version: $version
Architecture: $pkg_arch
Maintainer: zen-viewers local builder <builder@localhost>
Section: graphics
Priority: optional
Installed-Size: $(du -sk --apparent-size "$stage/usr" | cut -f1)
Depends: $depends
Description: $description
CONTROL
    # Older GdkPixbuf releases use a separate loader for WebP images.
    case $app in
        zen-image) echo 'Recommends: webp-pixbuf-loader' >> "$stage/DEBIAN/control" ;;
        zen-markdown) echo 'Recommends: fonts-noto-core' >> "$stage/DEBIAN/control" ;;
    esac
    (cd "$stage"; find usr -type f -print0 | LC_ALL=C sort -z | xargs -0 md5sum > DEBIAN/md5sums)
    dpkg-deb --root-owner-group --build "$stage" "$tmp/packages/${app}_${version}_${pkg_arch}.deb"
}
for app in "${apps[@]}"; do
    case $app in
    zen-pdf)      source/build/$app --self-test; summary="Borderless PDF reader with smooth scrolling and selectable text" ;;
    zen-image)    source/build/$app --version; summary="Borderless image viewer with folder navigation and metadata" ;;
    zen-markdown) source/build/$app --self-test; summary="Borderless Markdown reader with formatted tables and selectable text" ;;
    zen-3d)
        source/build/$app --version
        if [[ -e $root/examples/trefoil.glb ]]; then source/build/$app --inspect "$root/examples/trefoil.glb" >/dev/null; fi
        summary="Borderless 3D model viewer with interactive orbit and zoom" ;;
    esac
    mkdir -p "debian/$app/DEBIAN"
    make -C source install APPS="$app" PREFIX=/usr DESTDIR="$tmp/debian/$app"
    strip --strip-unneeded "debian/$app/usr/bin/$app"
    substvars=$(dpkg-shlibdeps -O "debian/$app/usr/bin/$app")
    depends=$(sed -n 's/^shlibs:Depends=//p' <<< "$substvars")
    if [[ -z $depends ]]; then echo "Missing runtime dependencies for $app" >&2; exit 1; fi
    finish_package "$app" "$arch" "$depends" "$summary" "$root/apps/$app/README.md"
done
if $meta; then
    finish_package zen-viewers all "zen-pdf (= $version), zen-image (= $version), zen-markdown (= $version), zen-3d (= $version)" \
        "Install the Zen PDF, image, Markdown and 3D viewers" "$root/README.md"
fi
mkdir -p "$root/dist"
install -m644 "$tmp"/packages/*.deb "$root/dist/"
printf 'Wrote packages to %s/dist\n' "$root"
