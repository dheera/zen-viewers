#!/usr/bin/env python3
"""Write a three-page test document using only the Python standard library."""
import argparse
from pathlib import Path


def page_stream(width, height, index, title, subtitle):
    # Distinct color fields and a footer near the bottom expose page, clip and scroll errors.
    palettes = [("0.89 0.92 0.84", "0.16 0.30 0.24"),
                ("0.95 0.88 0.76", "0.39 0.23 0.15"),
                ("0.85 0.90 0.92", "0.15 0.28 0.35")]
    ground, ink = palettes[index - 1]
    left, right = 32, width - 32

    def label(x, y, size, text):
        return f"BT /F1 {size} Tf {x} {y} Td ({text}) Tj ET"

    commands = [
        f"{ground} rg 0 0 {width} {height} re f",
        f"{ink} rg {ink} RG",
        label(left, height - 44, 11, "FIELD NOTES   /   VOLUME 01"),
        label(left, height - 128, 37, title),
        label(left, height - 159, 12, subtitle),
        f"0.7 w {left} {height - 68} m {right} {height - 68} l S",
    ]
    # A series of contour lines reaches down tall pages, making vertical motion visible.
    top = height - 222
    for y in range(top, 94, -14):
        commands.append(
            f"{left} {y} m "
            f"{width * .34:.1f} {y + 52} {width * .58:.1f} {y - 45} {right} {y + 4} c S"
        )
    commands += [
        f"{left} 59 m {right} 59 l S",
        label(left, 32, 10, "ZEN PDF   /   READING SAMPLE"),
        label(right - 16, 32, 12, f"{index:02}"),
    ]
    return ("\n".join(commands) + "\n").encode("ascii")


def save_objects(path, objects):
    pdf = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, obj in enumerate(objects, start=1):
        offsets.append(len(pdf))
        pdf.extend(f"{number} 0 obj\n".encode() + obj + b"\nendobj\n")
    xref = len(pdf)
    pdf.extend(f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode())
    for offset in offsets:
        pdf.extend(f"{offset:010} 00000 n \n".encode())
    pdf.extend(f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode())
    path.write_bytes(pdf)


def write_selection_fixture(path):
    # Point coordinates and font fix the locations used in the native clipboard test.
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R] /Count 3 >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    ]
    first = "BT /F1 24 Tf 80 720 Td (One café naïve €) Tj 0 -38 Td (Two lines of text) Tj 0 -38 Td (Three anchors) Tj ET"
    second = "BT /F1 24 Tf 80 720 Td (Rotated page) Tj ET"
    # The last page has one inline RGB image with 12 rows and no text at all.
    stripes = ["d5e1c1", "cbb6a3", "819597", "d6c9b9"] * 3
    image = "q 600 0 0 1200 0 0 cm BI /W 1 /H 12 /CS /RGB /BPC 8 /F /AHx ID\n" + "".join(stripes) + ">\nEI Q"
    for height, rotation, text in [(900, 0, first), (900, 90, second), (1200, 0, image)]:
        content_id = len(objects) + 2
        objects.append((f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 {height}] "
                        f"/Rotate {rotation} /Resources << /Font << /F1 3 0 R >> >> "
                        f"/Contents {content_id} 0 R >>").encode())
        content = text.encode("cp1252") + b"\n"
        objects.append(f"<< /Length {len(content)} >>\nstream\n".encode() + content + b"endstream")
    save_objects(path, objects)


def write_fixture(path, tall_strip=False):
    pages = [(480, 720, "Ridge lines", "01   /   Portrait study"),
             (720, 480, "Open horizon", "02   /   Landscape study"),
             (360, 1200, "River descent", "03   /   Scroll to the lower margin")]
    if tall_strip:
        pages[2] = (50, 12000, "Strip", "1 to 240")
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R] /Count 3 >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    for index, (width, height, title, subtitle) in enumerate(pages, start=1):
        content_id = len(objects) + 2
        objects.append(
            (f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {width} {height}] "
             f"/Resources << /Font << /F1 3 0 R >> >> /Contents {content_id} 0 R >>").encode()
        )
        content = page_stream(width, height, index, title, subtitle)
        objects.append(f"<< /Length {len(content)} >>\nstream\n".encode() + content + b"endstream")

    save_objects(path, objects)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--tall-strip", action="store_true", help="use a 1:240 third page")
    mode.add_argument("--selection", action="store_true", help="make a Unicode, rotated and image-only document")
    args = parser.parse_args()
    if args.selection:
        write_selection_fixture(args.output)
    else:
        write_fixture(args.output, args.tall_strip)
