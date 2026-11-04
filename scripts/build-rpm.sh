#!/usr/bin/env bash
# Run on the RPM-based distribution and architecture you want to target.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
version=$(cat "$root/VERSION")
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
top="$tmp/rpmbuild"
name="zen-viewers-$version"
mkdir -p "$top"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} "$tmp/$name"
files=(Makefile VERSION LICENSE common apps scripts packaging tests)
for item in README.md examples previews; do
    if [[ -e $root/$item ]]; then files+=("$item"); fi
done
tar --exclude='dist' --exclude='build' --exclude='__pycache__' --exclude='*.o' \
    --exclude='apps/zen-pdf/zen-pdf' -C "$root" -cf - "${files[@]}" | tar -C "$tmp/$name" -xf -
tar -czf "$top/SOURCES/$name.tar.gz" -C "$tmp" "$name"
cp "$root/packaging/zen-viewers.spec" "$top/SPECS/"
rpmbuild --define "_topdir $top" --define "zen_viewers_version $version" -ba "$top/SPECS/zen-viewers.spec"
mkdir -p "$root/dist"
find "$top/RPMS" "$top/SRPMS" -type f -name '*.rpm' -exec install -m644 -t "$root/dist" {} +
printf 'Wrote packages to %s/dist\n' "$root"
