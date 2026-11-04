#!/bin/sh
# Verify real Poppler rendering, aspect ratios and CLI errors without a display server.
set -eu
cd "$(dirname "$0")/.."
app=${ZEN_PDF_BIN:-./zen-pdf}
tempdir=$(mktemp -d)
trap 'rm -rf -- "$tempdir"' EXIT
unset DISPLAY WAYLAND_DISPLAY

"$app" --self-test
python3 tests/make-fixture.py "$tempdir/mixed pages.pdf"
for page in 1 2 3; do
    "$app" --render "$tempdir/mixed pages.pdf" "$tempdir/$page.png" 360 "$page"
done
python3 tests/make-fixture.py --tall-strip "$tempdir/strip.pdf"
"$app" --render "$tempdir/strip.pdf" "$tempdir/strip.png" 360 3
python3 - "$tempdir" <<'PY'
from pathlib import Path
import struct
import sys

directory = Path(sys.argv[1])
for page, height in enumerate((540, 240, 1200), start=1):
    png = (directory / f"{page}.png").read_bytes()
    if png[:16] != b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR":
        sys.exit(f"Page {page} is missing the PNG header")
    dimensions = struct.unpack(">II", png[16:24])
    if dimensions != (360, height):
        sys.exit(f"Page {page} has dimensions {dimensions}, expected {(360, height)}")
with (directory / "strip.png").open("rb") as stream:
    stream.seek(16)
    width, height = struct.unpack(">II", stream.read(8))
if not (0 < width < 360 and height == width * 240 and height <= 32767):
    sys.exit(f"Extreme aspect ratio was not preserved within the render limit: {width} x {height}")
PY

must_reject() {
    if "$app" --render "$@" >"$tempdir/error.log" 2>&1; then
        echo "Unexpected success: --render $*" >&2
        exit 1
    else
        status=$?
    fi
    if [ "$status" -gt 2 ] || [ ! -s "$tempdir/error.log" ]; then
        echo "Expected a clean error, got exit $status: --render $*" >&2
        exit 1
    fi
}

input="$tempdir/mixed pages.pdf"
output="$tempdir/bad.png"
for bad_page in 0 4 1x; do
    must_reject "$input" "$output" 360 "$bad_page"
done
for bad_width in 0 16385 1.5; do
    must_reject "$input" "$output" "$bad_width" 1
done
must_reject "$tempdir/missing.pdf" "$output" 360 1
echo 'This is not a PDF.' >"$tempdir/invalid.pdf"
must_reject "$tempdir/invalid.pdf" "$output" 360 1
must_reject "$input" "$tempdir/no-such-dir/bad.png" 360 1
echo "Passed: mixed page sizes, PNG output and invalid inputs."
