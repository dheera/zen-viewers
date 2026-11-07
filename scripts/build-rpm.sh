#!/usr/bin/env bash
# Run on the RPM-based distribution and architecture you want to target.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
version=$(cat "$root/VERSION")
[[ $version =~ ^[0-9][A-Za-z0-9._+~]*$ ]] || { printf 'VERSION is not a valid RPM version.\n' >&2; exit 1; }
command -v rpmbuild >/dev/null || { printf 'Install rpm-build before running this script.\n' >&2; exit 1; }
cmark_version=0.29.0.gfm.13
cmark_sha256=5abc61798ebd9de5660bc076443c07abad2b8d15dbc11094a3a79644b8ad243a
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
top="$tmp/rpmbuild"
name="zen-viewers-$version"
mkdir -p "$top"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} "$tmp/$name"
files=(Makefile VERSION LICENSE common apps scripts packaging tests vendor)
for item in README.md examples previews; do
    if [[ -e $root/$item ]]; then files+=("$item"); fi
done
tar --exclude='dist' --exclude='build' --exclude='__pycache__' --exclude='*.o' \
    --exclude='apps/zen-pdf/zen-pdf' -C "$root" -cf - "${files[@]}" | tar -C "$tmp/$name" -xf -
tar -czf "$top/SOURCES/$name.tar.gz" -C "$tmp" "$name"
# Fedora does not package the standalone C cmark-gfm library. Keep the exact
# upstream source in the SRPM and link its private static libraries into Markdown.
cmark_source="$top/SOURCES/cmark-gfm-$cmark_version.tar.gz"
if [[ -n ${CMARK_GFM_SOURCE:-} ]]; then
    cp -- "$CMARK_GFM_SOURCE" "$cmark_source"
else
    command -v curl >/dev/null || { printf 'Install curl, or set CMARK_GFM_SOURCE to the pinned source archive.\n' >&2; exit 1; }
    curl --fail --location --show-error --retry 2 \
        "https://codeload.github.com/github/cmark-gfm/tar.gz/refs/tags/$cmark_version" -o "$cmark_source"
fi
printf '%s  %s\n' "$cmark_sha256" "$cmark_source" | sha256sum --check --status || {
    printf 'The cmark-gfm source checksum does not match the pinned release.\n' >&2; exit 1;
}
# Preserve the exact version when the resulting SRPM is rebuilt independently.
printf '%%global zen_viewers_version %s\n' "$version" > "$top/SPECS/zen-viewers.spec"
cat "$root/packaging/zen-viewers.spec" >> "$top/SPECS/zen-viewers.spec"
rpmbuild --define "_topdir $top" \
    --define "_smp_mflags -j${JOBS:-2}" -ba "$top/SPECS/zen-viewers.spec"
mkdir -p "$root/dist"
find "$top/RPMS" "$top/SRPMS" -type f -name '*.rpm' -exec install -m644 -t "$root/dist" {} +
printf 'Wrote packages to %s/dist\n' "$root"
