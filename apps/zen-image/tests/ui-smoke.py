#!/usr/bin/env python3
"""Verify displayed pixels under a real GTK window. Pass the zen-image executable."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from PIL import Image, ImageDraw, ImageGrab


def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], timeout=8, text=True).strip()


def wait(check, label):
    end = time.monotonic() + 5
    while time.monotonic() < end:
        if check():
            return
        time.sleep(.04)
    raise AssertionError(label)


def run(binary, tmp):
    w, h = 513, 257  # RGB stride includes padding.
    red, green, blue, yellow = (185, 49, 45), (56, 150, 78), (42, 75, 172), (232, 196, 63)
    src = Image.new("RGB", (w, h), red)
    draw = ImageDraw.Draw(src)
    draw.rectangle((w//2, 0, w-1, h//2-1), fill=green)
    draw.rectangle((0, h//2, w//2-1, h-1), fill=blue)
    draw.rectangle((w//2, h//2, w-1, h-1), fill=yellow)
    png, webp, jpg, broken = [tmp / s for s in ["odd.png", "odd.webp", "oriented.jpg", "broken.png"]]
    src.save(png)
    src.save(webp, lossless=True)
    exif = Image.Exif()
    exif[274] = 6
    exif[271] = "Zen Camera"
    exif[272] = "Navigation Test"
    exif[306] = "2026:11:06 12:00:00"
    src.save(jpg, quality=95, exif=exif)
    broken.write_bytes(b"this is not an image")
    app = subprocess.Popen([binary, png])
    try:
        win = x("search", "--sync", "--onlyvisible", "--name", "odd.png").splitlines()[0]
        x("windowfocus", win)
        x("windowsize", "--sync", win, 600, 500)
        x("mousemove", 1100, 900)
        geo = dict(s.split("=") for s in x("getwindowgeometry", "--shell", win).splitlines())
        ox, oy = int(geo["X"]), int(geo["Y"])

        def color(px, py, expected):
            rgb = ImageGrab.grab(bbox=(ox+px, oy+py, ox+px+1, oy+py+1),
                                 xdisplay=os.environ["DISPLAY"]).convert("RGB").getpixel((0, 0))
            return sum(abs(a-b) for a,b in zip(rgb,expected)) < 22

        def load(path):
            x("key", "ctrl+o")
            chooser = x("search", "--sync", "--onlyvisible", "--name", "^Open").splitlines()[-1]
            x("windowfocus", chooser)
            subprocess.run(["xclip", "-selection", "clipboard", "-i", "-target", "UTF8_STRING"],
                           input=os.fsencode(path), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            x("key", "ctrl+l", "ctrl+a", "ctrl+v")
            time.sleep(.2)
            x("key", "Return")
            wait(lambda: chooser not in subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^Open"],
                                                       capture_output=True, text=True).stdout.splitlines(),
                 "File chooser did not close")
            x("windowfocus", win)

        ambient = (22,24,24)
        wait(lambda: color(5,170,red) and color(5,280,blue) and color(5,75,ambient), "PNG fit and margins")
        x("key", "1")
        wait(lambda: color(5,170,ambient) and color(55,170,red), "Actual physical pixel sizing")
        x("key", "p", "r")
        wait(lambda: color(200,130,blue) and color(390,130,red) and color(100,130,ambient), "Clockwise rotation")
        x("key", "w")
        wait(lambda: color(100,130,blue), "Fit-width rotated image")
        x("key", "p", "r", "r", "r")
        wait(lambda: color(5,170,red), "Four turns restore layout")
        x("key", "plus", "plus", "plus")
        wait(lambda: color(350,200,green), "Zoom in")
        x("mousemove", "--window", win, 400, 250, "mousedown", 1, "sleep", ".1",
          "mousemove", "--window", win, 520, 250, "sleep", ".1", "mouseup", 1)
        wait(lambda: color(350,200,red), "Mouse pan moves center boundary")
        x("key", "p")
        x("mousemove", "--window", win, 120, 130, "keydown", "Control_L", "click", 4, "keyup", "Control_L")
        wait(lambda: color(310,180,red), "Ctrl+wheel zooms around the pointer")
        print("PASS PNG, fit, 1:1 pixels, rotation, wheel zoom and pan", flush=True)

        load(broken)
        wait(lambda: png.name in x("getwindowname",win) and color(160,175,red), "Error should keep the current image")
        load(webp)
        wait(lambda: webp.name in x("getwindowname",win) and color(120,175,red), "Load WebP after failed decode")
        load(jpg)
        wait(lambda: jpg.name in x("getwindowname",win) and color(200,130,blue) and color(390,130,red),
             "JPEG EXIF orientation")
        print("PASS chooser error recovery, WebP and JPEG EXIF orientation", flush=True)

        # Metadata is opt-in and copyable. Starting from the rotated JPEG catches
        # the distinction between source orientation and displayed dimensions.
        x("key", "i", "ctrl+a", "ctrl+c")
        def metadata():
            return subprocess.check_output(["xclip", "-selection", "clipboard", "-o", "-target", "UTF8_STRING"],
                                           timeout=3, text=True)
        info = metadata()
        assert "oriented.jpg" in info and "257 × 513 pixels" in info and "File size" in info, info
        if os.environ.get("EXPECT_EXIF") == "1":
            assert "Zen Camera" in info and "Navigation Test" in info, info
        x("key", "i")
        wait(lambda: color(390,130,red), "Metadata toggle hides panel")
        print("PASS hidden-by-default metadata, original dimensions and native clipboard", flush=True)

        album = tmp / "album"
        album.mkdir()
        for name, paint in [("image1.PNG", red), ("image2.jpg", green), ("image4.bmp", blue), ("image10.tiff", yellow)]:
            Image.new("RGB", (300,200), paint).save(album / name)
        (album / "image3.png").write_bytes(b"unreadable")
        (album / "notes.txt").write_text("Not an image")
        (album / "image9.png").mkdir()
        load(album / "image1.PNG")
        def named(name):
            return name in x("getwindowname", win)
        wait(lambda: named("image1.PNG") and color(300,200,red), "Open an album")
        x("key", "Right")
        wait(lambda: named("image2.jpg") and color(300,200,green), "Natural order image1 -> image2")
        x("key", "greater")
        wait(lambda: named("image4.bmp") and color(300,200,blue), "Skip corrupt image3 and open BMP")
        x("key", "Right")
        wait(lambda: named("image10.tiff") and color(300,200,yellow), "Natural order image4 -> image10 and TIFF")
        x("key", "Right")
        time.sleep(.12)
        assert named("image10.tiff"), "Next should stop at last image"
        x("key", "less")
        wait(lambda: named("image4.bmp"), "Less-than goes back")
        x("key", "Left", "Left")
        wait(lambda: named("image1.PNG") and color(300,200,red), "Rapid navigation ignores obsolete results")
        x("key", "Left")
        time.sleep(.12)
        assert named("image1.PNG"), "Previous should stop at first image"
        print("PASS natural folder order, < > and arrow navigation, corrupt file skipping, mixed formats and boundaries", flush=True)

        # At minimum width all seven toolbar actions remain reachable. These
        # points are within the compact buttons across the supported GTK themes.
        x("windowsize", "--sync", win, 340, 500)
        x("mousemove", "--window", win, 100, 472)
        time.sleep(.25)
        x("click", 1)
        wait(lambda: named("image2.jpg"), "Compact next button")
        x("mousemove", "--window", win, 30, 472, "click", 1)
        wait(lambda: named("image1.PNG"), "Compact previous button")
        x("mousemove", "--window", win, 310, 472, "click", 1)
        x("key", "ctrl+a", "ctrl+c")
        assert "image1.PNG" in metadata(), "Compact metadata button"
        x("key", "Escape")
        x("windowsize", "--sync", win, 600, 500)
        x("mousemove", 1100, 900)
        wait(lambda: color(390,200,red), "Escape closes metadata")
        print("PASS compact navigation and metadata buttons, Escape dismissal", flush=True)

        # Additional loaders use the same decoding path; still images are
        # sufficient here because animation is intentionally not implemented.
        extra = tmp / "formats"
        extra.mkdir()
        for extension in ("gif", "ico", "ppm"):
            image = extra / f"still.{extension}"
            # DIB-backed ICO works with both legacy GdkPixbuf and newer loaders.
            # Some older loaders cannot read PNG-compressed icon entries.
            options = {"bitmap_format": "bmp", "sizes": [(64,64)]} if extension == "ico" else {}
            Image.new("RGBA" if extension == "ico" else "RGB", (64,64), blue).save(image, **options)
            load(image)
            wait(lambda: named(image.name) and color(300,200,blue), f"Decode {extension}")
        installed = subprocess.check_output([binary, "--formats"], text=True).lower()
        if "svg" in installed:
            image = extra / "vector.svg"
            image.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="#2a4bac"/></svg>')
            load(image)
            wait(lambda: named(image.name) and color(300,200,blue), "Decode SVG")
        if os.environ.get("ZEN_TEST_EXTRA_FORMATS") == "1":
            source = extra / "source.png"
            Image.new("RGB", (64,64), blue).save(source)
            for extension, encoder in (("avif", "libaom-av1"), ("jxl", "libjxl")):
                assert extension in installed, f"Install a {extension} GdkPixbuf loader first"
                image = extra / f"still.{extension}"
                subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", source,
                                "-frames:v", "1", "-c:v", encoder, "-threads", "1", image], check=True, timeout=30)
                load(image)
                wait(lambda: named(image.name) and color(300,200,blue), f"Decode {extension}")
        print("PASS additional installed image loaders", flush=True)
        x("key", "ctrl+w")
        assert app.wait(timeout=5) == 0
    finally:
        if app.poll() is None:
            app.terminate()
            app.wait(timeout=4)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("Usage: ui-smoke.py /path/to/zen-image")
    binary = Path(sys.argv[1]).resolve(strict=True)
    if "--display-ready" in sys.argv:
        os.environ.update(GDK_DEBUG="no-portals", GTK_A11Y="none", GDK_BACKEND="x11", GSK_RENDERER="cairo",
                          GSETTINGS_BACKEND="memory", G_DEBUG="fatal-warnings", GIO_USE_VFS="local")
        with tempfile.TemporaryDirectory() as temp:
            run(binary, Path(temp))
    else:
        raise SystemExit(subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24", "dbus-run-session", "--",
                                         sys.executable, __file__, binary, "--display-ready"]).returncode)
